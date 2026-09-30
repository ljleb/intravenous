#include <intravenous/runtime/graph_executor.h>

#include <intravenous/node/layout.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>
#include <utility>

namespace iv {

class GraphExecutor::MaintenanceWorker {
    static constexpr std::uint32_t allocation_failure = 1u << 0;
    static constexpr std::uint32_t unexpected_failure = 1u << 1;
    static constexpr auto poll_interval = std::chrono::milliseconds{1};
    static constexpr auto allocation_retry_interval =
        std::chrono::milliseconds{100};

    GraphExecutor* executor_ = nullptr;
    std::mutex mutex_{};
    std::condition_variable wake_{};
    TickCaptureReservePolicy policy_{};
    std::uint64_t policy_revision_ = 0;
    std::atomic<std::uint32_t> failure_bits_{0};
    // Declared last so its destructor joins while every field used by run()
    // and the owning GraphExecutor is still alive.
    std::jthread thread_{};

    void run(std::stop_token stop) noexcept
    {
        std::uint64_t observed_revision = 0;
        bool allocation_backoff = false;
        for (;;) {
            TickCaptureReservePolicy policy;
            {
                std::unique_lock lock(mutex_);
                if (policy_revision_ == observed_revision) {
                    if (policy_.maximum_blocks_per_callback == 0) {
                        wake_.wait(lock, [&] {
                            return stop.stop_requested()
                                || policy_revision_ != observed_revision;
                        });
                    } else {
                        auto const failures = failure_bits_.load(
                            std::memory_order_acquire);
                        auto const interval = (
                            allocation_backoff
                            || (failures & unexpected_failure) != 0)
                            ? allocation_retry_interval
                            : poll_interval;
                        wake_.wait_for(lock, interval, [&] {
                            return stop.stop_requested()
                                || policy_revision_ != observed_revision;
                        });
                    }
                }
                if (stop.stop_requested()) return;
                policy = policy_;
                observed_revision = policy_revision_;
            }

            if ((failure_bits_.load(std::memory_order_acquire)
                    & unexpected_failure) == 0) {
                try {
                    static_cast<void>(
                        executor_->tick_captures_.maintain_free_block_reserve(
                            policy));
                    allocation_backoff = false;
                } catch (std::bad_alloc const&) {
                    // Retry slowly: reclamation may restore sufficient free
                    // capacity without another successful allocation.
                    failure_bits_.fetch_or(
                        allocation_failure, std::memory_order_release);
                    allocation_backoff = true;
                } catch (...) {
                    // No exception may escape a std::jthread entry point. Policy
                    // construction and bounds are validated synchronously, so
                    // an unexpected failure disables further allocation while
                    // leaving reclamation alive.
                    failure_bits_.fetch_or(
                        unexpected_failure, std::memory_order_release);
                }
            }

            // Reclamation is independent of replenishment and remains useful
            // after an allocation failure.
            static_cast<void>(executor_->tick_captures_.reclaim_committed());
        }
    }

public:
    explicit MaintenanceWorker(GraphExecutor& executor)
        : executor_(&executor)
        , thread_([this](std::stop_token stop) { run(stop); })
    {}

    ~MaintenanceWorker()
    {
        thread_.request_stop();
        wake_.notify_all();
    }

    void publish(TickCaptureReservePolicy policy)
    {
        {
            std::scoped_lock lock(mutex_);
            policy_ = policy;
            ++policy_revision_;
        }
        wake_.notify_one();
    }

    [[nodiscard]] TickCaptureMaintenanceFailures failures() const noexcept
    {
        auto const bits = failure_bits_.load(std::memory_order_acquire);
        return {
            .allocation_failed = (bits & allocation_failure) != 0,
            .unexpected_failure = (bits & unexpected_failure) != 0,
        };
    }
};

namespace {

ProducerCapacityPolicy producer_capacity_policy(
    std::size_t maximum_blocks_per_callback,
    TickCaptureAllocatorConfig const& config)
{
    if (maximum_blocks_per_callback == 0) return {};
    if (maximum_blocks_per_callback
            > std::numeric_limits<std::size_t>::max()
                / config.high_watermark_callbacks) {
        throw std::length_error(
            "Realtime producer capacity watermarks are too large");
    }
    return {
        .maximum_burst = maximum_blocks_per_callback,
        .low_watermark = maximum_blocks_per_callback
            * config.low_watermark_callbacks,
        .high_watermark = maximum_blocks_per_callback
            * config.high_watermark_callbacks,
    };
}

} // namespace

GraphExecutor::RealtimeGeneration::RealtimeGeneration(
    CompiledGraph const& graph,
    ResourceContext const& resources,
    PersistedTickCaptureRegistry& captures,
    AsyncCapacityManager& capacity_manager,
    TickCaptureAllocatorConfig const& capacity_policy)
    : storage(graph.node_layout.create_storage(resources))
    , tick_invocation(
        graph.background_evaluation_plan,
        graph.project_generation,
        graph.specialization.block_size,
        &captures)
{
    auto const requirements = tick_invocation.capture_producer_requirements();
    producer_reserves.reserve(requirements.size());
    capacity_registrations.reserve(requirements.size());
    for (auto const& requirement : requirements) {
        auto reserve = std::make_unique<ProducerReserve>(
            GraphExecutor::tick_capture_payload_capacity);
        auto registration = capacity_manager.register_producer(
            *reserve,
            producer_capacity_policy(
                requirement.maximum_blocks_per_callback, capacity_policy));
        producer_reserves.push_back(std::move(reserve));
        capacity_registrations.push_back(std::move(registration));
    }
}

GraphExecutor::BackgroundGeneration::BackgroundGeneration(
    CompiledGraph const& graph,
    ResourceContext const& resources,
    RealtimeGeneration& realtime)
    : storage(graph.node_layout.create_storage(resources))
    , coverage(graph.background_evaluation_plan.accumulators.output_change_count)
    , propagation(
        graph.background_evaluation_plan,
        graph.specialization.sample_rate)
{
    auto const requirements =
        realtime.tick_invocation.capture_producer_requirements();
    pending_inputs.reserve(requirements.size());
    input_routes.reserve(requirements.size());
    input_selections.resize(requirements.size());
    for (std::size_t index = 0; index < requirements.size(); ++index) {
        auto pending = std::make_unique<PendingQueue>(
            *realtime.producer_reserves[index]);
        input_routes.push_back({
            .queue = pending.get(),
            .output = requirements[index].output,
            .kind = requirements[index].kind,
        });
        pending_inputs.push_back(std::move(pending));
    }
}

GraphExecutor::ExecutionGeneration::ExecutionGeneration(
    std::shared_ptr<CompiledGraph const> compiled_graph,
    ResourceContext const& resources,
    PersistedTickCaptureRegistry& captures,
    AsyncCapacityManager& capacity_manager,
    TickCaptureAllocatorConfig const& capacity_policy)
    : graph(std::move(compiled_graph))
    , realtime(
        *graph, resources, captures, capacity_manager, capacity_policy)
    , background(*graph, resources, realtime)
{}

void GraphExecutor::ExecutionGeneration::initialize()
{
    realtime.storage.initialize();
    background.storage.initialize();
    initialized = true;
}

void GraphExecutor::ExecutionGeneration::migrate_from(
    ExecutionGeneration& previous)
{
    auto realtime_migration = realtime.storage.migration_from(
        previous.realtime.storage);
    auto background_migration = background.storage.migration_from(
        previous.background.storage);
    realtime_migration.commit();
    background_migration.commit();
    initialized = true;
}

GraphExecutor::GraphExecutor(
    ResourceContext resources,
    TickCaptureAllocatorConfig tick_capture_allocator)
    : resources_(std::move(resources))
    , tick_capture_allocator_(tick_capture_allocator)
    , tick_page_reader_(persisted_pages_.register_reader())
    , tick_materialization_reader_(tick_materializations_.register_reader())
{
    if (tick_capture_allocator_.low_watermark_callbacks == 0
        || tick_capture_allocator_.high_watermark_callbacks
            <= tick_capture_allocator_.low_watermark_callbacks
        || tick_capture_allocator_.slab_allocation_granularity == 0) {
        throw std::invalid_argument(
            "GraphExecutor has an invalid Tick capture allocator policy");
    }
    maintenance_ = std::make_unique<MaintenanceWorker>(*this);
}

GraphExecutor::~GraphExecutor() = default;

GraphExecutor::ExecutionGeneration&
GraphExecutor::active_execution_generation()
{
    if (!active_) throw std::logic_error("GraphExecutor has no active generation");
    return *generations_[*active_];
}

GraphExecutor::ExecutionGeneration const&
GraphExecutor::active_execution_generation() const
{
    if (!active_) throw std::logic_error("GraphExecutor has no active generation");
    return *generations_[*active_];
}

std::size_t GraphExecutor::maximum_capture_blocks_per_callback() const noexcept
{
    std::size_t maximum = 0;
    if (active_) {
        maximum = generations_[*active_]
            ->realtime.tick_invocation.maximum_capture_blocks_per_callback();
    }
    if (pending_) {
        maximum = std::max(
            maximum,
            generations_[*pending_]
                ->realtime.tick_invocation.maximum_capture_blocks_per_callback());
    }
    return maximum;
}

TickCaptureReservePolicy GraphExecutor::tick_capture_reserve_policy(
    std::size_t maximum_blocks_per_callback) const
{
    if (maximum_blocks_per_callback == 0) {
        return {
            .slab_allocation_granularity =
                tick_capture_allocator_.slab_allocation_granularity,
        };
    }
    if (maximum_blocks_per_callback
            > std::numeric_limits<std::size_t>::max()
                / tick_capture_allocator_.high_watermark_callbacks) {
        throw std::length_error(
            "Tick capture allocator watermarks are too large");
    }
    return {
        .maximum_blocks_per_callback = maximum_blocks_per_callback,
        .low_watermark = maximum_blocks_per_callback
            * tick_capture_allocator_.low_watermark_callbacks,
        .high_watermark = maximum_blocks_per_callback
            * tick_capture_allocator_.high_watermark_callbacks,
        .slab_allocation_granularity =
            tick_capture_allocator_.slab_allocation_granularity,
    };
}

void GraphExecutor::publish_tick_capture_maintenance_policy()
{
    maintenance_->publish(tick_capture_reserve_policy(
        maximum_capture_blocks_per_callback()));
}

GraphExecutorStageResult GraphExecutor::stage(
    std::shared_ptr<CompiledGraph const> compiled_graph)
{
    if (!compiled_graph) {
        throw std::invalid_argument("GraphExecutor cannot stage an empty compiled graph");
    }
    auto const newest_generation = pending_
        ? generations_[*pending_]->graph->project_generation
        : active_ ? generations_[*active_]->graph->project_generation
                  : std::uint64_t{0};
    if ((active_ || pending_)
        && compiled_graph->project_generation <= newest_generation) {
        return GraphExecutorStageResult::ignored_stale;
    }

    auto const index = active_ ? 1 - *active_ : std::size_t{0};
    pending_.reset();
    generations_[index].emplace(
        std::move(compiled_graph),
        resources_,
        persisted_tick_captures_,
        async_capacity_manager_,
        tick_capture_allocator_);
    auto const maximum_blocks_per_callback = std::max(
        active_ ? active_execution_generation().realtime.tick_invocation
                      .maximum_capture_blocks_per_callback()
                : std::size_t{0},
        generations_[index]->realtime.tick_invocation
            .maximum_capture_blocks_per_callback());
    static_cast<void>(
        tick_captures_.maintain_free_block_reserve(
            tick_capture_reserve_policy(maximum_blocks_per_callback)));
    if (!active_) {
        generations_[index]->initialize();
    }
    pending_ = index;
    publish_tick_capture_maintenance_policy();
    return GraphExecutorStageResult::staged;
}

bool GraphExecutor::activate_pending()
{
    if (!pending_) return false;
    auto& next = *generations_[*pending_];
    if (!next.initialized) {
        if (active_) {
            next.migrate_from(active_execution_generation());
        } else {
            next.initialize();
        }
    }
    auto const previous = active_;
    active_ = pending_;
    pending_.reset();
    if (previous) generations_[*previous].reset();
    publish_tick_capture_maintenance_policy();
    return true;
}

std::optional<std::uint64_t> GraphExecutor::active_generation() const noexcept
{
    if (!active_) return std::nullopt;
    return generations_[*active_]->graph->project_generation;
}

std::optional<std::uint64_t> GraphExecutor::pending_generation() const noexcept
{
    if (!pending_) return std::nullopt;
    return generations_[*pending_]->graph->project_generation;
}

std::shared_ptr<CompiledGraph const> GraphExecutor::active_graph() const noexcept
{
    return active_ ? generations_[*active_]->graph : nullptr;
}

std::expected<BackgroundEvaluationResult, std::string>
GraphExecutor::evaluate_background(BackgroundEvaluationRequest request)
{
    auto& generation = active_execution_generation();
    BackgroundEvaluationTransaction transaction{
        *generation.graph,
        generation.background.storage.buffer().data(),
        generation.background.coverage,
        generation.background.propagation,
        persisted_pages_,
        tick_materializations_,
        std::move(request),
    };
    return transaction.execute();
}

GraphExecutorReclaimedSnapshots GraphExecutor::reclaim_retired_snapshots()
{
    return {
        .persisted_pages = persisted_pages_.reclaim_retired(),
        .tick_materializations = tick_materializations_.reclaim_retired(),
    };
}

TickCaptureReservationFailures
GraphExecutor::tick_capture_reservation_failures() const noexcept
{
    return tick_captures_.reservation_failures();
}

TickCaptureMaintenanceFailures
GraphExecutor::tick_capture_maintenance_failures() const noexcept
{
    auto const capture = maintenance_->failures();
    auto const generic = async_capacity_manager_.failures();
    return {
        .allocation_failed =
            capture.allocation_failed || generic.allocation_failed,
        .unexpected_failure =
            capture.unexpected_failure || generic.unexpected_failure,
    };
}

void GraphExecutor::tick_block(std::size_t sample_index, std::size_t block_size)
{
    auto& generation = active_execution_generation();
    if (block_size == 0
        || block_size > generation.graph->specialization.block_size) {
        throw std::invalid_argument(
            "GraphExecutor tick block size is outside the compiled specialization");
    }
    TickInvocationFrame invocation{
        tick_page_reader_,
        tick_materialization_reader_,
        generation.realtime.tick_invocation,
        sample_index,
        block_size};
    generation.graph->root_operations.tick_block(
        generation.realtime.storage.buffer().data(),
        &invocation.call(),
        sample_index,
        block_size);
}

} // namespace iv
