#include <intravenous/runtime/graph_executor.h>

#include <intravenous/node/layout.h>

#include <algorithm>
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

void GraphExecutor::BackgroundGeneration::close_inputs() noexcept
{
    for (auto& input : pending_inputs) input->close();
}

void GraphExecutor::BackgroundGeneration::select_inputs(
    BackgroundEvaluationRequest request)
{
    if (input_selection_active) return;
    for (std::size_t index = 0; index < pending_inputs.size(); ++index) {
        input_selections[index] = pending_inputs[index]->pin();
    }
    selected_request.emplace(std::move(request));
    input_selection_active = true;
}

void GraphExecutor::BackgroundGeneration::discard_empty_selection() noexcept
{
    if (!input_selection_active || has_selected_inputs()) return;
    selected_request.reset();
    input_selection_active = false;
}

bool GraphExecutor::BackgroundGeneration::has_selected_inputs() const noexcept
{
    return std::ranges::any_of(
        input_selections,
        [](PinnedBlockPrefix const& prefix) { return !prefix.empty(); });
}

bool GraphExecutor::BackgroundGeneration::inputs_closed_and_drained()
    const noexcept
{
    return std::ranges::all_of(
        pending_inputs,
        [](auto const& input) { return input->is_closed_and_drained(); });
}

bool GraphExecutor::BackgroundGeneration::release_closed_input_sentinels(
    ReleasedBlockQueue& released) noexcept
{
    return std::ranges::all_of(
        pending_inputs,
        [&](auto const& input) {
            return input->release_closed_sentinel(released);
        });
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
    static_assert(std::atomic<ExecutionGeneration*>::is_always_lock_free);
    for (std::size_t producer = 0;
         producer < realtime.producer_reserves.size(); ++producer) {
        realtime.tick_invocation.bind_producer_endpoint(
            producer,
            *realtime.producer_reserves[producer],
            *background.pending_inputs[producer],
            production_reservation_failed);
    }
}

void GraphExecutor::ExecutionGeneration::initialize_first_generation()
{
    realtime.storage.initialize();
    background.storage.initialize();
    realtime_initialized = true;
    background_initialized = true;
}

void GraphExecutor::ExecutionGeneration::prepare_migration_from(
    ExecutionGeneration& previous)
{
    if (realtime_initialized || background_initialized
        || realtime_migration || background_migration) {
        throw std::logic_error(
            "GraphExecutor generation migration is already prepared");
    }
    realtime_migration.emplace(
        realtime.storage.prepare_migration_from(previous.realtime.storage));
    background_migration.emplace(
        background.storage.prepare_migration_from(previous.background.storage));
}

void GraphExecutor::ExecutionGeneration::commit_realtime_migration()
{
    if (realtime_initialized || !realtime_migration) {
        throw std::logic_error(
            "GraphExecutor realtime generation migration is not prepared");
    }
    realtime_migration->commit();
    realtime_migration.reset();
    realtime_initialized = true;
}

void GraphExecutor::ExecutionGeneration::commit_background_migration()
{
    if (background_initialized || !background_migration) {
        throw std::logic_error(
            "GraphExecutor background generation migration is not prepared");
    }
    background_migration->commit();
    background_migration.reset();
    background_initialized = true;
}

void GraphExecutor::ExecutionGeneration::publish_successor(
    std::unique_ptr<ExecutionGeneration> successor)
{
    if (!successor || successor_owner
        || published_successor.load(std::memory_order_relaxed)) {
        throw std::logic_error(
            "GraphExecutor generation cannot publish this successor");
    }
    successor_owner = std::move(successor);
    published_successor.store(
        successor_owner.get(), std::memory_order_release);
}

GraphExecutor::ExecutionGeneration*
GraphExecutor::ExecutionGeneration::successor() const noexcept
{
    return published_successor.load(std::memory_order_acquire);
}

void GraphExecutor::ExecutionGeneration::prepare_initial_persisted_state(
    PersistedPageStore& pages)
{
    initial_persisted_state = RealtimePersistedState::prepare_initial(
        graph->project_generation, pages);
}

GraphExecutor::BackgroundExecutor::BackgroundExecutor(GraphExecutor& owner)
    : owner_(owner)
    , worker_([this](std::stop_token stop) { run(stop); })
{}

GraphExecutor::BackgroundExecutor::~BackgroundExecutor()
{
    {
        std::lock_guard lock{commands_mutex_};
        stopping_ = true;
    }
    worker_.request_stop();
    commands_changed_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void GraphExecutor::BackgroundExecutor::install_first_generation(
    std::unique_ptr<ExecutionGeneration> prepared)
{
    if (!prepared) {
        throw std::invalid_argument(
            "BackgroundExecutor cannot install an empty generation");
    }
    std::lock_guard lock{commands_mutex_};
    if (stopping_ || generation_chain_ || first_command_) {
        throw std::logic_error(
            "BackgroundExecutor cannot install this first generation");
    }
    current_generation_.store(
        prepared->graph->project_generation, std::memory_order_relaxed);
    generation_chain_ = std::move(prepared);
    has_generation_.store(true, std::memory_order_release);
}

std::optional<std::uint64_t>
GraphExecutor::BackgroundExecutor::current_generation() const noexcept
{
    if (!has_generation_.load(std::memory_order_acquire)) {
        return std::nullopt;
    }
    return current_generation_.load(std::memory_order_acquire);
}

GraphExecutor::ExecutionGeneration&
GraphExecutor::BackgroundExecutor::generation()
{
    if (!generation_chain_) {
        throw std::logic_error("BackgroundExecutor has no generation");
    }
    return *generation_chain_;
}

GraphExecutor::BackgroundExecutor::Result
GraphExecutor::BackgroundExecutor::evaluate(
    BackgroundEvaluationRequest request)
{
    Command command{.request = std::move(request)};
    {
        std::lock_guard lock{commands_mutex_};
        if (stopping_) {
            return std::unexpected("BackgroundExecutor is stopping");
        }
        if (!generation_chain_) {
            return std::unexpected("BackgroundExecutor has no generation");
        }
        if (last_command_) {
            last_command_->next = &command;
        } else {
            first_command_ = &command;
        }
        last_command_ = &command;
    }
    commands_changed_.notify_one();

    std::unique_lock lock{command.mutex};
    command.completed.wait(lock, [&] { return command.done; });
    return std::move(*command.result);
}

GraphExecutorReclaimedSnapshots
GraphExecutor::BackgroundExecutor::reclaim_snapshots()
{
    Command command{.reclaim_snapshots = true};
    {
        std::lock_guard lock{commands_mutex_};
        if (stopping_) return {};
        if (last_command_) {
            last_command_->next = &command;
        } else {
            first_command_ = &command;
        }
        last_command_ = &command;
    }
    commands_changed_.notify_one();

    std::unique_lock lock{command.mutex};
    command.completed.wait(lock, [&] { return command.done; });
    return *command.reclaimed;
}

void GraphExecutor::BackgroundExecutor::run(std::stop_token stop) noexcept
{
    for (;;) {
        Command* command = nullptr;
        {
            std::unique_lock lock{commands_mutex_};
            commands_changed_.wait(
                lock,
                stop,
                [&] { return stopping_ || first_command_ != nullptr; });
            if (!first_command_) {
                if (stopping_ || stop.stop_requested()) return;
                continue;
            }
            command = first_command_;
            first_command_ = command->next;
            if (!first_command_) last_command_ = nullptr;
            command->next = nullptr;
        }

        if (command->reclaim_snapshots) {
            auto reclaimed = [&] {
                try {
                    return owner_.reclaim_retired_snapshots_on_background();
                } catch (...) {
                    return GraphExecutorReclaimedSnapshots{};
                }
            }();
            std::lock_guard lock{command->mutex};
            command->reclaimed.emplace(reclaimed);
            command->done = true;
        } else {
            auto result = [&]() -> Result {
                try {
                    return execute(std::move(command->request));
                } catch (std::exception const& exception) {
                    return std::unexpected(exception.what());
                } catch (...) {
                    return std::unexpected(
                        "BackgroundExecutor encountered an unknown failure");
                }
            }();
            std::lock_guard lock{command->mutex};
            command->result.emplace(std::move(result));
            command->done = true;
        }
        command->completed.notify_one();
    }
}

GraphExecutor::GraphExecutor(
    ResourceContext resources,
    RealtimeProducerCapacityConfig producer_capacity)
    : resources_(std::move(resources))
    , producer_capacity_(producer_capacity)
    , background_executor_(*this)
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
GraphExecutor::realtime_execution_generation()
{
    if (!realtime_active_) {
        throw std::logic_error("GraphExecutor has no realtime generation");
    }
    return *realtime_active_;
}

GraphExecutor::ExecutionGeneration const&
GraphExecutor::realtime_execution_generation() const
{
    if (!realtime_active_) {
        throw std::logic_error("GraphExecutor has no realtime generation");
    }
    return *realtime_active_;
}

GraphExecutorStageResult GraphExecutor::stage(
    std::shared_ptr<CompiledGraph const> compiled_graph)
{
    if (!compiled_graph) {
        throw std::invalid_argument("GraphExecutor cannot stage an empty compiled graph");
    }
    auto const newest_generation = pending_generation_
        ? pending_generation_->graph->project_generation
        : realtime_active_ ? realtime_active_->graph->project_generation
                           : std::uint64_t{0};
    if ((realtime_active_ || pending_generation_)
        && compiled_graph->project_generation <= newest_generation) {
        return GraphExecutorStageResult::ignored_stale;
    }

    auto prepared = std::make_unique<ExecutionGeneration>(
        std::move(compiled_graph),
        resources_,
        async_capacity_manager_,
        production_reservation_failed_,
        producer_capacity_);
    prepared->prepare_initial_persisted_state(persisted_pages_);
    if (!realtime_active_) {
        prepared->initialize_first_generation();
    } else {
        prepared->prepare_migration_from(realtime_execution_generation());
    }
    pending_generation_ = std::move(prepared);
    return GraphExecutorStageResult::staged;
}

bool GraphExecutor::activate_pending()
{
    if (!pending_generation_) return false;
    if (!realtime_active_) {
        if (!pending_generation_->initial_persisted_state
            || !pending_generation_->initial_persisted_state->capture_initial()) {
            throw std::logic_error(
                "GraphExecutor initial persisted state was not prepared");
        }
        auto* first = pending_generation_.get();
        background_executor_.install_first_generation(
            std::move(pending_generation_));
        realtime_active_ = first;
        realtime_persisted_state_.activate_generation(
            std::move(realtime_active_->initial_persisted_state));
        return true;
    }

    auto& previous = realtime_execution_generation();
    if (!pending_generation_->initial_persisted_state
        || !pending_generation_->initial_persisted_state->capture_initial()) {
        throw std::logic_error(
            "GraphExecutor initial persisted state was not prepared");
    }
    pending_generation_->commit_realtime_migration();
    previous.background.close_inputs();
    auto* next = pending_generation_.get();
    realtime_persisted_state_.activate_generation(
        std::move(next->initial_persisted_state));
    previous.publish_successor(std::move(pending_generation_));
    realtime_active_ = next;
    return true;
}

std::optional<std::uint64_t> GraphExecutor::active_generation() const noexcept
{
    if (!realtime_active_) return std::nullopt;
    return realtime_active_->graph->project_generation;
}

std::optional<std::uint64_t> GraphExecutor::pending_generation() const noexcept
{
    if (!pending_generation_) return std::nullopt;
    return pending_generation_->graph->project_generation;
}

std::optional<std::uint64_t>
GraphExecutor::background_generation() const noexcept
{
    return background_executor_.current_generation();
}

std::shared_ptr<CompiledGraph const> GraphExecutor::active_graph() const noexcept
{
    return realtime_active_ ? realtime_active_->graph : nullptr;
}

std::expected<BackgroundEvaluationResult, std::string>
GraphExecutor::evaluate_generation(
    ExecutionGeneration& generation,
    BackgroundEvaluationRequest request,
    bool publish_realtime_state)
{
    auto& background = generation.background;
    std::unique_ptr<RealtimePersistedState> published_state;
    if (publish_realtime_state) {
        // Every allocation/reader registration required by a coherent
        // realtime root happens before the domain transaction can commit.
        published_state = RealtimePersistedState::prepare(
            generation.graph->project_generation,
            persisted_pages_,
            tick_materializations_);
    }
    background.select_inputs(std::move(request));
    if (!background.selected_request) {
        return std::unexpected(
            "GraphExecutor background selection has no request");
    }
    BackgroundEvaluationTransaction transaction{
        *generation.graph,
        background.storage.buffer().data(),
        background.coverage,
        background.propagation,
        persisted_pages_,
        tick_materializations_,
        *background.selected_request,
        background.input_routes,
        background.input_selections,
    };
    auto result = transaction.execute();
    if (!result || result->status != BackgroundEvaluationStatus::committed) {
        // Preserve every selected producer-queue prefix exactly as pinned so a
        // retry cannot accidentally absorb later realtime publications.
        return result;
    }
    std::optional<std::string> persisted_state_error{};
    if (published_state) {
        if (auto captured = published_state->capture_current(); !captured) {
            persisted_state_error.emplace(std::move(captured.error()));
        } else {
            realtime_persisted_state_.publish(std::move(published_state));
        }
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
    background.selected_request.reset();
    background.input_selection_active = false;
    if (persisted_state_error) {
        return std::unexpected(std::move(*persisted_state_error));
    }
    return result;
}

bool GraphExecutor::BackgroundExecutor::advance_generation()
{
    auto& previous = generation();
    auto* successor = previous.successor();
    if (!successor) return false;
    if (previous.background.input_selection_active
        || !previous.background.inputs_closed_and_drained()) {
        return false;
    }
    if (successor != previous.successor_owner.get()) {
        throw std::logic_error(
            "GraphExecutor cutover ownership disagrees with publication");
    }
    if (!previous.background.release_closed_input_sentinels(
            owner_.async_capacity_manager_.released_blocks())) {
        throw std::logic_error(
            "GraphExecutor could not retire drained producer queues");
    }
    successor->commit_background_migration();

    auto retired = std::move(generation_chain_);
    generation_chain_ = std::move(retired->successor_owner);
    current_generation_.store(
        generation_chain_->graph->project_generation,
        std::memory_order_release);
    return true;
}

GraphExecutor::BackgroundExecutor::Result
GraphExecutor::BackgroundExecutor::execute(
    BackgroundEvaluationRequest request)
{
    for (;;) {
        auto& current = generation();
        if (!current.successor()) {
            return owner_.evaluate_generation(
                current, std::move(request), true);
        }

        if (current.background.input_selection_active) {
            auto drained = owner_.evaluate_generation(
                current, {}, false);
            if (!drained
                || drained->status != BackgroundEvaluationStatus::committed) {
                return drained;
            }
            continue;
        }

        current.background.select_inputs({
            .semantic_version = request.semantic_version,
            .page_width = request.page_width,
        });
        if (current.background.has_selected_inputs()) {
            auto drained = owner_.evaluate_generation(current, {}, false);
            if (!drained
                || drained->status != BackgroundEvaluationStatus::committed) {
                return drained;
            }
            continue;
        }
        current.background.discard_empty_selection();
        if (!current.background.inputs_closed_and_drained()) {
            return std::unexpected(
                "GraphExecutor cutover reached a producer queue that is not closed");
        }
        if (!advance_generation()) {
            return std::unexpected(
                "GraphExecutor could not advance a drained generation cutover");
        }
    }
}

std::expected<BackgroundEvaluationResult, std::string>
GraphExecutor::evaluate_background(BackgroundEvaluationRequest request)
{
    return background_executor_.evaluate(std::move(request));
}

GraphExecutorReclaimedSnapshots
GraphExecutor::reclaim_retired_snapshots_on_background()
{
    auto const states = realtime_persisted_state_.reclaim_returned();
    return {
        .realtime_persisted_states = states,
        .persisted_pages = persisted_pages_.reclaim_retired(),
        .tick_materializations = tick_materializations_.reclaim_retired(),
    };
}

GraphExecutorReclaimedSnapshots GraphExecutor::reclaim_retired_snapshots()
{
    return background_executor_.reclaim_snapshots();
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
    auto& generation = realtime_execution_generation();
    if (block_size == 0
        || block_size > generation.graph->specialization.block_size) {
        throw std::invalid_argument(
            "GraphExecutor tick block size is outside the compiled specialization");
    }
    auto const* persisted = realtime_persisted_state_.adopt(
        generation.graph->project_generation);
    if (!persisted) {
        throw std::logic_error(
            "GraphExecutor has no compatible realtime persisted state");
    }
    TickInvocationFrame invocation{
        *persisted,
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
