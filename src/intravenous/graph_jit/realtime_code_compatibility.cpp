#include <intravenous/graph_jit/realtime_code_compatibility.h>

#include <algorithm>
#include <ranges>

namespace iv::graph_jit {

RealtimeCodeConfiguration realtime_code_configuration(CompiledGraph const& graph) noexcept
{
    return {graph.configured_generation, graph.definitions_generation,
        graph.specialization, graph.configured_graph.get(), graph.configured_bundles};
}

RealtimeCodeLayout realtime_code_layout(CompiledGraph const& graph) noexcept
{
    return {realtime_code_configuration(graph), graph.node_layout, graph.background_evaluation_plan,
        graph.realtime_port_state_requirements, graph.realtime_port_state_realizations, graph.manual_controls};
}

bool same_audio_configuration(CompiledGraph const& lhs, RealtimeCodeConfiguration const& rhs) noexcept
{
    return lhs.configured_bundles.snapshot && lhs.configured_bundles == rhs.configured_bundles
        && lhs.definitions_generation == rhs.definitions_generation
        && lhs.specialization.sample_rate == rhs.specialization.sample_rate
        && lhs.specialization.block_size == rhs.specialization.block_size
        && lhs.configured_graph && rhs.configured_graph
        && lhs.configured_graph->manual_controls == rhs.configured_graph->manual_controls;
}

namespace {

bool prepared_controls_match(
    CompiledGraph const& structural,
    RealtimeCodeLayout const& replacement) noexcept
{
    if (!replacement.configuration.configured_graph) return true;
    for (auto const& control : replacement.configuration.configured_graph->manual_controls) {
        if (!control.requires_preparation) continue;
        if (!structural.configured_graph) return false;
        auto const& previous_controls = structural.configured_graph->manual_controls;
        auto const previous = std::ranges::find(previous_controls, control.id, &ConfiguredManualControl::id);
        if (previous == previous_controls.end() || previous->settled_value != control.settled_value)
            return false;
    }
    return true;
}

// The running generation keeps both its TickInvocationWorkspace and its
// background playback selection when new steady code takes effect.
bool can_reuse_background_workspace(
    graph_jit::BackgroundEvaluationPlan const& previous,
    graph_jit::BackgroundEvaluationPlan const& replacement) noexcept
{
    if (previous.tick_runtime != replacement.tick_runtime
        || previous.requires_live_tick_coverage != replacement.requires_live_tick_coverage
        || previous.nodes.size() != replacement.nodes.size()
        || previous.connections != replacement.connections) return false;
    return std::ranges::equal(previous.ports, replacement.ports, [](auto const& before, auto const& after) {
        return before.configured_port == after.configured_port
            && before.direction == after.direction
            && before.input_access == after.input_access
            && before.content_access == after.content_access
            && before.full_live_tick_coverage == after.full_live_tick_coverage
            && before.sample_fallback_value == after.sample_fallback_value
            && before.sample_manual_contribution == after.sample_manual_contribution
            && before.accumulators.input_change == after.accumulators.input_change
            && before.accumulators.output_change == after.accumulators.output_change
            && before.incoming_connections == after.incoming_connections
            && before.outgoing_connections == after.outgoing_connections;
    });
}

} // namespace

// Steady Tick code addresses the canonical NodeStorage prefix. Each graph can
// also have a transition-only suffix that its steady code never uses.
bool compatible_steady_realtime_layout(
    CompiledGraph const& structural,
    RealtimeCodeLayout const& replacement) noexcept
{
    if (!prepared_controls_match(structural, replacement)
        || !can_reuse_background_workspace(
            structural.background_evaluation_plan, replacement.background_evaluation_plan))
        return false;

    auto const steady_regions = static_cast<std::size_t>(std::ranges::count_if(
        structural.node_layout.regions,
        [](NodeLayout::Region const& region) { return !region.transition_only; }));
    auto const replacement_steady_regions = static_cast<std::size_t>(std::ranges::count_if(
        replacement.node_layout.regions,
        [](NodeLayout::Region const& region) { return !region.transition_only; }));
    if (replacement.configuration.configured_generation != structural.configured_generation
        || replacement.configuration.definitions_generation
            != structural.definitions_generation
        || replacement.configuration.specialization.sample_rate
            != structural.specialization.sample_rate
        || replacement.configuration.specialization.block_size
            != structural.specialization.block_size
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
        || replacement_steady_regions != steady_regions
        || replacement.node_layout.imported_arrays.size()
            != structural.node_layout.imported_arrays.size()
        || replacement.node_layout.exported_arrays.size()
            != structural.node_layout.exported_arrays.size()
        || replacement.node_layout.initialize_order
            != structural.node_layout.initialize_order
        || replacement.manual_controls.size() != structural.manual_controls.size()) {
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
        std::span<ManualControlRuntimeBinding const> controls,
        NodeLayout::Region const& region) {
        return region.kind == NodeLayout::Region::Kind::raw
            && region.size == sizeof(ManualControlRuntimeState)
            && std::ranges::any_of(
                controls,
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
        auto const lhs_manual = owns_manual_control_state(structural.manual_controls, lhs);
        auto const rhs_manual = owns_manual_control_state(replacement.manual_controls, rhs);
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

static bool same_realtime_port_state(
    CompiledGraph const& previous, RealtimeCodeLayout const& successor) noexcept
{
    auto const& before = previous.realtime_port_state_realizations;
    auto const& after = successor.realtime_port_state_realizations;
    return previous.realtime_port_state_requirements.states == successor.realtime_port_state_requirements.states
        && before.sample_states == after.sample_states
        && before.event_states == after.event_states;
}

bool can_reuse_observation_layout(
    CompiledGraph const& structural, CompiledGraph const& active_code,
    RealtimeCodeLayout const& replacement) noexcept
{
    return same_audio_configuration(active_code, replacement.configuration)
        && same_realtime_port_state(structural, replacement)
        && compatible_steady_realtime_layout(structural, replacement);
}

} // namespace iv::graph_jit
