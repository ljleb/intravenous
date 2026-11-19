#include <intravenous/runtime/graph_executor.h>

#include <intravenous/node/layout.h>
#include <intravenous/graph_jit/background_storage_plan.h>
#include <intravenous/graph_jit/sequential_coverage.h>
#include <intravenous/runtime/observation_window.h>
#include <intravenous/runtime/event_observation_window.h>
#include <intravenous/runtime/observation_resolution.h>
#include <intravenous/runtime/background_evaluation_call_frame.h>
#include <intravenous/runtime/playback_preparation_metrics.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdio>
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
    if (replacement.configured_graph) {
        for (auto const& control : replacement.configured_graph->manual_controls) {
            if (!control.requires_preparation) continue;
            if (!structural.configured_graph) return false;
            auto const previous = std::ranges::find(structural.configured_graph->manual_controls, control.id, &ConfiguredManualControl::id);
            if (previous == structural.configured_graph->manual_controls.end()
                || previous->settled_value != control.settled_value) return false;
        }
    }
    auto const steady_regions = static_cast<std::size_t>(std::ranges::count_if(
        structural.node_layout.regions,
        [](NodeLayout::Region const& region) { return !region.transition_only; }));
    if (replacement.cutover
        || replacement.project_generation != structural.project_generation
        || replacement.configured_generation != structural.configured_generation
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
    // Code-only replacement keeps the structural generation's background
    // workspace and immutable playback selection. A different fallback or
    // coverage inventory needs an ordinary prepared transition, even when the
    // NodeStorage byte extent happens to match.
    auto const& old_background = structural.background_evaluation_plan;
    auto const& new_background = replacement.background_evaluation_plan;
    if (old_background.requires_live_tick_coverage != new_background.requires_live_tick_coverage
        || old_background.nodes.size() != new_background.nodes.size()
        || old_background.ports.size() != new_background.ports.size()
        || old_background.connections != new_background.connections) return false;
    for (std::size_t port = 0; port < old_background.ports.size(); ++port) {
        auto const& old_port = old_background.ports[port];
        auto const& new_port = new_background.ports[port];
        if (old_port.configured_port != new_port.configured_port
            || old_port.direction != new_port.direction
            || old_port.input_access != new_port.input_access
            || old_port.content_access != new_port.content_access
            || old_port.full_live_tick_coverage != new_port.full_live_tick_coverage
            || old_port.sample_fallback_value != new_port.sample_fallback_value
            || old_port.sample_manual_contribution != new_port.sample_manual_contribution
            || old_port.accumulators.input_change != new_port.accumulators.input_change
            || old_port.accumulators.output_change != new_port.accumulators.output_change
            || old_port.incoming_connections != new_port.incoming_connections
            || old_port.outgoing_connections != new_port.outgoing_connections) return false;
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
        if (binding.configured.requires_preparation) continue;
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

bool requires_prepared_playback(CompiledGraph const& graph) noexcept
{
    auto const& plan = graph.background_evaluation_plan;
    return plan.requires_live_tick_coverage || !plan.tick_sequential_inputs.empty()
        || !plan.tick_runtime.random_access_sample_inputs.empty()
        || !plan.tick_runtime.random_access_event_inputs.empty();
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
        if (!uses_output_cache(previous_plan, prior, true)) {
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
                if (!uses_output_cache(current_plan, candidate, true)
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
    auto demand_complete = [&](auto const& bindings) {
        for (auto const& binding : bindings) {
            auto existing = std::ranges::find(roots.input_demands, binding.port, &InputCoverageRequest::port);
            if (existing == roots.input_demands.end()) roots.input_demands.push_back({
                .port = binding.port, .required = graph_jit::full_index_coverage()});
            else existing->required = graph_jit::full_index_coverage();
        }
    };
    demand_complete(plan.tick_runtime.random_access_sample_inputs);
    demand_complete(plan.tick_runtime.random_access_event_inputs);
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
    PersistedDataStore& data)
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
    for (std::size_t index = 0; index < requirements.size(); ++index) {
        producer_reserves.push_back(std::make_unique<ProducerReserve>(
            realtime_produced_block_storage_size));
    }
}

void GraphExecutor::RealtimeGeneration::provision_producers(
    AsyncCapacityManager& capacity_manager,
    RealtimeProducerCapacityConfig const& capacity_policy)
{
    auto const requirements = tick_invocation.producer_requirements();
    capacity_registrations.reserve(requirements.size());
    for (std::size_t index = 0; index < requirements.size(); ++index) {
        capacity_registrations.push_back(capacity_manager.register_producer(
            *producer_reserves[index],
            producer_capacity_policy(
                requirements[index].maximum_blocks_per_callback, capacity_policy)));
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
    std::atomic<bool>& production_reservation_failed,
    AsyncWorkSignal& background_work_signal)
    : graph(std::move(compiled_graph))
    , realtime(*graph, resources, data)
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
    if (requires_prepared_playback(*graph)) {
        background.reconciliation_required = true;
        for (graph_jit::BackgroundNodeIndex node = 0; node < graph->background_evaluation_plan.nodes.size(); ++node) {
            auto const& planned = graph->background_evaluation_plan.nodes[node];
            if (planned.authored_forward_coverage || planned.uses_replay_forward_coverage)
                background.reconciliation_roots.locally_changed_nodes.push_back(node);
        }
    }
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
    if (graph->specialization.sample_rate == previous.graph->specialization.sample_rate) {
        std::vector<NodeBundlePortId> inherited_outputs;
        std::vector<NodeBundlePortId> incomplete_outputs;
        auto const& requirements = graph->realtime_port_state_requirements.states;
        auto collect = [&](auto const& states) {
            for (auto const& state : states) {
                auto const& requirement = requirements[state.current_requirement_index];
                if (requirement.role != graph_jit::RealtimePortStateRole::tick_output_history) continue;
                auto const complete = state.has_predecessor
                    && state.inherited_begin == state.target_begin && state.inherited_end == 0;
                auto& outputs = complete ? inherited_outputs : incomplete_outputs;
                outputs.push_back(requirement.configured_port);
            }
        };
        collect(prepared_transition->plan().sample_states);
        collect(prepared_transition->plan().event_states);
        std::erase_if(inherited_outputs, [&](auto const& port) {
            return std::ranges::contains(incomplete_outputs, port);
        });
        realtime.tick_invocation.prepare_recording_transfer_from(
            previous.realtime.tick_invocation, graph->background_evaluation_plan, inherited_outputs);
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

void GraphExecutor::ExecutionGeneration::commit_background_migration(
    ExecutionGeneration const* drained_predecessor)
{
    if (background_initialized) return; // The successor may already own its prepared selection.
    if (!background_coverage_migration) {
        throw std::logic_error(
            "GraphExecutor background generation migration is not prepared");
    }
    // Private successor preparation must not consume an active predecessor's
    // cache. Only advance_generation(), after draining predecessor work, can
    // offer it to an authored move. Other preparation starts fresh.
    background.tock_states.initialize(drained_predecessor
        ? &drained_predecessor->background.tock_states : nullptr);
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
    stop();
}

void GraphExecutor::BackgroundExecutor::stop() noexcept
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

void GraphExecutor::BackgroundExecutor::queue_initial_playback(ExecutionGeneration& target)
{
    auto* head = initial_playback_requests_.load(std::memory_order_relaxed);
    do { target.initial_playback_next = head; }
    while (!initial_playback_requests_.compare_exchange_weak(head, &target,
        std::memory_order_release, std::memory_order_relaxed));
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

void GraphExecutor::BackgroundExecutor::retire_pending_generations(
    std::unique_ptr<ExecutionGeneration> suffix, bool initial)
{
    Command command{
        .kind = CommandKind::retire_pending_generations,
        .retiring = std::move(suffix),
        .retire_initial = initial,
    };
    enqueue(command);
    std::unique_lock lock{command.mutex};
    command.completed.wait(lock, [&] { return command.done; });
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
    PlaybackPreparationMetrics::Attachment preparation_metrics{owner_.preparation_metrics_};
    // Revision zero means no domain work has been consumed. The worker may be
    // scheduled for the first time after realtime has already published, so
    // sampling the current revision here would incorrectly discard that work.
    std::uint64_t observed_work_revision = 0;
    auto observed_wake_revision = work_signal_.wake_revision();
    bool serve_visible_waveform = false;
    for (;;) {
        auto* requests = initial_playback_requests_.exchange(nullptr, std::memory_order_acquire);
        ExecutionGeneration* ordered = nullptr;
        while (requests) {
            auto* next = requests->initial_playback_next;
            requests->initial_playback_next = ordered;
            ordered = requests;
            requests = next;
        }
        while (ordered) {
            auto* next = ordered->initial_playback_next;
            ordered->initial_playback_next = nullptr;
            preparation_queue_.push_back(ordered);
            ordered = next;
        }

        Command* command = nullptr;
        {
            std::lock_guard lock{commands_mutex_};
            if (stopping_ || stop.stop_requested()) {
                // Pending preparation can initialize a sidecar before its
                // realtime cutover. Tear it down on this worker too.
                std::vector<ExecutionGeneration*> retiring;
                auto add = [&](ExecutionGeneration* generation) {
                    if (generation && !std::ranges::contains(retiring, generation)) retiring.push_back(generation);
                };
                for (auto* generation = generation_chain_.get(); generation; generation = generation->successor()) add(generation);
                add(prepared_generation_);
                for (auto* generation : preparation_queue_) add(generation);
                for (auto* generation : retiring) {
                    generation->background.tock_states.release();
                    generation->background.tock_states.destroy_constructed_states();
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
            bool const caught_up = current_work_revision == observed_work_revision;
            if (caught_up || serve_visible_waveform) {
                serve_visible_waveform = false;
                std::shared_ptr<ObservationWindowQuery> query;
                {
                    std::lock_guard lock{commands_mutex_};
                    while (!observation_windows_.empty()
                        && observation_windows_.front()->canceled.load(std::memory_order_acquire))
                        observation_windows_.pop_front();
                    if (!observation_windows_.empty()) {
                        auto next = std::ranges::find_if(observation_windows_, [caught_up](auto const& pending) {
                            return !pending->prefetch.load(std::memory_order_relaxed)
                                && (caught_up || pending->port.port_kind == PortKind::sample)
                                && !pending->canceled.load(std::memory_order_acquire);
                        });
                        if (next == observation_windows_.end() && caught_up)
                            next = observation_windows_.begin();
                        if (next != observation_windows_.end()) {
                            query = std::move(*next);
                            observation_windows_.erase(next);
                        }
                    }
                }
                if (query) {
                    if (!query->canceled.load(std::memory_order_acquire)) process_window(*query);
                    continue; // Recheck playback and cutover work after each bounded query.
                }
                if (caught_up) {
                    auto const current_wake_revision = work_signal_.wake_revision();
                    if (current_wake_revision != observed_wake_revision) {
                        observed_wake_revision = current_wake_revision;
                        continue;
                    }
                    work_signal_.wait(observed_wake_revision);
                    continue;
                }
            }
            observed_work_revision = current_work_revision;
            if (!generation_chain_) continue;

            auto progress = process_available_work_safely();
            // Playback notifications can keep arriving faster than a pass
            // finishes. Let one visible waveform use the committed snapshot
            // before another pass. Cutovers and input reconciliation still
            // gate the query in process_window().
            serve_visible_waveform = progress && progress->status == BackgroundEvaluationStatus::committed;
            if (!progress || progress->status == BackgroundEvaluationStatus::terminal_preparation_error) {
                if (!progress_failed_.exchange(true, std::memory_order_acq_rel)) {
                    auto const& message = progress ? progress->preparation_error : progress.error();
                    std::fprintf(stderr, "background graph execution failed: %s\n", message.c_str());
                }
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
        } else if (command->kind == CommandKind::retire_pending_generations) {
            auto retired = command->retire_initial
                ? std::move(generation_chain_) : std::move(command->retiring);
            // The submission mutex excludes new requests until this command
            // completes. run() has already drained the intrusive requests.
            IV_ASSERT(prepared_generation_ == retired.get(), "failed generation is not the prepared head");
            prepared_generation_ = nullptr;
            preparation_queue_.clear();
            for (auto* generation = retired.get(); generation; generation = generation->successor_owner.get()) {
                IV_ASSERT(!generation->successor(), "retired pending generation had published a cutover");
                generation->background.tock_states.release();
                generation->background.tock_states.destroy_constructed_states();
            }
            retired.reset();
            work_signal_.notify_work();
            std::lock_guard lock{command->mutex};
            command->done = true;
            command->completed.notify_one();
            continue;
        } else if (command->kind
                   == CommandKind::set_tick_materialization_demand) {
            auto failure = [&]() -> std::exception_ptr {
                try {
                    // The control owner retains both active and accepted
                    // pending targets until this synchronous command completes.
                    auto* selected = command->demand_target;
                    if (!selected) {
                        throw std::logic_error(
                            "Tick materialization demand targets a retired generation");
                    }
                    auto& background = selected->background;
                    background.tick_materialization_demand =
                        std::move(command->materialization_demand);
                    background.explicit_playback_demand = !background.tick_materialization_demand.empty();
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
    RealtimeProducerCapacityConfig producer_capacity,
    BufferedStorageLimits buffered_storage,
    PlaybackPreparationMetrics* preparation_metrics)
    : resources_(std::move(resources))
    , producer_capacity_(producer_capacity)
    , persisted_data_(buffered_storage)
    , preparation_metrics_(preparation_metrics)
    , background_executor_(*this)
{
    measure_observation_capture_ = diagnostic_flag_enabled("IV_OBSERVATION_TIMING");
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
    background_executor_.stop();
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

GraphExecutorPreparedGeneration GraphExecutor::prepare(
    std::shared_ptr<CompiledGraph const> compiled_graph,
    std::shared_ptr<NodeDefinitionsSnapshot const> definitions)
{
    struct Unlock {
        GraphExecutor* executor;
        ~Unlock() { executor->unlock_accepted_chain(); }
    };
    struct PreparedSubmission {
        std::unique_lock<std::mutex> submission;
        std::unique_ptr<ExecutionGeneration> generation{};
    };
    auto submission = std::make_shared<PreparedSubmission>(
        PreparedSubmission{std::unique_lock(generation_submission_mutex_)});
    IndexCoverage playback_demand;
    ExecutionGeneration* predecessor = nullptr;
    {
        lock_accepted_chain();
        Unlock unlock{this};
        playback_demand = tick_materialization_demand_;
        predecessor = pending_tail_ ? pending_tail_ : realtime_active_;
    }
    if (!compiled_graph) {
        throw std::invalid_argument("GraphExecutor cannot stage an empty compiled graph");
    }
    auto const newest_generation = predecessor
        ? predecessor->graph->project_generation : std::uint64_t{0};
    if (predecessor
        && compiled_graph->project_generation <= newest_generation) {
        return {};
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
        production_reservation_failed_,
        background_executor_.work_signal());
    prepared->active_realtime_code_owner = prepare_realtime_code(prepared->graph);
    prepared->realtime_code.store(
        prepared->active_realtime_code_owner.get(), std::memory_order_relaxed);
    prepared->background.tick_materialization_demand = playback_demand;
    prepared->background.explicit_playback_demand = !playback_demand.empty();
    prepared->background.tick_materialization_required =
        !playback_demand.empty()
        && !prepared->graph->background_evaluation_plan
                .tick_sequential_inputs.empty();
    prepared->preparation_predecessor = predecessor;
    prepared->definitions_for_pending_recompile = std::move(definitions);
    if (!requires_prepared_playback(*prepared->graph))
        prepared->prepare_initial_persisted_state(persisted_data_);
    if (!predecessor) {
        prepared->initialize_first_generation();
    } else {
        prepared->prepare_realtime_migration_from(*predecessor);
        background_executor_.prepare_background_migration(
            *prepared, *predecessor);
    }
    prepared->realtime.provision_producers(async_capacity_manager_, producer_capacity_);
    submission->generation = std::move(prepared);
    return GraphExecutorPreparedGeneration{[this, submission = std::move(submission), predecessor]() {
        auto& prepared = submission->generation;
        lock_accepted_chain();
        Unlock unlock{this};
        // Another structural submission cannot pass the retained submission lock.
        // Audio may have activated the same tail while it was being prepared.
        if (prepared->background.tick_materialization_demand != tick_materialization_demand_)
            prepared->background.tick_materialization_demand = tick_materialization_demand_;
        prepared->background.explicit_playback_demand = !tick_materialization_demand_.empty();
        prepared->background.tick_materialization_required = !tick_materialization_demand_.empty()
            && !prepared->graph->background_evaluation_plan.tick_sequential_inputs.empty();
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
        if (requires_prepared_playback(*accepted->graph))
            background_executor_.queue_initial_playback(*accepted);
        return GraphExecutorStageResult::accepted;
    }};
}

GraphExecutorStageResult GraphExecutor::stage(
    std::shared_ptr<CompiledGraph const> compiled_graph)
{
    return std::move(prepare(std::move(compiled_graph))).publish();
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
    auto prepared = prepare_realtime_code(std::move(compiled_graph));
    // Activation has finished using the previous code before retiring it.
    // Reclamation belongs to this control path, never the audio callback.
    target->retired_realtime_code_owner.reset();
    target->pending_realtime_code_owner = std::move(prepared);
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

std::unique_ptr<GraphExecutor::ExecutionGeneration::RealtimeCode>
GraphExecutor::prepare_realtime_code(std::shared_ptr<CompiledGraph const> graph)
{
    static_assert(std::atomic<ExecutionGeneration::RealtimeCode*>::is_always_lock_free);
    auto prepared = std::make_unique<ExecutionGeneration::RealtimeCode>();
    prepared->publication_id = next_observation_publication_++;
    prepared->graph = std::move(graph);
    auto const& bindings = prepared->graph->manual_controls;
    prepared->live_targets.reserve(std::ranges::count_if(bindings,
        [](ManualControlRuntimeBinding const& binding) { return binding.configured.dynamic; }));
    auto const storage_size = prepared->graph->node_layout.storage_size;
    for (auto const& binding : bindings) {
        auto const offset = binding.state_storage_offset
            + offsetof(ManualControlRuntimeState, target_bits);
        if (offset > storage_size
            || sizeof(std::uint32_t) > storage_size - offset
            || offset % alignof(std::uint32_t) != 0) {
            throw std::logic_error("GraphExecutor manual-control state lies outside NodeStorage");
        }
        if (!binding.configured.dynamic) continue;
        auto target = live_target(binding.configured.id);
        auto const completion = target->completion_sequence.load(std::memory_order_acquire);
        prepared->live_targets.push_back({
            .target = std::move(target),
            .target_storage_offset = offset,
            .observed_completion = completion,
        });
    }
    return prepared;
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
    ProjectGraphGraphExecutorRequest const& request)
{
    return {.attempted = true, .prepared = prepare(request.graph, request.definitions)};
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
    return activate_pending_at({sample_index, {sample_index, 0}});
}

bool GraphExecutor::playback_ready(CompiledGraph const& graph,
    RealtimePersistedState const& persisted, PlaybackDemand const& demand) noexcept
{
    auto const& plan = graph.background_evaluation_plan;
    auto const extent = demand.paused ? 1 : demand.execution.empty()
        ? graph.specialization.block_size : demand.execution.extent;
    return plan.tick_sequential_inputs.empty() || persisted.prepares({demand.project, extent});
}

bool GraphExecutor::activate_pending_at(PlaybackDemand const& applied, PlaybackDemand const* candidate,
    RealtimePersistedState const** activated_root, bool* applied_candidate)
{
    if (accepted_chain_guard_.test_and_set(std::memory_order_acquire)) {
        return false;
    }
    struct Unlock {
        GraphExecutor* executor;
        ~Unlock() { executor->unlock_accepted_chain(); }
    } unlock{this};
    if (pending_head_ && requires_prepared_playback(*pending_head_->graph)) {
        requested_playback_index_.store(applied.project.bits(), std::memory_order_release);
        background_executor_.work_signal().notify_work();
    }
    auto const* selected_boundary = &applied;
    auto initial_selection = [&](ExecutionGeneration& generation) -> std::unique_ptr<RealtimePersistedState> {
        using Activation = ExecutionGeneration::Activation;
        auto claim = [&] {
            auto expected = Activation::pending;
            return generation.activation.compare_exchange_strong(expected, Activation::activating,
                std::memory_order_acq_rel);
        };
        if (generation.activation.load(std::memory_order_acquire) == Activation::failed) return {};
        if (requires_prepared_playback(*generation.graph)) {
            auto const* selected = generation.prepared_initial_states.adopt(generation.graph->project_generation);
            if (!selected) return {};
            if (candidate && playback_ready(*generation.graph, *selected, *candidate))
                selected_boundary = candidate;
            else if (!playback_ready(*generation.graph, *selected, applied)) return {};
            if (!claim()) return {};
            return generation.prepared_initial_states.take_active();
        }
        if (!claim()) return {};
        if (candidate) selected_boundary = candidate;
        if (!generation.initial_persisted_state || !generation.initial_persisted_state->capture_initial())
            throw std::logic_error("GraphExecutor initial persisted state was not prepared");
        return std::move(generation.initial_persisted_state);
    };
    auto install_initial = [&](std::unique_ptr<RealtimePersistedState> initial) {
        if (activated_root) *activated_root = initial.get();
        if (applied_candidate) *applied_candidate = candidate && selected_boundary == candidate;
        realtime_persisted_state_.activate_generation(std::move(initial));
    };
    auto activate_realtime_code = [&](ExecutionGeneration& generation) {
        generation.update_realtime_phase();
        if (!generation.pending_realtime_code_owner
            || generation.realtime_phase.load(std::memory_order_relaxed)
                != CompiledGraphRealtimePhase::steady) {
            return false;
        }
        IV_ASSERT(!generation.retired_realtime_code_owner,
            "cold code staging must reclaim previously retired code");
        generation.retired_realtime_code_owner =
            std::move(generation.active_realtime_code_owner);
        generation.active_realtime_code_owner =
            std::move(generation.pending_realtime_code_owner);
        generation.realtime_code.store(
            generation.active_realtime_code_owner.get(),
            std::memory_order_release);
        return true;
    };
    if (!pending_head_ || rebuilding_pending_suffix_) {
        return realtime_active_
            && activate_realtime_code(*realtime_active_);
    }
    if (!realtime_active_) {
        auto initial = initial_selection(*pending_head_);
        if (!initial) return false;
        auto* first = pending_head_;
        realtime_active_ = first;
        published_active_generation_.store(first->graph->configured_generation.value_or(first->graph->project_generation), std::memory_order_relaxed);
        published_has_active_generation_.store(true, std::memory_order_release);
        install_initial(std::move(initial));
        first->activation.store(ExecutionGeneration::Activation::active, std::memory_order_release);
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
    auto initial = initial_selection(*pending_head_);
    if (!initial) return false;
    pending_head_->commit_realtime_migration(selected_boundary->execution.begin);
    pending_head_->realtime.tick_invocation.transfer_recording_from(previous.realtime.tick_invocation);
    previous.background.close_inputs();
    auto* next = pending_head_;
    install_initial(std::move(initial));
    next->activation.store(ExecutionGeneration::Activation::active, std::memory_order_release);
    if (pending_owner_) {
        previous.attach_successor(std::move(pending_owner_));
    }
    previous.publish_attached_successor();
    realtime_active_ = next;
    published_active_generation_.store(next->graph->configured_generation.value_or(next->graph->project_generation), std::memory_order_relaxed);
    published_has_active_generation_.store(true, std::memory_order_release);
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

std::optional<FailedExecutionGeneration> GraphExecutor::reject_failed_pending_generation()
{
    std::scoped_lock submission{generation_submission_mutex_};
    std::unique_ptr<ExecutionGeneration> retired;
    FailedExecutionGeneration result;
    bool initial;
    {
        lock_accepted_chain();
        struct Unlock {
            GraphExecutor* executor;
            ~Unlock() { executor->unlock_accepted_chain(); }
        } unlock{this};
        if (!has_failed_pending_generation()) return std::nullopt;
        auto* failed = pending_head_;
        IV_ASSERT(failed && failed->activation.load(std::memory_order_acquire)
            == ExecutionGeneration::Activation::failed, "pending playback failure lost its generation");
        result.failed = failed->graph;
        result.message = failed->preparation_error;
        for (auto* successor = failed->successor_owner.get(); successor; successor = successor->successor_owner.get()) {
            auto const& replacement = successor->pending_realtime_code_owner;
            auto const& graph = replacement ? *replacement->graph : *successor->graph;
            result.successors.push_back({.project_generation = graph.project_generation,
                .graph = graph.configured_graph, .definitions = successor->definitions_for_pending_recompile,
                .observations = graph.observations, .configured_generation = graph.configured_generation});
        }
        if (!result.successors.empty()) {
            result.publish_rebuilt_suffix = [this] {
                lock_accepted_chain();
                rebuilding_pending_suffix_ = false;
                unlock_accepted_chain();
            };
            rebuilding_pending_suffix_ = true;
        }
        initial = !realtime_active_;
        if (!initial) {
            result.predecessor = realtime_active_->graph;
            retired = pending_owner_ ? std::move(pending_owner_) : std::move(realtime_active_->successor_owner);
            IV_ASSERT(retired.get() == failed, "pending generation recovery lost suffix ownership");
        }
        pending_head_ = nullptr;
        pending_tail_ = nullptr;
        accepted_tail_graph_ = result.predecessor;
        failed_pending_generation_.store(false, std::memory_order_release);
    }
    background_executor_.retire_pending_generations(std::move(retired), initial);
    return result;
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
        background_executor_.set_tick_materialization_demand(*pending, tick_materialization_demand_);
    }
    if (realtime_active_) {
        background_executor_.set_tick_materialization_demand(
            *realtime_active_, tick_materialization_demand_);
    }
}

std::expected<BackgroundEvaluationResult, std::string>
GraphExecutor::evaluate_generation(
    ExecutionGeneration& generation,
    bool publish_realtime_state,
    bool prepare_only)
{
    auto& background = generation.background;
    std::unique_ptr<RealtimePersistedState> published_state;
    if (publish_realtime_state && !prepare_only) {
        // Every allocation/reader registration required by a coherent
        // realtime root happens before the domain transaction can commit.
        published_state = RealtimePersistedState::prepare(
            generation.graph->project_generation,
            persisted_data_,
            tick_materializations_);
    }
    background.select_inputs();
    auto roots = background.reconciliation_required
        ? background.reconciliation_roots : CoveragePropagationRequest{};
    if (publish_realtime_state) {
        append_tick_materialization_demands(generation.graph->background_evaluation_plan,
            background.tick_materialization_demand, roots);
    }
    BackgroundEvaluationTransaction transaction{
        *generation.graph,
        background.tock_states,
        background.coverage,
        background.propagation,
        persisted_data_,
        tick_materializations_,
        BackgroundTransactionInputs{
            .roots = std::move(roots),
            .retired_outputs = !prepare_only && background.reconciliation_required
                ? std::span<PersistedOutputHandle const>{
                    background.retired_outputs}
                : std::span<PersistedOutputHandle const>{},
            .prepare_only = prepare_only,
            .reusable_data = background.prepared_data.get(),
            .reusable_materialization = background.prepared_materialization.get(),
            .reconstruction_graph = generation.graph,
        },
        background.input_routes,
        background.input_selections,
    };
    auto result = measure_playback_preparation(PlaybackPreparationPhase::transaction,
        [&] { return transaction.execute(); });
    if (!result || result->status != BackgroundEvaluationStatus::committed) {
        // Preserve every selected producer-queue prefix exactly as pinned so a
        // retry cannot accidentally absorb later realtime publications.
        return result;
    }
    if (result->published_data)
        execution_data_version_.store(result->published_data->revision, std::memory_order_release);
    if (prepare_only) {
        if (!result->prepared_data || !result->prepared_materialization)
            throw TerminalBackgroundPreparationError("successor preparation produced no coherent playback selection");
        background.prepared_data = std::move(result->prepared_data);
        background.prepared_materialization = std::move(result->prepared_materialization);
        auto selected = RealtimePersistedState::prepare_owned(generation.graph->project_generation,
            background.prepared_data, background.prepared_materialization);
        if (auto leased = measure_playback_preparation(PlaybackPreparationPhase::select_foreground_payload, [&] {
                return selected->select_payload(generation.graph->background_evaluation_plan, persisted_data_);
            }); !leased)
            return std::unexpected(std::move(leased.error()));
        PlaybackPreparationMetrics::Interval publish{PlaybackPreparationPhase::publish_foreground_snapshot};
        selected->set_prepared_playback(background.tick_materialization_demand);
        generation.prepared_initial_states.publish(std::move(selected));
    }
    std::optional<std::string> persisted_state_error{};
    if (published_state) {
        if (auto captured = published_state->capture_current(); !captured) {
            persisted_state_error.emplace(std::move(captured.error()));
        } else if (auto leased = measure_playback_preparation(PlaybackPreparationPhase::select_foreground_payload, [&] {
                return published_state->select_payload(generation.graph->background_evaluation_plan, persisted_data_);
            }); !leased) {
            persisted_state_error.emplace(std::move(leased.error()));
        } else {
            PlaybackPreparationMetrics::Interval publish{PlaybackPreparationPhase::publish_foreground_snapshot};
            published_state->set_prepared_playback(background.tick_materialization_demand);
            if (requires_prepared_playback(*generation.graph)
                && generation.activation.load(std::memory_order_acquire) != ExecutionGeneration::Activation::active)
                generation.prepared_initial_states.publish(std::move(published_state));
            else {
                // Initial adoption can race the last private refresh. Forward
                // that older refresh first, then publish this newer selection.
                if (auto pending = generation.prepared_initial_states.take_pending())
                    realtime_persisted_state_.publish(std::move(pending));
                realtime_persisted_state_.publish(std::move(published_state));
            }
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
    if (!prepare_only) {
        background.prepared_data.reset();
        background.prepared_materialization.reset();
    }
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
    if (!successor->background_initialized) successor->commit_background_migration(&previous);
    else {
        // Its private metadata/result pages remain reusable. Recording-root
        // identity comparisons in the transaction invalidate the finite tail
        // committed after that preview, without discarding unchanged results.
        successor->background.reconciliation_required = true;
    }

    auto retired = std::move(generation_chain_);
    generation_chain_ = std::move(retired->successor_owner);
    generation_chain_->definitions_for_pending_recompile.reset();
    current_generation_.store(
        generation_chain_->graph->project_generation,
        std::memory_order_release);
    return true;
}

GraphExecutor::BackgroundExecutor::Result
GraphExecutor::BackgroundExecutor::prepare_playback(ExecutionGeneration& target)
{
    PlaybackPreparationMetrics::Interval preparation{PlaybackPreparationPhase::prepare_playback};
    auto& background = target.background;
    if (!target.background_initialized) {
        if (target.preparation_predecessor) target.commit_background_migration();
        else target.initialize_first_background_generation();
        // A prepared migration may reuse metadata from a previously unused
        // Tock producer. Refresh it without inventing a local semantic change.
        for (graph_jit::BackgroundNodeIndex node = 0; node < target.graph->background_evaluation_plan.nodes.size(); ++node) {
            auto const& planned = target.graph->background_evaluation_plan.nodes[node];
            if (planned.authored_forward_coverage || planned.uses_replay_forward_coverage)
                background.reconciliation_roots.forward_nodes.push_back(node);
        }
    }
    auto const& plan = target.graph->background_evaluation_plan;
    auto const demand = owner_.read_playback_demand();
    IndexCoverage refill_demand = background.tick_materialization_demand;
    auto update_demand = [&](std::span<PlaybackDemand const> selections, bool speculate) {
        if (background.explicit_playback_demand) return;
        // Default reserve, not a compilation-latency promise. An explicit
        // set_tick_materialization_demand() retains caller-selected extents.
        constexpr std::size_t lookahead_blocks = 8;
        auto const block = target.graph->specialization.block_size;
        if (block > std::numeric_limits<std::size_t>::max() / lookahead_blocks)
            throw std::length_error("automatic background playback reserve overflows");
        auto const lookahead = block * lookahead_blocks;
        background.tick_materialization_demand = {};
        refill_demand = {};
        if (plan.tick_sequential_inputs.empty()) return;
        for (auto const& entry : selections) {
            if (entry.execution.empty()) continue;
            auto const extent = entry.paused ? 1 : entry.execution.extent;
            auto const reserve = speculate && !entry.paused;
            background.tick_materialization_demand.include(
                {entry.project, reserve ? std::max(extent, lookahead) : extent});
            // A moving playhead uses the ready reserve until less than half
            // remains. Refill both running and pending seek windows in one
            // transaction when either reaches that threshold.
            refill_demand.include(
                {entry.project, reserve ? std::max(extent, lookahead / 2) : extent});
        }
        if (background.tick_materialization_demand.empty()) {
            background.tick_materialization_demand.include({
                GlobalIndex::from_bits(owner_.requested_playback_index_.load(std::memory_order_acquire)),
                lookahead});
            refill_demand = background.tick_materialization_demand;
        }
    };
    auto prepare = [&]() -> Result {
        background.tick_materialization_required = !plan.tick_sequential_inputs.empty()
            && std::ranges::any_of(refill_demand, [&](auto range) {
                return !background.materialized_tick_coverage.contains(range);
            });
        background.select_inputs();
        if (!background.has_selected_inputs() && !background.reconciliation_required
            && !background.tick_materialization_required) {
            background.discard_empty_selection();
            return BackgroundEvaluationResult{};
        }
        auto const private_preparation = requires_prepared_playback(*target.graph)
            && target.activation.load(std::memory_order_acquire) != ExecutionGeneration::Activation::active;
        auto result = owner_.evaluate_generation(target, true, private_preparation);
        if (!result || result->status != BackgroundEvaluationStatus::committed) return result;
        background.reconciliation_required = false;
        background.tick_materialization_required = false;
        background.materialized_tick_coverage = background.tick_materialization_demand;
        if (!private_preparation) background.retired_outputs.clear();
        background.reconciliation_roots = {};
        (void)target.prepared_initial_states.reclaim_returned();
        return result;
    };
    if (demand) update_demand(*demand, true);
    auto result = prepare();
    if (!result && demand && !(*demand)[0].execution.empty()) {
        update_demand(*demand, false);
        result = prepare();
    }
    if (!result && demand && !(*demand)[1].execution.empty()) {
        update_demand(std::span{*demand}.first(1), true);
        auto applied = prepare();
        if (!applied) {
            update_demand(std::span{*demand}.first(1), false);
            applied = prepare();
        }
        if (applied) return applied;
    }
    return result;
}

GraphExecutor::BackgroundExecutor::Result
GraphExecutor::BackgroundExecutor::prepare_pending_playback(ExecutionGeneration& target)
{
    using Activation = ExecutionGeneration::Activation;
    if (target.activation.load(std::memory_order_acquire) == Activation::failed)
        return BackgroundEvaluationResult{};
    auto terminal_failure = [&](std::string message) -> Result {
        target.preparation_error = std::move(message);
        auto expected = Activation::pending;
        if (target.activation.compare_exchange_strong(expected, Activation::failed, std::memory_order_acq_rel)) {
            owner_.failed_pending_generation_.store(true, std::memory_order_release);
            return BackgroundEvaluationResult{};
        }
        return std::unexpected(target.preparation_error);
    };
    std::string retry_message;
    try {
        // The operations and plan maps belong to the immutable executable.
        if (!target.background_initialized) {
            auto const valid = graph_jit::detail::validate_background_runtime_plan(target.graph->background_evaluation_plan);
            if (!valid) return terminal_failure(valid.error());
            if (!target.graph->background_operations.valid())
                return terminal_failure("pending graph has incomplete background operations");
        }
        auto result = prepare_playback(target);
        if (result) {
            if (result->status == BackgroundEvaluationStatus::terminal_preparation_error)
                return terminal_failure(std::move(result->preparation_error));
            return result;
        }
        retry_message = std::move(result.error());
    } catch (TerminalBackgroundPreparationError const& error) {
        return terminal_failure(error.what());
    } catch (std::exception const& error) {
        retry_message = error.what();
    } catch (...) {
        retry_message = "unknown background preparation exception";
    }
    if (!target.background_initialized) {
        target.background.tock_states.release();
        target.background.tock_states.destroy_constructed_states();
    }
    if (target.activation.load(std::memory_order_acquire) == Activation::active)
        return std::unexpected(std::move(retry_message));
    if (!std::exchange(target.preparation_retry_reported, true))
        std::fprintf(stderr, "pending graph preparation will retry: %s\n", retry_message.c_str());
    return BackgroundEvaluationResult{};
}

GraphExecutor::BackgroundExecutor::Result
GraphExecutor::BackgroundExecutor::process_available_work_safely() noexcept
{
    PlaybackPreparationMetrics::Interval pass{PlaybackPreparationPhase::background_pass};
    try {
        // Retired snapshots must release obsolete pages before this pass tests
        // physical/residency budgets. Reclamation stays on the same writer.
        measure_playback_preparation(PlaybackPreparationPhase::reclaim_snapshots,
            [&] { (void)owner_.reclaim_retired_snapshots_on_background(); });
        measure_playback_preparation(PlaybackPreparationPhase::cache_maintenance,
            [&] { owner_.persisted_data_.prepare_cache(0, {}, [](auto) {}); });
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
        if (current.activation.load(std::memory_order_acquire) == ExecutionGeneration::Activation::active)
            current.definitions_for_pending_recompile.reset();
        if (prepared_generation_) {
            auto& prepared = *prepared_generation_;
            if (&prepared == &current) {
                if (prepared.activation.load(std::memory_order_acquire) != ExecutionGeneration::Activation::active)
                    return prepare_pending_playback(prepared);
                if (auto pending = prepared.prepared_initial_states.take_pending())
                    owner_.realtime_persisted_state_.publish(std::move(pending));
                // The first selection was private. Publish its reusable
                // output roots into the canonical store on this actor now.
                current.background.reconciliation_required = true;
                prepared_generation_ = nullptr;
            } else if (!current.successor()) {
                auto continued = prepare_playback(current);
                if (continued && continued->status != BackgroundEvaluationStatus::committed) return continued;
                auto successor = prepare_pending_playback(prepared);
                if (successor && successor->status == BackgroundEvaluationStatus::committed && !continued)
                    return continued;
                return successor;
            } else {
                if (current.successor() != &prepared)
                    return std::unexpected("prepared playback does not match the ordered cutover");
                current.background.select_inputs();
                if (current.background.has_selected_inputs()) {
                    auto drained = owner_.evaluate_generation(current, false);
                    if (!drained || drained->status != BackgroundEvaluationStatus::committed) return drained;
                    continue;
                }
                current.background.discard_empty_selection();
                if (!current.background.inputs_closed_and_drained() || !advance_generation())
                    return std::unexpected("prepared cutover could not drain its predecessor");
                if (auto pending = prepared.prepared_initial_states.take_pending())
                    owner_.realtime_persisted_state_.publish(std::move(pending));
                prepared_generation_ = nullptr;
                continue;
            }
        }
        if (!preparation_queue_.empty()) {
            auto* target = preparation_queue_.front();
            if (target == &current || target->preparation_predecessor == &current) {
                // Consume one finite predecessor prefix before switching the
                // canonical semantic writer to the prepared successor.
                Result caught_up{BackgroundEvaluationResult{}};
                if (target != &current) {
                    if (!current.background_initialized) current.initialize_first_background_generation();
                    caught_up = prepare_playback(current);
                    if (caught_up && caught_up->status != BackgroundEvaluationStatus::committed) return caught_up;
                }
                preparation_queue_.pop_front();
                prepared_generation_ = target;
                auto successor = prepare_pending_playback(*target);
                if (successor && successor->status == BackgroundEvaluationStatus::committed && !caught_up)
                    return caught_up;
                return successor;
            }
        }
        if (!current.background_initialized) {
            current.initialize_first_background_generation();
        }
        if (requires_prepared_playback(*current.graph)
            && !current.successor()) return prepare_playback(current);
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

void GraphExecutor::publish_playback_demand(std::array<PlaybackDemand, 2> demand) noexcept
{
    if (last_playback_demand_ == demand) return;
    last_playback_demand_ = demand;
    playback_demand_revision_.fetch_add(1, std::memory_order_acq_rel);
    for (std::size_t index = 0; index < demand.size(); ++index) {
        auto const& source = demand[index];
        auto& target = published_playback_demand_[index];
        target.project.store(source.project.bits(), std::memory_order_relaxed);
        target.execution.store(source.execution.begin.bits(), std::memory_order_relaxed);
        target.extent.store(source.execution.extent, std::memory_order_relaxed);
        target.paused.store(source.paused, std::memory_order_relaxed);
    }
    playback_demand_revision_.fetch_add(1, std::memory_order_release);
    background_executor_.work_signal().notify_work();
}

std::optional<std::array<GraphExecutor::PlaybackDemand, 2>> GraphExecutor::read_playback_demand() const noexcept
{
    auto const revision = playback_demand_revision_.load(std::memory_order_acquire);
    if (revision & 1) return {};
    std::array<PlaybackDemand, 2> result{};
    for (std::size_t index = 0; index < result.size(); ++index) {
        auto const& source = published_playback_demand_[index];
        result[index] = {
            GlobalIndex::from_bits(source.project.load(std::memory_order_relaxed)),
            {GlobalIndex::from_bits(source.execution.load(std::memory_order_relaxed)),
             source.extent.load(std::memory_order_relaxed)},
            source.paused.load(std::memory_order_relaxed),
        };
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    if (revision != playback_demand_revision_.load(std::memory_order_relaxed)) return {};
    return result;
}

void GraphExecutor::render_block(std::size_t frames, double sample_rate)
{
    auto const candidate = playback_.begin_block(sample_rate);
    auto const applied = playback_.current_block(sample_rate);
    bool const pending = applied.state.applied_command != candidate.state.applied_command;
    auto demand = [&](auto const& block) -> PlaybackDemand {
        return {block.state.background_playhead, {block.state.foreground_playhead, frames}, !block.state.running};
    };
    auto const applied_demand = demand(applied);
    auto const candidate_demand = pending ? demand(candidate) : PlaybackDemand{};
    publish_playback_demand({applied_demand, candidate_demand});
    RealtimePersistedState const* persisted = nullptr;
    bool can_apply = false;
    (void)activate_pending_at(applied_demand, pending ? &candidate_demand : nullptr,
        &persisted, &can_apply);
    if (!realtime_active_) return;
    auto& generation = *realtime_active_;
    if (!persisted) {
        persisted = realtime_persisted_state_.adopt(generation.graph->project_generation);
        if (!persisted) return;
        can_apply = pending && playback_ready(*generation.graph, *persisted, candidate_demand);
    }
    auto const& selected = can_apply ? candidate : applied;
    auto const can_execute = can_apply || playback_ready(*generation.graph, *persisted, applied_demand);
    execute_tick_block(generation, *persisted, selected.state.foreground_playhead, frames, selected.selection(), can_execute);
    if (can_execute) playback_.end_block(selected, frames);
}

void GraphExecutor::tick_block(GlobalIndex sample_index, std::size_t block_size)
{
    auto& generation = realtime_execution_generation();
    if (!generation.graph->background_evaluation_plan.tick_sequential_inputs.empty()) {
        requested_playback_index_.store(sample_index.bits(), std::memory_order_release);
        publish_playback_demand({});
        background_executor_.work_signal().notify_work();
    }
    auto const* persisted = realtime_persisted_state_.adopt(generation.graph->project_generation);
    if (!persisted) throw std::logic_error("GraphExecutor has no compatible realtime persisted state");
    auto const ready = generation.graph->background_evaluation_plan.tick_sequential_inputs.empty()
        || persisted->prepares({sample_index, block_size});
    execute_tick_block(generation, *persisted, sample_index, block_size, {}, ready);
}

void GraphExecutor::execute_tick_block(ExecutionGeneration& generation, RealtimePersistedState const& persisted,
    GlobalIndex sample_index, std::size_t block_size, TickPlaybackSelection selection, bool ready)
{
    if (block_size == 0
        || block_size > generation.graph->specialization.block_size) {
        throw std::invalid_argument(
            "GraphExecutor tick block size is outside the compiled specialization");
    }
    auto* const realtime_code = generation.realtime_code.load(std::memory_order_acquire);
    auto begin_observation_frame = [&]() -> LiveObservationFrame* {
        auto const changed = last_observation_publication_ != realtime_code->publication_id;
        if (!changed && realtime_code->graph->observations.probes.empty()) return nullptr;
        if (changed) {
            last_observation_publication_ = realtime_code->publication_id;
            ++observation_activation_;
        }
        // Code can be prepared in a different order from activation while a
        // successor waits for background data. Stamp the actual realtime order.
        auto& frame = observations_.begin(realtime_code->graph->observations,
            observation_activation_, generation.graph->project_generation, sample_index, block_size);
        frame.configured_generation = realtime_code->graph->configured_generation
            .value_or(generation.graph->project_generation);
        return &frame;
    };
    if (!ready) {
        playback_underrun_blocks_.fetch_add(1, std::memory_order_relaxed);
        // Activation and unavailability must reach observers even when no
        // DSP callback can run for this playback window.
        if (begin_observation_frame()) observations_.publish();
        return;
    }
    generation.update_realtime_phase();
    // An open gesture's latest value reaches the root's per-call latch here,
    // independently of the control path's generation chain and locks.
    auto* const storage_base = generation.realtime.storage.buffer().data();
    for (auto& live : realtime_code->live_targets) {
        auto const completion = live.target->completion_sequence.load(std::memory_order_acquire);
        if (live.target->open_gestures.load(std::memory_order_acquire) == 0
            && completion == live.observed_completion) continue;
        std::atomic_ref<std::uint32_t>(*reinterpret_cast<std::uint32_t*>(
            storage_base + live.target_storage_offset))
            .store(live.target->target_bits.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
        live.observed_completion = completion;
    }
    TickInvocationFrame invocation{
        persisted,
        generation.realtime.tick_invocation,
        sample_index,
        block_size, selection};
    auto const phase = generation.realtime_phase.load(std::memory_order_relaxed);
    auto const& root_operations = phase == CompiledGraphRealtimePhase::transition
        ? generation.graph->root_operations
        : realtime_code->graph->root_operations;
    auto const& tick = phase == CompiledGraphRealtimePhase::transition
        ? root_operations.transition_tick_block
        : root_operations.tick_block;
    if (!tick) {
        throw std::logic_error(
            "GraphExecutor active realtime realization has no root operation");
    }
    auto call = invocation.call();
    call.observations = begin_observation_frame();
    if (call.observations) call.observations->measure_capture = measure_observation_capture_;
    tick(
        generation.realtime.storage.buffer().data(),
        &call,
        sample_index,
        block_size);
    if (call.observations) observations_.publish();
    generation.update_realtime_phase(block_size);
}

} // namespace iv

namespace iv {
void GraphExecutor::handle_observation_collect(ObservationCollectRequest& request)
{
    request.handled = true;
    background_executor_.queue_windows(request.windows);
    request.execution_data_version = execution_data_version_.load(std::memory_order_acquire);
    if (auto const* frame = observations_.collect()) request.frame = *frame;
}

void GraphExecutor::BackgroundExecutor::queue_windows(
    std::span<std::shared_ptr<ObservationWindowQuery> const> queries)
{
    if (queries.empty()) return;
    {
        std::lock_guard lock{commands_mutex_};
        std::erase_if(observation_windows_, [](auto const& query) {
            return query->canceled.load(std::memory_order_acquire);
        });
        for (auto const& query : queries) {
            if (query->complete.load(std::memory_order_acquire)) continue;
            if (stopping_ || observation_windows_.size() >= maximum_live_observation_probes) {
                query->result.status = ObservationWindowResult::Status::failed;
                query->result.error = "background observation queue is full or stopped";
                query->complete.store(true, std::memory_order_release);
            } else observation_windows_.push_back(query);
        }
    }
    work_signal_.notify_control();
}

void GraphExecutor::BackgroundExecutor::process_event_windows(
    ExecutionGeneration& current, ObservationWindowQuery& query)
{
    auto const& graph = *current.graph;
    auto const& plan = graph.background_evaluation_plan;
    std::vector<std::shared_ptr<ObservationWindowQuery>> pending;
    pending.reserve(31);
    std::vector<ObservationWindowQuery*> batch;
    batch.reserve(32);
    batch.push_back(&query);
    {
        std::scoped_lock lock(commands_mutex_);
        for (auto it = observation_windows_.begin(); it != observation_windows_.end() && pending.size() < 31;) {
            auto const& next = **it;
            if (!next.canceled.load(std::memory_order_acquire) && next.port.port_kind == PortKind::event
                && next.configured_generation == query.configured_generation
                && next.window.begin == query.window.begin && next.window.length == query.window.length) {
                pending.push_back(std::move(*it));
                it = observation_windows_.erase(it);
            } else ++it;
        }
    }
    for (auto const& item : pending) batch.push_back(item.get());
    try {
        CoveragePropagationRequest roots;
        std::vector<BeatGridObservation> grids;
        auto const mark_share = std::size_t{4096} / batch.size();
        for (auto* item : batch) {
            auto& result = item->result;
            result.configured_generation = item->configured_generation;
            result.data_version = owner_.execution_data_version_.load(std::memory_order_acquire);
            auto const* input = observation_public_event_coverage_input(graph, item->port);
            if (!input || input->type != EventTypeId::beat || !input->reconstructible || !item->window.max_depth) {
                result.status = ObservationWindowResult::Status::unavailable;
                continue;
            }
            result.execution_generation = graph.project_generation;
            result.sample_rate = graph.specialization.sample_rate;
            grids.emplace_back(*item, *input, plan, std::min(mark_share, item->window.points));
            for (auto const& source : input->sources) {
                auto const node = plan.ports[source.output].node;
                if (!std::ranges::contains(roots.forward_nodes, node)) roots.forward_nodes.push_back(node);
            }
        }
        if (!grids.empty()) {
            BackgroundEvaluationTransaction transaction{graph, current.background.tock_states,
                current.background.coverage, current.background.propagation, owner_.persisted_data_,
                BackgroundTransactionInputs{.roots = std::move(roots), .projection = {
                    .data = &grids,
                    .project = +[](void* data, graph_jit::BackgroundEvaluationPlan const&,
                        PreparedCoveragePropagation const&, BackgroundStorageRealization const& storage,
                        BackgroundEvaluationCallFrame const& frame, PersistedDataVersion version) {
                        for (auto& grid : *static_cast<std::vector<BeatGridObservation>*>(data))
                            grid.project(storage, frame, version);
                    },
                    .select_demands = +[](void* data, BackgroundDemandSelection const& candidate,
                        CoveragePropagationRequest& roots, PersistedDataVersion) {
                        auto& grids = *static_cast<std::vector<BeatGridObservation>*>(data);
                        auto const share = std::size_t{8192} / grids.size();
                        for (auto& grid : grids) grid.select(candidate, roots, share, share);
                    }},
                    .retain_generated_outputs = false,
                    .maximum_materialized_sample_bytes = 16 * 1024 * 1024,
                    .reconstruction_graph = current.graph}};
            auto const evaluated = transaction.execute();
            if (!evaluated) throw std::runtime_error(evaluated.error());
            if (evaluated->status != BackgroundEvaluationStatus::committed)
                throw std::runtime_error("event observation snapshot changed before commit");
        }
        for (auto* item : batch)
            if (item->result.status == ObservationWindowResult::Status::pending_execution)
                item->result.status = ObservationWindowResult::Status::ready;
    } catch (std::exception const& error) {
        for (auto* item : batch) {
            item->result.events.clear();
            item->result.error = error.what();
            item->result.status = ObservationWindowResult::Status::failed;
        }
    } catch (...) {
        for (auto* item : batch) {
            item->result.events.clear();
            item->result.error = "background event observation failed";
            item->result.status = ObservationWindowResult::Status::failed;
        }
    }
    for (auto* item : batch) item->complete.store(true, std::memory_order_release);
}

void GraphExecutor::BackgroundExecutor::process_window(ObservationWindowQuery& query) noexcept
{
    auto& result = query.result;
    result.configured_generation = query.configured_generation;
    result.data_version = owner_.execution_data_version_.load(std::memory_order_acquire);
    try {
        (void)owner_.reclaim_retired_snapshots_on_background();
        auto* current = generation_chain_.get();
        if (!current || prepared_generation_ || !preparation_queue_.empty()
            || !current->background_initialized || current->background.has_selected_inputs()
            || current->background.reconciliation_required || current->background.tick_materialization_required
            || current->graph->configured_generation.value_or(current->graph->project_generation) != query.configured_generation
            || !owner_.published_has_active_generation_.load(std::memory_order_acquire)
            || owner_.published_active_generation_.load(std::memory_order_acquire) != query.configured_generation) {
            query.complete.store(true, std::memory_order_release);
            return;
        }
        if (!query.window.length || !query.window.points || query.window.points > maximum_observation_window_points)
            throw std::invalid_argument("invalid background observation window");
        auto const& graph = *current->graph;
        auto const& plan = graph.background_evaluation_plan;
        if (query.port.port_kind == PortKind::event) {
            process_event_windows(*current, query);
            return;
        }
        auto const observing_input = query.kind == ObservationSourceKind::input;
        auto const input_port = observing_input ? observation_input_window_port(graph, query.port) : std::nullopt;
        auto const* public_input = observing_input ? observation_public_coverage_input(graph, query.port) : nullptr;
        auto selected_output = query.port;
        if (!observing_input) {
            for (auto const& [source, view] : graph.background_output_gain_views) if (source == query.port) {
                selected_output = view;
                break;
            }
        }
        auto found = observing_input ? (input_port ? plan.ports.begin() + *input_port : plan.ports.end())
            : std::ranges::find_if(plan.ports, [&](auto const& port) {
                return port.direction == graph_jit::PortDirection::output && port.configured_port == selected_output;
            });
        auto const port_readable = public_input
            ? public_input->reconstructible && query.channel < channel_count(public_input->layout)
            : found != plan.ports.end() && found->kind == PortKind::sample
                && found->continuous_geometry.value_type == ContinuousValueTypeId::sample
                && (observing_input || found->authored_tock_output || found->persisted_tick_output)
                && query.channel < channel_count(found->sample_layout);
        if (!port_readable || !query.operation || !query.operation->capture_point) {
            result.status = ObservationWindowResult::Status::unavailable;
            query.complete.store(true, std::memory_order_release);
            return;
        }
        auto const port = static_cast<graph_jit::BackgroundPortIndex>(found - plan.ports.begin());
        auto const pace = public_input ? public_input->pace_exponent : found->absolute_pace_exponent;
        result.execution_generation = graph.project_generation;
        result.sample_rate = graph.specialization.sample_rate;
        result.pace_exponent = pace;
        if (public_input && public_input->projections.empty()) {
            auto reader = owner_.persisted_data_.register_reader();
            auto const version = reader.pin()->version();
            result.semantic_version = version.semantic;
            result.data_version = version.revision;
            result.status = ObservationWindowResult::Status::ready;
            query.complete.store(true, std::memory_order_release);
            return;
        }
        auto lattice = FractionalGlobalIndexRange::from_global_range({query.window.begin, query.window.length})
            .try_project_onto_lattice(pace);
        if (!lattice) throw std::invalid_argument("observation window cannot be represented on the port lattice");
        GlobalIndexRange window{GlobalIndex::from_bits(lattice->begin_index_bits), lattice->count};
        auto positions = observation_window_positions(window, query.window.points);
        auto const cacheable = public_input
            ? std::ranges::all_of(public_input->projections, [&](auto const& projection) {
                return std::ranges::all_of(projection.sources, [&](auto const& source) {
                    return plan.ports[source.output].authored_tock_output;
                });
            }) : !observing_input && found->authored_tock_output;
        auto const matches_source = [&](CachedObservationWindow const& previous) {
            return previous.port == query.port && previous.kind == query.kind
                && previous.channel == query.channel && previous.operation == query.operation;
        };
        std::vector<CachedObservationWindow const*> previous;
        if (cacheable) {
            for (auto const& cached : sampled_windows_)
                if (matches_source(cached)) previous.push_back(&cached);
        }
        BackgroundSampleDemand demand;
        if (!cacheable) demand.include_ordered_positions(positions);
        struct Projection {
            ObservationWindowQuery& query;
            graph_jit::BackgroundPortIndex port;
            graph_jit::PublicCoverageInputView const* public_input;
            GlobalIndexRange window;
            std::span<GlobalIndex const> positions;
            std::span<CachedObservationWindow const* const> previous;
            ObservationWindowReuse reuse;
            PersistedDataVersion reuse_version;
        } projection{query, port, public_input, window, positions, previous, {}, {}};
        CoveragePropagationRequest roots;
        // Forward metadata is semantic, independent of what previous playback
        // happened to materialize. The ordinary transaction discovers dependencies.
        if (public_input) {
            for (auto const& projection : public_input->projections) {
                for (auto const& source : projection.sources) {
                    auto const node = plan.ports[source.output].node;
                    if (!std::ranges::contains(roots.forward_nodes, node)) roots.forward_nodes.push_back(node);
                    auto required = std::ranges::find(roots.output_demands, source.output, &OutputCoverageRequest::port);
                    if (required == roots.output_demands.end())
                        required = roots.output_demands.insert(roots.output_demands.end(), {.port = source.output});
                    auto shifted = demand.retreated_by(source.read_latency);
                    required->sample_positions.include(shifted.view());
                }
            }
        } else if (observing_input) {
            roots.input_demands.push_back({.port = port, .sample_positions = std::move(demand)});
            for (auto const connection : found->incoming_connections)
                for (auto const source : plan.connections[connection].source_coverage_ports)
                    if (auto const node = plan.ports[source].node; !std::ranges::contains(roots.forward_nodes, node))
                        roots.forward_nodes.push_back(node);
        } else {
            roots.output_demands.push_back({.port = port, .sample_positions = std::move(demand)});
            roots.forward_nodes.push_back(found->node);
        }
        BackgroundEvaluationTransaction transaction{graph, current->background.tock_states,
            current->background.coverage, current->background.propagation, owner_.persisted_data_,
            BackgroundTransactionInputs{.roots = std::move(roots),
                .projection = {.data = &projection, .project = +[](void* opaque, graph_jit::BackgroundEvaluationPlan const& plan,
                    PreparedCoveragePropagation const& prepared, BackgroundStorageRealization const& storage,
                    BackgroundEvaluationCallFrame const& frame, PersistedDataVersion version) {
                    auto& selected = *static_cast<Projection*>(opaque);
                    auto& result = selected.query.result;
                    if (!selected.reuse.windows.empty() && version != selected.reuse_version)
                        throw std::runtime_error("observation snapshot changed during sample reuse");
                    result.semantic_version = version.semantic;
                    result.data_version = version.revision;
                    if (selected.public_input) {
                        project_public_coverage_input(selected.query, *selected.public_input, selected.window,
                            selected.positions, plan, prepared, storage, &selected.reuse);
                        return;
                    }
                    auto const& port = plan.ports[selected.port];
                    if (port.direction == graph_jit::PortDirection::input) {
                        if (port.input_access == InputAccessKind::sequential) {
                            if (selected.window.empty()) return;
                            auto& region = result.regions.emplace_back();
                            region.range = selected.window;
                            for (auto const position : selected.positions) {
                                if (selected.query.canceled.load(std::memory_order_relaxed)) return;
                                ObservationCaptureState captured;
                                if (auto const value = frame.observe_sequential_sample_input(
                                        selected.port, position, selected.query.channel))
                                    selected.query.operation->capture_point(captured, &*value);
                                region.values.emplace_back(position, captured);
                            }
                        } else {
                            auto const input = frame.discovery_continuous_input(selected.port);
                            std::vector<GlobalIndexRange> logical;
                            for (auto const& region : input.region_views()) logical.push_back(region.range);
                            BackgroundSampleReadView const view{
                                .regions = input.region_views(), .channels = std::span{&selected.query.channel, 1},
                            };
                            project_observation_window(selected.query, selected.window, selected.positions, logical, &view);
                        }
                        return;
                    }
                    auto const slot = observation_output_storage(plan, storage, selected.port, selected.query.channel);
                    auto const* view = slot ? storage.sample_read(*slot) : nullptr;
                    auto logical = prepared.output_regions(selected.port);
                    std::vector<GlobalIndexRange> recorded_regions;
                    if (logical.empty() && view) {
                        for (auto const& region : view->regions) recorded_regions.push_back(region.range);
                        logical = recorded_regions;
                    }
                    std::optional<std::span<std::size_t const>> prepared_regions;
                    if (slot && plan.ports[selected.port].authored_tock_output) {
                        if (auto const& indices = storage.selection().output_region_indices[*slot])
                            prepared_regions = std::span<std::size_t const>{*indices};
                    }
                    project_observation_window(selected.query, selected.window, selected.positions, logical, view,
                        plan.ports[selected.port].persisted_tick_output, prepared_regions,
                        &selected.reuse);
                }, .sample_input = input_port,
                .select_demands = cacheable ? +[](void* opaque, BackgroundDemandSelection const& candidate,
                    CoveragePropagationRequest& roots, PersistedDataVersion version) {
                    auto& selected = *static_cast<Projection*>(opaque);
                    for (auto const* previous : selected.previous) {
                        if (previous->result.execution_generation == selected.query.result.execution_generation
                            && previous->result.semantic_version == version.semantic
                            && previous->result.data_version == version.revision)
                            selected.reuse.windows.emplace_back(previous->positions, previous->result.regions);
                    }
                    selected.reuse_version = version;
                    std::vector<GlobalIndex> missing;
                    for (auto const position : selected.positions)
                        if (!selected.reuse.contains(position)) missing.push_back(position);
                    BackgroundSampleDemand demand;
                    demand.include_ordered_positions(missing);
                    auto add_runs = [&](graph_jit::BackgroundPortIndex output, BackgroundSampleDemand const& positions) {
                        auto const regions = candidate.output_regions(candidate.data, output);
                        for (std::size_t index = 0; index < regions.size(); ++index) {
                            auto const region = regions[index];
                            if (!positions.intersects(region)) continue;
                            auto& selection = roots.indexed_output_demands.emplace_back(output, index);
                            for (auto const run : positions.prepared_runs(region, {}, maximum_observation_window_points))
                                selection.runs.push_back({region.begin.forward_distance_to(run.first), run.count, run.step});
                        }
                    };
                    if (selected.public_input) {
                        for (auto const& projection : selected.public_input->projections)
                            for (auto const& source : projection.sources)
                                add_runs(source.output, demand.retreated_by(source.read_latency));
                    } else {
                        add_runs(selected.port, demand);
                    }
                } : nullptr},
                // The response owns the sampled values. Sparse viewport
                // demands should not fill the shared mapped playback cache.
                .retain_generated_outputs = false,
                .maximum_materialized_sample_bytes = 16 * 1024 * 1024,
                .reconstruction_graph = current->graph}};
        auto evaluated = transaction.execute();
        if (!evaluated) throw std::runtime_error(evaluated.error());
        if (evaluated->status != BackgroundEvaluationStatus::committed)
            throw std::runtime_error("background observation snapshot changed before commit");
        result.status = ObservationWindowResult::Status::ready;
        if (cacheable && !result.truncated && !query.canceled.load(std::memory_order_relaxed)) {
            std::erase_if(sampled_windows_, [&](auto const& cached) {
                return cached.result.execution_generation != result.execution_generation
                    || cached.result.semantic_version != result.semantic_version
                    || cached.result.data_version != result.data_version
                    || (matches_source(cached) && cached.positions == positions);
            });
            constexpr std::size_t maximum_cached_windows = 16;
            if (sampled_windows_.size() == maximum_cached_windows) sampled_windows_.pop_front();
            sampled_windows_.push_back({query.port, query.kind, query.channel, query.operation,
                std::move(positions), result});
        }
    } catch (std::exception const& error) {
        result.regions.clear();
        result.events.clear();
        result.unavailable_regions.clear();
        result.error = error.what();
        result.status = ObservationWindowResult::Status::failed;
    } catch (...) {
        result.regions.clear();
        result.events.clear();
        result.unavailable_regions.clear();
        result.error = "background observation failed";
        result.status = ObservationWindowResult::Status::failed;
    }
    query.complete.store(true, std::memory_order_release);
}
} // namespace iv
