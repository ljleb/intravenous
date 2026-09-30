#include <intravenous/runtime/graph_executor.h>

#include <intravenous/node/layout.h>

#include <atomic>
#include <limits>
#include <stdexcept>
#include <utility>

namespace iv {

namespace {

ProducerCapacityPolicy producer_capacity_policy(
    std::size_t maximum_blocks_per_callback,
    RealtimeProducerCapacityConfig const& config)
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
    AsyncCapacityManager& capacity_manager,
    RealtimeProducerCapacityConfig const& capacity_policy)
    : storage(graph.node_layout.create_storage(resources))
    , tick_invocation(
        graph.background_evaluation_plan,
        graph.project_generation,
        graph.specialization.block_size)
{
    auto const requirements = tick_invocation.producer_requirements();
    producer_reserves.reserve(requirements.size());
    capacity_registrations.reserve(requirements.size());
    for (auto const& requirement : requirements) {
        auto reserve = std::make_unique<ProducerReserve>(
            realtime_produced_block_storage_size);
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
        realtime.tick_invocation.producer_requirements();
    pending_inputs.reserve(requirements.size());
    input_routes.reserve(requirements.size());
    input_selections.resize(requirements.size());
    for (std::size_t index = 0; index < requirements.size(); ++index) {
        auto pending = std::make_unique<PendingQueue>(
            *realtime.producer_reserves[index]);
        input_routes.push_back({
            .output = requirements[index].output,
            .port = requirements[index].port,
            .kind = requirements[index].kind,
        });
        pending_inputs.push_back(std::move(pending));
    }
}

GraphExecutor::ExecutionGeneration::ExecutionGeneration(
    std::shared_ptr<CompiledGraph const> compiled_graph,
    ResourceContext const& resources,
    AsyncCapacityManager& capacity_manager,
    std::atomic<bool>& production_reservation_failed,
    RealtimeProducerCapacityConfig const& capacity_policy)
    : graph(std::move(compiled_graph))
    , realtime(
        *graph, resources, capacity_manager, capacity_policy)
    , background(*graph, resources, realtime)
{
    for (std::size_t producer = 0;
         producer < realtime.producer_reserves.size(); ++producer) {
        realtime.tick_invocation.bind_producer_endpoint(
            producer,
            *realtime.producer_reserves[producer],
            *background.pending_inputs[producer],
            production_reservation_failed);
    }
}

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
    RealtimeProducerCapacityConfig producer_capacity)
    : resources_(std::move(resources))
    , producer_capacity_(producer_capacity)
    , tick_page_reader_(persisted_pages_.register_reader())
    , tick_materialization_reader_(tick_materializations_.register_reader())
{
    if (producer_capacity_.low_watermark_callbacks == 0
        || producer_capacity_.high_watermark_callbacks
            <= producer_capacity_.low_watermark_callbacks) {
        throw std::invalid_argument(
            "GraphExecutor has an invalid realtime producer capacity policy");
    }
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
        async_capacity_manager_,
        production_reservation_failed_,
        producer_capacity_);
    if (!active_) {
        generations_[index]->initialize();
    }
    pending_ = index;
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
    auto& background = generation.background;
    if (!background.input_selection_active) {
        for (std::size_t index = 0;
             index < background.pending_inputs.size(); ++index) {
            background.input_selections[index] =
                background.pending_inputs[index]->pin();
        }
        background.input_selection_active = true;
    }
    BackgroundEvaluationTransaction transaction{
        *generation.graph,
        background.storage.buffer().data(),
        background.coverage,
        background.propagation,
        persisted_pages_,
        tick_materializations_,
        std::move(request),
        background.input_routes,
        background.input_selections,
    };
    auto result = transaction.execute();
    if (!result || result->status != BackgroundEvaluationStatus::committed) {
        // Preserve every selected producer-queue prefix exactly as pinned so a
        // retry cannot accidentally absorb later realtime publications.
        return result;
    }
    for (std::size_t index = 0;
         index < background.pending_inputs.size(); ++index) {
        if (background.input_selections[index].empty()) continue;
        auto const released = background.pending_inputs[index]->release(
            std::move(background.input_selections[index]),
            async_capacity_manager_.released_blocks());
        if (!released) {
            return std::unexpected(
                "committed background input prefix could not be released");
        }
    }
    background.input_selection_active = false;
    return result;
}

GraphExecutorReclaimedSnapshots GraphExecutor::reclaim_retired_snapshots()
{
    return {
        .persisted_pages = persisted_pages_.reclaim_retired(),
        .tick_materializations = tick_materializations_.reclaim_retired(),
    };
}

RealtimeProductionFailures
GraphExecutor::realtime_production_failures() const noexcept
{
    return {
        .insufficient_reserve_capacity =
            production_reservation_failed_.load(std::memory_order_acquire),
    };
}

RealtimeCapacityMaintenanceFailures
GraphExecutor::realtime_capacity_maintenance_failures() const noexcept
{
    auto const generic = async_capacity_manager_.failures();
    return {
        .allocation_failed = generic.allocation_failed,
        .unexpected_failure = generic.unexpected_failure,
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
