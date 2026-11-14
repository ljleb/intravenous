#include <intravenous/runtime/graph_executor.h>

#include <intravenous/node/layout.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
#include <limits>
#include <span>
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

// A replacement changes only the steady Tick operations, which run after the
// structural generation's transition phase and address only its steady
// NodeStorage prefix. So it is compared with that prefix: the structural
// generation's transition-only regions, a suffix of its layout, are not part of
// the canonical layout that dynamic and constant code share.
bool compatible_realtime_code(
    CompiledGraph const& structural,
    CompiledGraph const& replacement) noexcept
{
    auto const steady_regions = static_cast<std::size_t>(std::ranges::count_if(
        structural.node_layout.regions,
        [](NodeLayout::Region const& region) { return !region.transition_only; }));
    if (replacement.cutover
        || replacement.project_generation != structural.project_generation
        || replacement.definitions_generation
            != structural.definitions_generation
        || replacement.specialization.sample_rate
            != structural.specialization.sample_rate
        || replacement.specialization.block_size
            != structural.specialization.block_size
        || replacement.node_layout.storage_size
            != replacement.node_layout.steady_storage_size
        || replacement.node_layout.steady_storage_size
            != structural.node_layout.steady_storage_size
        // A transition-only region may raise the structural alignment; storage
        // aligned for it is aligned for the replacement too.
        || replacement.node_layout.storage_alignment == 0
        || structural.node_layout.storage_alignment
                % replacement.node_layout.storage_alignment
            != 0
        || replacement.node_layout.max_block_size
            != structural.node_layout.max_block_size
        || replacement.node_layout.default_silence_ttl_samples
            != structural.node_layout.default_silence_ttl_samples
        || replacement.node_layout.nodes.size()
            != structural.node_layout.nodes.size()
        || replacement.node_layout.regions.size() != steady_regions
        || replacement.node_layout.imported_arrays.size()
            != structural.node_layout.imported_arrays.size()
        || replacement.node_layout.exported_arrays.size()
            != structural.node_layout.exported_arrays.size()
        || replacement.node_layout.initialize_order
            != structural.node_layout.initialize_order
        || replacement.manual_controls.size()
            != structural.manual_controls.size()
        // TickInvocationWorkspace keeps these slots and capture reserves for
        // the structural generation. A source replacement may change them
        // even when its NodeStorage happens to have the same byte extent.
        || replacement.background_evaluation_plan.tick_runtime
            != structural.background_evaluation_plan.tick_runtime
        || !replacement.root_operations.valid()) {
        return false;
    }
    for (std::size_t i = 0; i < structural.node_layout.nodes.size(); ++i) {
        auto const& lhs = structural.node_layout.nodes[i];
        auto const& rhs = replacement.node_layout.nodes[i];
        if (lhs.node_type != rhs.node_type
            || lhs.migration_identity != rhs.migration_identity
            || lhs.state_structure != rhs.state_structure
            || lhs.state_offset != rhs.state_offset
            || lhs.state_size != rhs.state_size
            || lhs.state_alignment != rhs.state_alignment
            || lhs.dependencies != rhs.dependencies
            || lhs.lifecycle.move_fn != rhs.lifecycle.move_fn
            || lhs.lifecycle.initialize_fn != rhs.lifecycle.initialize_fn
            || lhs.lifecycle.release_fn != rhs.lifecycle.release_fn
            || lhs.lifecycle.default_construct_state_fn
                != rhs.lifecycle.default_construct_state_fn
            || lhs.lifecycle.destroy_state_fn
                != rhs.lifecycle.destroy_state_fn) {
            return false;
        }
    }
    auto owns_manual_control_state = [](
        CompiledGraph const& graph,
        NodeLayout::Region const& region) {
        return region.kind == NodeLayout::Region::Kind::raw
            && region.size == sizeof(ManualControlRuntimeState)
            && std::ranges::any_of(
                graph.manual_controls,
                [&](ManualControlRuntimeBinding const& binding) {
                    return binding.state_storage_offset
                        == region.storage_offset;
                });
    };
    // NodeLayoutTransitionBuilder appends transition-only regions after the
    // steady ones, so the steady regions are the first steady_regions entries.
    for (std::size_t i = 0; i < steady_regions; ++i) {
        auto const& lhs = structural.node_layout.regions[i];
        auto const& rhs = replacement.node_layout.regions[i];
        auto const lhs_manual = owns_manual_control_state(structural, lhs);
        auto const rhs_manual = owns_manual_control_state(replacement, rhs);
        if (lhs.kind != rhs.kind
            || lhs.owner_node != rhs.owner_node
            || lhs.storage_offset != rhs.storage_offset
            || lhs.size != rhs.size
            || lhs.alignment != rhs.alignment
            || lhs.state_field_offset != rhs.state_field_offset
            || lhs.element_count != rhs.element_count
            || lhs.transition_only != rhs.transition_only
            || lhs.migration_identity != rhs.migration_identity
            || lhs.raw_initialize_fn != rhs.raw_initialize_fn
            || lhs.element_type != rhs.element_type
            || lhs.assign_span_fn != rhs.assign_span_fn
            || lhs.nested_node_indices != rhs.nested_node_indices
            || lhs_manual != rhs_manual
            || (!lhs_manual
                && lhs.raw_initialize_data != rhs.raw_initialize_data)) {
            return false;
        }
    }
    auto arrays_match = [](auto const& lhs, auto const& rhs) {
        return lhs.owner_node == rhs.owner_node
            && lhs.id == rhs.id
            && lhs.state_field_offset == rhs.state_field_offset
            && lhs.element_type == rhs.element_type
            && lhs.element_size == rhs.element_size
            && lhs.assign_span_fn == rhs.assign_span_fn
            && lhs.read_span_fn == rhs.read_span_fn;
    };
    if (!std::ranges::equal(
            structural.node_layout.imported_arrays,
            replacement.node_layout.imported_arrays,
            arrays_match)
        || !std::ranges::equal(
            structural.node_layout.exported_arrays,
            replacement.node_layout.exported_arrays,
            arrays_match)) {
        return false;
    }
    for (std::size_t i = 0; i < structural.manual_controls.size(); ++i) {
        auto const& lhs = structural.manual_controls[i];
        auto const& rhs = replacement.manual_controls[i];
        if (lhs.configured.id != rhs.configured.id
            || lhs.configured.input != rhs.configured.input
            || lhs.configured.concrete_selection_index
                != rhs.configured.concrete_selection_index
            || lhs.configured.participation != rhs.configured.participation
            || lhs.state_storage_offset != rhs.state_storage_offset) {
            return false;
        }
    }
    return true;
}

bool write_manual_control_target(
    CompiledGraph const& graph,
    NodeStorage& node_storage,
    ManualControlId const& id,
    Sample value)
{
    auto storage = node_storage.buffer();
    auto const bits = std::bit_cast<std::uint32_t>(value.value);
    bool updated = false;
    for (auto const& binding : graph.manual_controls) {
        if (binding.configured.id != id) continue;
        auto const offset = binding.state_storage_offset
            + offsetof(ManualControlRuntimeState, target_bits);
        if (offset > storage.size()
            || sizeof(std::uint32_t) > storage.size() - offset
            || reinterpret_cast<std::uintptr_t>(storage.data() + offset)
                % alignof(std::uint32_t) != 0) {
            throw std::logic_error(
                "GraphExecutor manual-control state lies outside NodeStorage");
        }
        auto& target = *reinterpret_cast<std::uint32_t*>(
            storage.data() + offset);
        std::atomic_ref<std::uint32_t>(target).store(
            bits, std::memory_order_relaxed);
        updated = true;
    }
    return updated;
}

std::vector<PersistedOutputHandle> retired_persisted_outputs(
    CompiledGraph const& current,
    CompiledGraph const& previous,
    PersistedDataStore& data)
{
    std::vector<PersistedOutputHandle> result;
    auto const& current_plan = current.background_evaluation_plan;
    auto const& previous_plan = previous.background_evaluation_plan;
    for (graph_jit::PortStorageIndex previous_storage = 0;
         previous_storage < previous_plan.storage.ports.size();
         ++previous_storage) {
        auto const& prior = previous_plan.storage.ports[previous_storage];
        if (prior.storage != graph_jit::PortStorageKind::persisted_data) {
            continue;
        }
        auto const identity = persisted_output_id(
            previous_plan, previous_storage, previous.project_generation);
        auto const survives = [&] {
            for (graph_jit::PortStorageIndex current_storage = 0;
                 current_storage < current_plan.storage.ports.size();
                 ++current_storage) {
                auto const& candidate =
                    current_plan.storage.ports[current_storage];
                if (candidate.storage
                        != graph_jit::PortStorageKind::persisted_data
                    || !graph_jit::persisted_storage_is_compatible(
                        current_plan,
                        current_storage,
                        previous_plan,
                        previous_storage)) {
                    continue;
                }
                if (persisted_output_id(
                        current_plan,
                        current_storage,
                        current.project_generation)
                    == identity) {
                    return true;
                }
            }
            return false;
        }();
        if (survives) continue;
        auto const handle = data.resolve_output(identity);
        if (!std::ranges::contains(result, handle)) {
            result.push_back(handle);
        }
    }
    return result;
}

void append_tick_materialization_demands(
    graph_jit::BackgroundEvaluationPlan const& plan,
    IndexCoverage const& playback,
    CoveragePropagationRequest& roots)
{
    if (playback.empty()) return;
    for (auto const port_index : plan.tick_sequential_inputs) {
        if (port_index >= plan.ports.size()) {
            throw std::invalid_argument(
                "Tick materialization demand references a missing input port");
        }
        // Playback delivers only each callback's current block. Earlier
        // positions are the consumer's node-owned Sequential history.
        auto required = playback;
        auto existing = std::ranges::find(
            roots.input_demands, port_index, &InputCoverageRequest::port);
        if (existing == roots.input_demands.end()) {
            roots.input_demands.push_back({
                .port = port_index,
                .required = std::move(required),
            });
        } else {
            existing->required.include(required);
        }
    }
}

} // namespace

GraphExecutor::RealtimeGeneration::RealtimeGeneration(
    CompiledGraph const& graph,
    ResourceContext const& resources,
    PersistedDataStore& data,
    AsyncCapacityManager& capacity_manager,
    RealtimeProducerCapacityConfig const& capacity_policy)
    : storage(graph.node_layout.create_storage(resources))
    , tick_invocation(
        graph.background_evaluation_plan,
        graph.project_generation,
        data,
        graph.specialization.block_size,
        graph.root_operations.combine_sequential_input,
        graph.root_operations.initialize_sequential_input)
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
    RealtimeGeneration& realtime,
    AsyncWorkSignal& work_signal)
    : tock_states(graph.tock_state_layout.create_storage(resources))
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
            *realtime.producer_reserves[index], work_signal);
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

void GraphExecutor::BackgroundGeneration::select_inputs()
{
    if (input_selection_active) return;
    for (std::size_t index = 0; index < pending_inputs.size(); ++index) {
        input_selections[index] = pending_inputs[index]->pin();
    }
    input_selection_active = true;
}

void GraphExecutor::BackgroundGeneration::discard_empty_selection() noexcept
{
    if (!input_selection_active || has_selected_inputs()) return;
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
    PersistedDataStore& data,
    AsyncCapacityManager& capacity_manager,
    std::atomic<bool>& production_reservation_failed,
    RealtimeProducerCapacityConfig const& capacity_policy,
    AsyncWorkSignal& background_work_signal)
    : graph(std::move(compiled_graph))
    , realtime_code(graph.get())
    , active_realtime_code_owner(graph)
    , realtime(
        *graph, resources, data, capacity_manager, capacity_policy)
    , background(*graph, resources, realtime, background_work_signal)
{
    static_assert(std::atomic<ExecutionGeneration*>::is_always_lock_free);
    static_assert(
        std::atomic<CompiledGraphRealtimePhase>::is_always_lock_free);
    for (auto const& binding : graph->manual_controls) {
        if (!binding.configured.dynamic
            || std::ranges::any_of(manual_control_targets,
                [&](auto const& target) {
                    return target.first == binding.configured.id;
                })) {
            continue;
        }
        // The successor's desired value is authoritative, including undo
        // while a released knob stays dynamic. Later live target updates
        // replace this seed before or after activation in the usual way.
        manual_control_targets.emplace_back(
            binding.configured.id, binding.configured.settled_value);
    }
    if (graph->cutover) {
        auto const& expiry =
            graph->cutover->state_transition.sample_expiry_cursors;
        transition_cursor_progress.reserve(expiry.size());
        for (auto const& cursor : expiry) {
            if (cursor.value_count == 0
                || cursor.node_storage_offset
                    > realtime.storage.buffer().size()
                || sizeof(std::uint64_t)
                    > realtime.storage.buffer().size()
                        - cursor.node_storage_offset) {
                throw std::invalid_argument(
                    "GraphExecutor cutover has an invalid Sequential expiry cursor");
            }
            transition_cursor_progress.push_back({.expiry = cursor});
        }
    }
    for (std::size_t producer = 0;
         producer < realtime.producer_reserves.size(); ++producer) {
        realtime.tick_invocation.bind_producer_endpoint(
            producer,
            *realtime.producer_reserves[producer],
            *background.pending_inputs[producer],
            production_reservation_failed);
    }
}

// A first generation whose cutover starts from the empty graph writes its fresh
// values once its storage is initialized, before it is published to the audio
// thread, and starts in its transition phase when it has a transition root.
void GraphExecutor::ExecutionGeneration::initialize_first_generation()
{
    realtime.storage.initialize();
    if (graph->cutover) {
        if (!graph->cutover->from_empty_graph) {
            throw std::logic_error(
                "GraphExecutor first generation carries a predecessor cutover");
        }
        if ((!graph->cutover->state_transition.sample_expiry_cursors.empty()
                || graph->cutover->state_transition.event_expiry_samples != 0)
            && !graph->root_operations.transition_tick_block) {
            throw std::runtime_error(
                "GraphExecutor transition cutover has no transition root");
        }
        auto prepared = graph_jit::prepare_realtime_port_state_transfers(
            graph->cutover->state_transition, {}, realtime.storage.buffer());
        if (!prepared) {
            throw std::runtime_error(
                "GraphExecutor could not prepare the first generation's fresh values: "
                + std::move(prepared.error()));
        }
        graph_jit::execute_realtime_port_state_transfers(
            *prepared, {}, realtime.storage.buffer(), GlobalIndex{});
        auto const storage = realtime.storage.buffer();
        for (auto& progress : transition_cursor_progress) {
            std::memcpy(
                &progress.activation_index,
                storage.data() + progress.expiry.node_storage_offset,
                sizeof(progress.activation_index));
        }
        realtime_phase.store(
            transition_cursor_progress.empty()
                    && graph->cutover->state_transition.event_expiry_samples == 0
                ? CompiledGraphRealtimePhase::steady
                : CompiledGraphRealtimePhase::transition,
            std::memory_order_release);
    }
    realtime_initialized = true;
}

void GraphExecutor::ExecutionGeneration::initialize_first_background_generation()
{
    if (background_initialized || background_coverage_migration) {
        throw std::logic_error(
            "GraphExecutor first background generation is already initialized or has migration state");
    }
    background.tock_states.initialize();
    background_initialized = true;
}

void GraphExecutor::ExecutionGeneration::prepare_realtime_migration_from(
    ExecutionGeneration& previous)
{
    if (realtime_initialized || background_initialized
        || realtime_migration
        || realtime_port_state_transition) {
        throw std::logic_error(
            "GraphExecutor generation migration is already prepared");
    }
    if (graph->cutover
        && (!graph->cutover->state_transition.sample_expiry_cursors.empty()
            || graph->cutover->state_transition.event_expiry_samples != 0)
        && !graph->root_operations.transition_tick_block) {
        throw std::runtime_error(
            "GraphExecutor transition cutover has no transition root");
    }
    std::expected<graph_jit::RealtimePortStateTransitionPlan, std::string>
        transition = [&]()
        -> std::expected<graph_jit::RealtimePortStateTransitionPlan, std::string> {
        if (graph->cutover) {
            auto predecessor = graph->cutover->predecessor.lock();
            if (predecessor.get() != previous.graph.get()) {
                return std::unexpected(
                    "compiled cutover names a different active predecessor");
            }
            return graph->cutover->state_transition;
        }
        return graph_jit::plan_realtime_port_state_transition(
            previous.graph->realtime_port_state_requirements,
            previous.graph->realtime_port_state_realizations,
            graph->realtime_port_state_requirements,
            graph->realtime_port_state_realizations);
    }();
    if (!transition) {
        throw std::runtime_error(
            "GraphExecutor could not reconcile realtime port state: "
            + std::move(transition.error()));
    }
    auto prepared_transition =
        graph_jit::prepare_realtime_port_state_transfers(
            std::move(*transition),
            previous.realtime.storage.buffer(),
            realtime.storage.buffer());
    if (!prepared_transition) {
        throw std::runtime_error(
            "GraphExecutor could not prepare realtime port-state cutover: "
            + std::move(prepared_transition.error()));
    }
    realtime_port_state_transition.emplace(
        std::move(*prepared_transition));
    realtime_migration.emplace(
        realtime.storage.prepare_migration_from(previous.realtime.storage));
}

void GraphExecutor::ExecutionGeneration::prepare_background_migration_from(
    ExecutionGeneration& previous,
    PersistedDataStore& data)
{
    if (background_initialized || background_coverage_migration) {
        throw std::logic_error(
            "GraphExecutor background generation migration is already prepared");
    }
    background_coverage_migration.emplace(
        background.coverage.prepare_migration_from(
            previous.background.coverage,
            graph->background_evaluation_plan,
            previous.graph->background_evaluation_plan,
            graph->specialization.sample_rate
                != previous.graph->specialization.sample_rate));
    background.retired_outputs = retired_persisted_outputs(
        *graph, *previous.graph, data);
}

void GraphExecutor::ExecutionGeneration::commit_realtime_migration(
    GlobalIndex sample_index) noexcept
{
    IV_ASSERT(
        !realtime_initialized && realtime_migration
            && realtime_port_state_transition,
        "GraphExecutor realtime generation migration is not prepared");
    realtime_migration->commit();
    graph_jit::execute_realtime_port_state_transfers(
        *realtime_port_state_transition,
        realtime_migration->previous->buffer(),
        realtime.storage.buffer(),
        sample_index);
    realtime_migration.reset();
    if (!transition_cursor_progress.empty()
        || (graph->cutover
            && graph->cutover->state_transition.event_expiry_samples != 0)) {
        auto const storage = realtime.storage.buffer();
        for (auto& progress : transition_cursor_progress) {
            std::memcpy(
                &progress.activation_index,
                storage.data() + progress.expiry.node_storage_offset,
                sizeof(progress.activation_index));
        }
        realtime_phase.store(
            CompiledGraphRealtimePhase::transition,
            std::memory_order_release);
    } else {
        realtime_phase.store(
            CompiledGraphRealtimePhase::steady,
            std::memory_order_release);
    }
    realtime_initialized = true;
    for (auto const& [id, value] : manual_control_targets) {
        (void)write_manual_control_target(*graph, realtime.storage, id, value);
    }
}

void GraphExecutor::ExecutionGeneration::update_realtime_phase(
    std::size_t event_samples_advanced) noexcept
{
    if (realtime_phase.load(std::memory_order_relaxed)
            != CompiledGraphRealtimePhase::transition
        || !graph->cutover) {
        return;
    }
    transition_event_progress = event_samples_advanced
            > std::numeric_limits<std::uint64_t>::max()
                - transition_event_progress
        ? std::numeric_limits<std::uint64_t>::max()
        : transition_event_progress + event_samples_advanced;
    auto const storage = realtime.storage.buffer();
    auto const samples_complete = std::ranges::all_of(
        transition_cursor_progress,
        [&](auto const& progress) {
            std::uint64_t current = 0;
            std::memcpy(
                &current,
                storage.data() + progress.expiry.node_storage_offset,
                sizeof(current));
            return current - progress.activation_index
                >= progress.expiry.value_count;
        });
    auto const events_complete = transition_event_progress
        >= graph->cutover->state_transition.event_expiry_samples;
    if (samples_complete && events_complete) {
        realtime_phase.store(
            CompiledGraphRealtimePhase::steady,
            std::memory_order_release);
    }
}

void GraphExecutor::ExecutionGeneration::commit_background_migration()
{
    if (background_initialized || !background_coverage_migration) {
        throw std::logic_error(
            "GraphExecutor background generation migration is not prepared");
    }
    // TockState is non-semantic acceleration state. Each generation starts
    // fresh on the background worker instead of migrating it across code.
    background.tock_states.initialize();
    background_coverage_migration->commit();
    background.reconciliation_roots =
        background_coverage_migration->take_reconciliation_roots();
    background_coverage_migration.reset();
    background.reconciliation_required = true;
    background_initialized = true;
}

void GraphExecutor::ExecutionGeneration::attach_successor(
    std::unique_ptr<ExecutionGeneration> successor)
{
    if (!successor || successor_owner
        || published_successor.load(std::memory_order_relaxed)) {
        throw std::logic_error(
            "GraphExecutor generation cannot attach this successor");
    }
    successor_owner = std::move(successor);
}

void GraphExecutor::ExecutionGeneration::publish_attached_successor()
{
    if (!successor_owner
        || published_successor.load(std::memory_order_relaxed)) {
        throw std::logic_error(
            "GraphExecutor generation has no unpublished successor");
    }
    published_successor.store(
        successor_owner.get(), std::memory_order_release);
}

GraphExecutor::ExecutionGeneration*
GraphExecutor::ExecutionGeneration::successor() const noexcept
{
    return published_successor.load(std::memory_order_acquire);
}

void GraphExecutor::ExecutionGeneration::prepare_initial_persisted_state(
    PersistedDataStore& data)
{
    initial_persisted_state = RealtimePersistedState::prepare_initial(
        graph->project_generation, data);
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
    work_signal_.notify_control();
    if (worker_.joinable()) worker_.join();
}

AsyncWorkSignal& GraphExecutor::BackgroundExecutor::work_signal() noexcept
{
    return work_signal_;
}

void GraphExecutor::BackgroundExecutor::notify_generation_cutover() noexcept
{
    work_signal_.notify_work();
}

BackgroundExecutionFailures
GraphExecutor::BackgroundExecutor::failures() const noexcept
{
    return {
        .progress_failed =
            progress_failed_.load(std::memory_order_acquire),
    };
}

void GraphExecutor::BackgroundExecutor::install_initial_generation(
    std::unique_ptr<ExecutionGeneration> prepared)
{
    if (!prepared) {
        throw std::invalid_argument(
            "BackgroundExecutor cannot install an empty generation");
    }
    {
        std::lock_guard lock{commands_mutex_};
        if (stopping_ || generation_chain_ || first_command_) {
            throw std::logic_error(
                "BackgroundExecutor cannot install this first generation");
        }
        generation_chain_ = std::move(prepared);
    }
}

void GraphExecutor::BackgroundExecutor::activate_initial_generation(
    std::uint64_t generation) noexcept
{
    current_generation_.store(generation, std::memory_order_relaxed);
    has_generation_.store(true, std::memory_order_release);
    // Initialization belongs to the worker even when this graph has no
    // immediate producer input or reconciliation work. The generation owner
    // was installed during cold staging, so this publication takes no lock.
    work_signal_.notify_work();
}

void GraphExecutor::BackgroundExecutor::enqueue(Command& command)
{
    {
        std::lock_guard lock{commands_mutex_};
        if (stopping_) {
            throw std::runtime_error("BackgroundExecutor is stopping");
        }
        if (last_command_) {
            last_command_->next = &command;
        } else {
            first_command_ = &command;
        }
        last_command_ = &command;
    }
    work_signal_.notify_control();
}

void GraphExecutor::BackgroundExecutor::prepare_background_migration(
    ExecutionGeneration& target,
    ExecutionGeneration& source)
{
    Command command{
        .kind = CommandKind::prepare_background_migration,
        .migration_target = &target,
        .migration_source = &source,
    };
    enqueue(command);

    std::unique_lock lock{command.mutex};
    command.completed.wait(lock, [&] { return command.done; });
    if (command.exception) std::rethrow_exception(command.exception);
}

void GraphExecutor::BackgroundExecutor::set_tick_materialization_demand(
    ExecutionGeneration& target,
    IndexCoverage demand)
{
    Command command{
        .kind = CommandKind::set_tick_materialization_demand,
        .demand_target = &target,
        .materialization_demand = std::move(demand),
    };
    enqueue(command);

    std::unique_lock lock{command.mutex};
    command.completed.wait(lock, [&] { return command.done; });
    if (command.exception) std::rethrow_exception(command.exception);
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

GraphExecutor::ExecutionGeneration* GraphExecutor::find_accepted_generation(
    std::uint64_t project_generation) noexcept
{
    if (realtime_active_
        && realtime_active_->graph->project_generation == project_generation) {
        return realtime_active_;
    }
    for (auto* pending = pending_head_; pending;
         pending = pending->successor_owner.get()) {
        if (pending->graph->project_generation == project_generation) {
            return pending;
        }
    }
    return nullptr;
}

GraphExecutorReclaimedSnapshots
GraphExecutor::BackgroundExecutor::reclaim_snapshots()
{
    Command command{.kind = CommandKind::reclaim_snapshots};
    try {
        enqueue(command);
    } catch (...) {
        return {};
    }

    std::unique_lock lock{command.mutex};
    command.completed.wait(lock, [&] { return command.done; });
    return *command.reclaimed;
}

void GraphExecutor::BackgroundExecutor::run(std::stop_token stop) noexcept
{
    // Revision zero means no domain work has been consumed. The worker may be
    // scheduled for the first time after realtime has already published, so
    // sampling the current revision here would incorrectly discard that work.
    std::uint64_t observed_work_revision = 0;
    auto observed_wake_revision = work_signal_.wake_revision();
    for (;;) {
        Command* command = nullptr;
        {
            std::lock_guard lock{commands_mutex_};
            if (stopping_ || stop.stop_requested()) {
                // Run background lifecycle teardown on its owning worker.
                for (auto* generation = generation_chain_.get(); generation;
                     generation = generation->successor()) {
                    generation->background.tock_states.release();
                    generation->background.tock_states
                        .destroy_constructed_states();
                }
                return;
            }
            if (first_command_) {
                command = first_command_;
                first_command_ = command->next;
                if (!first_command_) last_command_ = nullptr;
                command->next = nullptr;
            }
        }

        if (!command) {
            auto const current_work_revision = work_signal_.work_revision();
            if (current_work_revision == observed_work_revision) {
                auto const current_wake_revision =
                    work_signal_.wake_revision();
                if (current_wake_revision != observed_wake_revision) {
                    observed_wake_revision = current_wake_revision;
                    continue;
                }
                work_signal_.wait(observed_wake_revision);
                continue;
            }
            observed_work_revision = current_work_revision;
            if (!generation_chain_) continue;

            auto progress = process_available_work_safely();
            if (!progress
                || progress->status
                    != BackgroundEvaluationStatus::committed) {
                progress_failed_.store(
                    true, std::memory_order_release);
                // A pre-commit failure keeps its exact selected queue prefixes.
                // Wait for a later domain-work notification rather than spin.
            }
            continue;
        }

        if (command->kind == CommandKind::reclaim_snapshots) {
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
        } else if (command->kind
                   == CommandKind::prepare_background_migration) {
            auto failure = [&]() -> std::exception_ptr {
                try {
                    if (!command->migration_target
                        || !command->migration_source) {
                        throw std::invalid_argument(
                            "BackgroundExecutor migration command is incomplete");
                    }
                    command->migration_target->prepare_background_migration_from(
                        *command->migration_source,
                        owner_.persisted_data_);
                    return {};
                } catch (...) {
                    return std::current_exception();
                }
            }();
            std::lock_guard lock{command->mutex};
            command->exception = std::move(failure);
            command->done = true;
        } else if (command->kind
                   == CommandKind::set_tick_materialization_demand) {
            auto failure = [&]() -> std::exception_ptr {
                try {
                    auto* selected = generation_chain_.get();
                    while (selected && selected != command->demand_target) {
                        selected = selected->successor();
                    }
                    if (!selected) {
                        throw std::logic_error(
                            "Tick materialization demand targets a retired generation");
                    }
                    auto& background = selected->background;
                    background.tick_materialization_demand =
                        std::move(command->materialization_demand);
                    auto const missing =
                        background.tick_materialization_demand
                        - background.materialized_tick_coverage;
                    background.tick_materialization_required =
                        !missing.empty()
                        && !selected->graph->background_evaluation_plan
                                .tick_sequential_inputs.empty();
                    if (background.tick_materialization_required) {
                        work_signal_.notify_work();
                    }
                    return {};
                } catch (...) {
                    return std::current_exception();
                }
            }();
            std::lock_guard lock{command->mutex};
            command->exception = std::move(failure);
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

GraphExecutor::~GraphExecutor()
{
    // A successor accepted after the installed chain but never attached also
    // owns PendingQueues referring to the background executor's work signal,
    // so retire it before that actor.
    pending_owner_.reset();
}

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

void GraphExecutor::lock_accepted_chain() noexcept
{
    while (accepted_chain_guard_.test_and_set(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

void GraphExecutor::unlock_accepted_chain() noexcept
{
    accepted_chain_guard_.clear(std::memory_order_release);
}

GraphExecutorStageResult GraphExecutor::stage(
    std::shared_ptr<CompiledGraph const> compiled_graph)
{
    struct Unlock {
        GraphExecutor* executor;
        ~Unlock() { executor->unlock_accepted_chain(); }
    };
    lock_accepted_chain();
    Unlock unlock{this};
    if (!compiled_graph) {
        throw std::invalid_argument("GraphExecutor cannot stage an empty compiled graph");
    }
    auto* predecessor = pending_tail_ ? pending_tail_ : realtime_active_;
    auto const newest_generation = predecessor
        ? predecessor->graph->project_generation : std::uint64_t{0};
    if (predecessor
        && compiled_graph->project_generation <= newest_generation) {
        return GraphExecutorStageResult::ignored_stale;
    }
    if (compiled_graph->cutover && compiled_graph->cutover->from_empty_graph) {
        if (predecessor) {
            throw std::invalid_argument(
                "GraphExecutor successor cannot start from the empty graph");
        }
    } else if (compiled_graph->cutover) {
        if (!predecessor) {
            throw std::invalid_argument(
                "GraphExecutor first generation cannot carry a predecessor cutover");
        }
        auto named_predecessor = compiled_graph->cutover->predecessor.lock();
        if (named_predecessor.get() != predecessor->graph.get()) {
            throw std::invalid_argument(
                "GraphExecutor compiled cutover does not name the accepted chain tail");
        }
        if (compiled_graph->cutover->predecessor_phase
            != CompiledGraphRealtimePhase::steady) {
            throw std::invalid_argument(
                "GraphExecutor ordered cutover must name the predecessor steady realization");
        }
    } else if (predecessor) {
        throw std::invalid_argument(
            "GraphExecutor successor does not carry its predecessor cutover");
    }

    auto prepared = std::make_unique<ExecutionGeneration>(
        std::move(compiled_graph),
        resources_,
        persisted_data_,
        async_capacity_manager_,
        production_reservation_failed_,
        producer_capacity_,
        background_executor_.work_signal());
    // Live gesture targets are resolved before the generation becomes visible,
    // so realtime copies through stable pointers and never searches.
    {
        auto const storage_size = prepared->realtime.storage.buffer().size();
        prepared->live_targets.reserve(prepared->graph->manual_controls.size());
        for (auto const& binding : prepared->graph->manual_controls) {
            auto const offset = binding.state_storage_offset
                + offsetof(ManualControlRuntimeState, target_bits);
            if (offset > storage_size
                || sizeof(std::uint32_t) > storage_size - offset
                || offset % alignof(std::uint32_t) != 0) {
                throw std::logic_error(
                    "GraphExecutor manual-control state lies outside NodeStorage");
            }
            auto target = live_target(binding.configured.id);
            auto const completion = target->completion_sequence.load(std::memory_order_acquire);
            prepared->live_targets.push_back({
                .target = std::move(target),
                .target_storage_offset = offset,
                .observed_completion = completion,
            });
        }
    }
    prepared->background.tick_materialization_demand =
        tick_materialization_demand_;
    prepared->background.tick_materialization_required =
        !tick_materialization_demand_.empty()
        && !prepared->graph->background_evaluation_plan
                .tick_sequential_inputs.empty();
    prepared->prepare_initial_persisted_state(persisted_data_);
    if (!predecessor) {
        prepared->initialize_first_generation();
    } else {
        prepared->prepare_realtime_migration_from(*predecessor);
        background_executor_.prepare_background_migration(
            *prepared, *predecessor);
    }
    auto* accepted = prepared.get();
    if (!predecessor) {
        background_executor_.install_initial_generation(std::move(prepared));
        pending_head_ = accepted;
    } else if (pending_tail_) {
        pending_tail_->attach_successor(std::move(prepared));
    } else {
        pending_owner_ = std::move(prepared);
        pending_head_ = pending_owner_.get();
    }
    pending_tail_ = accepted;
    accepted_tail_graph_ = accepted->graph;
    return GraphExecutorStageResult::accepted;
}

bool GraphExecutor::can_replace_realtime_code(
    CompiledGraph const& structural,
    CompiledGraph const& replacement) noexcept
{
    return compatible_realtime_code(structural, replacement);
}

GraphExecutorStageResult GraphExecutor::stage_realtime_code(
    std::shared_ptr<CompiledGraph const> compiled_graph)
{
    struct Unlock {
        GraphExecutor* executor;
        ~Unlock() { executor->unlock_accepted_chain(); }
    };
    lock_accepted_chain();
    Unlock unlock{this};
    if (!compiled_graph) {
        throw std::invalid_argument(
            "GraphExecutor cannot stage an empty realtime-code replacement");
    }
    auto* target = find_accepted_generation(
        compiled_graph->project_generation);
    if (!target) return GraphExecutorStageResult::ignored_stale;
    if (!compatible_realtime_code(*target->graph, *compiled_graph)) {
        throw std::invalid_argument(
            "GraphExecutor realtime-code replacement is not layout-compatible");
    }
    target->pending_realtime_code_owner = std::move(compiled_graph);
    return GraphExecutorStageResult::accepted;
}

bool GraphExecutor::set_manual_control_target(
    std::uint64_t project_generation,
    ManualControlId const& id,
    Sample value)
{
    struct Unlock {
        GraphExecutor* executor;
        ~Unlock() { executor->unlock_accepted_chain(); }
    };
    lock_accepted_chain();
    Unlock unlock{this};
    auto* generation = find_accepted_generation(project_generation);
    if (!generation) return false;
    if (!std::ranges::any_of(
            generation->graph->manual_controls,
            [&](ManualControlRuntimeBinding const& binding) {
                return binding.configured.id == id;
            })) {
        return false;
    }
    auto target = std::ranges::find(
        generation->manual_control_targets, id,
        [](auto const& entry) -> ManualControlId const& {
            return entry.first;
        });
    if (target == generation->manual_control_targets.end()) {
        generation->manual_control_targets.emplace_back(id, value);
    } else {
        target->second = value;
    }
    if (generation->realtime_initialized) {
        return write_manual_control_target(
            *generation->graph, generation->realtime.storage, id, value);
    }
    return true;
}

std::shared_ptr<ManualControlLiveTarget> GraphExecutor::live_target(
    ManualControlId const& id)
{
    std::scoped_lock lock(live_targets_mutex_);
    std::erase_if(live_targets_, [](auto const& entry) {
        return entry.second.expired();
    });
    auto found = std::ranges::find(
        live_targets_, id, [](auto const& entry) -> ManualControlId const& {
            return entry.first;
        });
    if (found != live_targets_.end()) {
        if (auto target = found->second.lock()) return target;
    }
    auto target = std::make_shared<ManualControlLiveTarget>();
    if (found != live_targets_.end()) found->second = target;
    else live_targets_.emplace_back(id, target);
    return target;
}

bool GraphExecutor::set_manual_control_target_in_accepted_chain(
    ManualControlId const& id, Sample value)
{
    auto update = [&](ExecutionGeneration& generation) {
        if (!std::ranges::any_of(
                generation.graph->manual_controls,
                [&](ManualControlRuntimeBinding const& binding) {
                    return binding.configured.id == id;
                })) {
            return false;
        }
        auto target = std::ranges::find(
            generation.manual_control_targets, id,
            [](auto const& entry) -> ManualControlId const& {
                return entry.first;
            });
        if (target == generation.manual_control_targets.end()) {
            generation.manual_control_targets.emplace_back(id, value);
        } else {
            target->second = value;
        }
        if (generation.realtime_initialized) {
            (void)write_manual_control_target(
                *generation.graph, generation.realtime.storage, id, value);
        }
        return true;
    };
    bool updated = realtime_active_ && update(*realtime_active_);
    for (auto* pending = pending_head_; pending;
         pending = pending->successor_owner.get()) {
        updated |= update(*pending);
    }
    return updated;
}

ProjectGraphGraphExecutorResult GraphExecutor::handle_project_graph_transaction(
    std::shared_ptr<CompiledGraph const> const& compiled_graph)
{
    auto const result = stage(compiled_graph);
    if (result != GraphExecutorStageResult::accepted) {
        throw std::logic_error(
            "ProjectGraph submitted a stale GraphExecutor generation");
    }
    return {
        .attempted = true,
        .accepted = true,
    };
}

void GraphExecutor::handle_project_graph_controls(
    ProjectGraphGraphExecutorControlsRequest& request)
{
    for (auto& target : request.targets) {
        auto const bits = std::bit_cast<std::uint32_t>(target.value.value);
        switch (target.gesture) {
        case ManualControlGesturePhase::none:
            request.target_applied |= set_manual_control_target(
                target.project_generation, target.id, target.value);
            break;
        case ManualControlGesturePhase::begin: {
            // Store before opening, so realtime never copies a stale value.
            target.live_target = live_target(target.id);
            target.live_target->target_bits.store(bits, std::memory_order_relaxed);
            target.live_target->open_gestures.fetch_add(1, std::memory_order_release);
            request.target_applied |= set_manual_control_target(
                target.project_generation, target.id, target.value);
            break;
        }
        case ManualControlGesturePhase::end: {
            // The committed value reaches the cell and every accepted
            // generation before the gesture closes, so the last realtime copy
            // and the settled targets agree.
            auto const live = live_target(target.id);
            live->target_bits.store(bits, std::memory_order_relaxed);
            {
                struct Unlock {
                    GraphExecutor* executor;
                    ~Unlock() { executor->unlock_accepted_chain(); }
                };
                lock_accepted_chain();
                Unlock unlock{this};
                request.target_applied |=
                    set_manual_control_target_in_accepted_chain(target.id, target.value);
            }
            auto open = live->open_gestures.load(std::memory_order_relaxed);
            while (open != 0 && !live->open_gestures.compare_exchange_weak(
                       open, open - 1, std::memory_order_release,
                       std::memory_order_relaxed)) {}
            live->completion_sequence.fetch_add(1, std::memory_order_release);
            break;
        }
        }
    }
    if (request.realtime_code) {
        request.realtime_code_result = stage_realtime_code(request.realtime_code);
    }
}

bool GraphExecutor::activate_pending(GlobalIndex sample_index)
{
    if (accepted_chain_guard_.test_and_set(std::memory_order_acquire)) {
        return false;
    }
    struct Unlock {
        GraphExecutor* executor;
        ~Unlock() { executor->unlock_accepted_chain(); }
    } unlock{this};
    auto activate_realtime_code = [&](ExecutionGeneration& generation) {
        generation.update_realtime_phase();
        if (!generation.pending_realtime_code_owner
            || generation.realtime_phase.load(std::memory_order_relaxed)
                != CompiledGraphRealtimePhase::steady) {
            return false;
        }
        generation.realtime_code.store(
            generation.pending_realtime_code_owner.get(),
            std::memory_order_release);
        generation.active_realtime_code_owner =
            std::move(generation.pending_realtime_code_owner);
        return true;
    };
    if (!pending_head_) {
        return realtime_active_
            && activate_realtime_code(*realtime_active_);
    }
    if (!realtime_active_) {
        if (!pending_head_->initial_persisted_state
            || !pending_head_->initial_persisted_state->capture_initial()) {
            throw std::logic_error(
                "GraphExecutor initial persisted state was not prepared");
        }
        auto* first = pending_head_;
        realtime_active_ = first;
        realtime_persisted_state_.activate_generation(
            std::move(realtime_active_->initial_persisted_state));
        background_executor_.activate_initial_generation(
            first->graph->project_generation);
        pending_head_ = first->successor_owner.get();
        if (!pending_head_) pending_tail_ = nullptr;
        (void)activate_realtime_code(*first);
        return true;
    }

    auto& previous = realtime_execution_generation();
    previous.update_realtime_phase();
    auto const previous_phase =
        previous.realtime_phase.load(std::memory_order_relaxed);
    auto const& cutover = pending_head_->graph->cutover;
    auto const cutover_is_current = cutover
        ? cutover->predecessor.lock().get() == previous.graph.get()
            && cutover->predecessor_phase == previous_phase
        : previous_phase == CompiledGraphRealtimePhase::steady;
    if (!cutover_is_current) {
        // The accepted successor remains queued until its named predecessor
        // realization becomes active. In particular, ProjectGraph may submit
        // a steady-predecessor cutover while the predecessor transition root
        // is still expiring.
        return false;
    }
    if (!pending_head_->initial_persisted_state
        || !pending_head_->initial_persisted_state->capture_initial()) {
        throw std::logic_error(
            "GraphExecutor initial persisted state was not prepared");
    }
    pending_head_->commit_realtime_migration(sample_index);
    previous.background.close_inputs();
    auto* next = pending_head_;
    realtime_persisted_state_.activate_generation(
        std::move(next->initial_persisted_state));
    if (pending_owner_) {
        previous.attach_successor(std::move(pending_owner_));
    }
    previous.publish_attached_successor();
    realtime_active_ = next;
    pending_head_ = next->successor_owner.get();
    if (!pending_head_) pending_tail_ = nullptr;
    background_executor_.notify_generation_cutover();
    (void)activate_realtime_code(*next);
    return true;
}

std::optional<std::uint64_t> GraphExecutor::active_generation() const noexcept
{
    if (!realtime_active_) return std::nullopt;
    return realtime_active_->graph->project_generation;
}

std::optional<std::uint64_t> GraphExecutor::pending_generation() const noexcept
{
    if (!pending_head_) return std::nullopt;
    return pending_head_->graph->project_generation;
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

std::shared_ptr<CompiledGraph const> GraphExecutor::accepted_graph() const noexcept
{
    return accepted_tail_graph_;
}

void GraphExecutor::set_tick_materialization_demand(IndexCoverage demand)
{
    struct Unlock {
        GraphExecutor* executor;
        ~Unlock() { executor->unlock_accepted_chain(); }
    };
    lock_accepted_chain();
    Unlock unlock{this};
    tick_materialization_demand_ = std::move(demand);
    for (auto* pending = pending_head_; pending;
         pending = pending->successor_owner.get()) {
        auto& background = pending->background;
        background.tick_materialization_demand =
            tick_materialization_demand_;
        auto const missing = background.tick_materialization_demand
            - background.materialized_tick_coverage;
        background.tick_materialization_required = !missing.empty()
            && !pending->graph->background_evaluation_plan
                    .tick_sequential_inputs.empty();
    }
    if (realtime_active_) {
        background_executor_.set_tick_materialization_demand(
            *realtime_active_, tick_materialization_demand_);
    }
}

std::expected<BackgroundEvaluationResult, std::string>
GraphExecutor::evaluate_generation(
    ExecutionGeneration& generation,
    bool publish_realtime_state)
{
    auto& background = generation.background;
    std::unique_ptr<RealtimePersistedState> published_state;
    if (publish_realtime_state) {
        // Every allocation/reader registration required by a coherent
        // realtime root happens before the domain transaction can commit.
        published_state = RealtimePersistedState::prepare(
            generation.graph->project_generation,
            persisted_data_,
            tick_materializations_);
    }
    background.select_inputs();
    auto roots = background.reconciliation_required
        ? background.reconciliation_roots
        : CoveragePropagationRequest{};
    append_tick_materialization_demands(
        generation.graph->background_evaluation_plan,
        background.tick_materialization_demand,
        roots);
    BackgroundEvaluationTransaction transaction{
        *generation.graph,
        background.tock_states,
        background.coverage,
        background.propagation,
        persisted_data_,
        tick_materializations_,
        BackgroundTransactionInputs{
            .roots = std::move(roots),
            .retired_outputs = background.reconciliation_required
                ? std::span<PersistedOutputHandle const>{
                    background.retired_outputs}
                : std::span<PersistedOutputHandle const>{},
        },
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
GraphExecutor::BackgroundExecutor::process_available_work_safely() noexcept
{
    try {
        return process_available_work();
    } catch (std::exception const& exception) {
        return std::unexpected(exception.what());
    } catch (...) {
        return std::unexpected(
            "BackgroundExecutor encountered an unknown failure");
    }
}

GraphExecutor::BackgroundExecutor::Result
GraphExecutor::BackgroundExecutor::process_available_work()
{
    for (;;) {
        auto& current = generation();
        if (!current.background_initialized) {
            current.initialize_first_background_generation();
        }
        current.background.select_inputs();
        if ((current.background.reconciliation_required
             || current.background.tick_materialization_required)
            && !current.graph->background_operations.valid()) {
            auto const& plan = current.graph->background_evaluation_plan;
            if (!plan.nodes.empty() || !plan.ports.empty()
                || !plan.storage.ports.empty()) {
                return std::unexpected(
                    "GraphExecutor cannot reconcile a successor with incomplete background operations");
            }
            if (current.background.retired_outputs.empty()
                && current.graph->background_evaluation_plan
                       .tick_sequential_inputs.empty()) {
                // An empty background plan has no materialized bindings to
                // rebind and no producer routes whose drained data could
                // become visible.
                current.background.reconciliation_required = false;
            }
        }
        if (current.background.has_selected_inputs()
            || current.background.reconciliation_required
            || current.background.tick_materialization_required) {
            auto evaluated = owner_.evaluate_generation(
                current, current.successor() == nullptr);
            if (!evaluated
                || evaluated->status != BackgroundEvaluationStatus::committed) {
                return evaluated;
            }
            current.background.reconciliation_required = false;
            current.background.tick_materialization_required = false;
            current.background.materialized_tick_coverage =
                current.background.tick_materialization_demand;
            current.background.retired_outputs.clear();
            current.background.reconciliation_roots = {};
            // A publication concurrent with the pinned prefix advanced the
            // work revision and will schedule another selection. Return after
            // one current-generation transaction so actor control commands
            // cannot be starved by a continuously producing realtime thread.
            if (!current.successor()) return evaluated;
            continue;
        }
        current.background.discard_empty_selection();
        if (!current.successor()) {
            return BackgroundEvaluationResult{};
        }
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

GraphExecutorReclaimedSnapshots
GraphExecutor::reclaim_retired_snapshots_on_background()
{
    auto const states = realtime_persisted_state_.reclaim_returned();
    return {
        .realtime_persisted_states = states,
        .persisted_data = persisted_data_.reclaim_retired(),
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

BackgroundExecutionFailures
GraphExecutor::background_execution_failures() const noexcept
{
    return background_executor_.failures();
}

void GraphExecutor::tick_block(GlobalIndex sample_index, std::size_t block_size)
{
    auto& generation = realtime_execution_generation();
    if (block_size == 0
        || block_size > generation.graph->specialization.block_size) {
        throw std::invalid_argument(
            "GraphExecutor tick block size is outside the compiled specialization");
    }
    generation.update_realtime_phase();
    // An open gesture's latest value reaches the root's per-call latch here,
    // independently of the control path's generation chain and locks.
    auto* const storage_base = generation.realtime.storage.buffer().data();
    for (auto& live : generation.live_targets) {
        auto const completion = live.target->completion_sequence.load(std::memory_order_acquire);
        if (live.target->open_gestures.load(std::memory_order_acquire) == 0
            && completion == live.observed_completion) continue;
        std::atomic_ref<std::uint32_t>(*reinterpret_cast<std::uint32_t*>(
            storage_base + live.target_storage_offset))
            .store(live.target->target_bits.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
        live.observed_completion = completion;
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
    auto const phase = generation.realtime_phase.load(std::memory_order_relaxed);
    auto const* realtime_code = generation.realtime_code.load(
        std::memory_order_acquire);
    auto const& root_operations = phase == CompiledGraphRealtimePhase::transition
        ? generation.graph->root_operations
        : realtime_code->root_operations;
    auto const& tick = phase == CompiledGraphRealtimePhase::transition
        ? root_operations.transition_tick_block
        : root_operations.tick_block;
    if (!tick) {
        throw std::logic_error(
            "GraphExecutor active realtime realization has no root operation");
    }
    tick(
        generation.realtime.storage.buffer().data(),
        &invocation.call(),
        sample_index,
        block_size);
    generation.update_realtime_phase(block_size);
}

} // namespace iv
