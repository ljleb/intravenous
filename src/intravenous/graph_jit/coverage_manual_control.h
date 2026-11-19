#pragma once

#include <intravenous/graph_jit/sequential_coverage.h>
#include <intravenous/graph/reflected_node_operations.h>
#include <intravenous/node/layout.h>
#include <intravenous/graph/configured_graph.hpp>

namespace iv::graph_jit::detail {

struct CoverageManualSource {
    NodeBundleHandle bundle;
    std::shared_ptr<Sample const> value;
};

inline std::size_t declare_coverage_manual(void const*, NodeStateStructures const*, NodeLayoutBuilder& builder,
    ReflectedNodePortRealization const&)
{
    return iv::details::register_node(builder, {.node_type_name = "Coverage manual input"});
}

inline auto expand_coverage_manual_controls(ConfiguredGraph& graph)
{
    std::erase_if(graph.manual_controls, [&](auto const& control) {
        return !control.output && !control.enabled && !control.override_connections
            && supports_coverage_manual_control(graph.node_bundles.resolve_sample_input(control.input).config);
    });
    std::vector<CoverageManualSource> sources;
    auto const original = graph.connections.configured_sample_connections();
    std::vector<ConfiguredSampleConnection> connections(original.begin(), original.end());
    for (auto& control : graph.manual_controls) {
        if (control.output) continue;
        auto const input = graph.node_bundles.resolve_sample_input(control.input).config;
        if (!supports_coverage_manual_control(input)) continue;
        auto value = std::make_shared<Sample const>(control.settled_value);
        ContinuousOutputConfig output{
            .name = "manual", .channel_layout = input.channel_layout,
            .production = default_coverage_output_properties(continuous_value_type_domain(input)),
        };
        ConcreteNode node{
            .ports = {.input_configs = {make_input_config(input)}, .output_configs = {make_output_config(output)}},
            .node_storage = value, .node_size = sizeof(Sample), .node_alignment = alignof(Sample),
            .type_identity = NodeTypeIdentity{.value = "compiler.coverage_manual_control"},
        };
        PortConstraintRef const in{PortConstraintDirection::input, PortKind::sample, 0};
        PortConstraintRef const out{PortConstraintDirection::output, PortKind::sample, 0};
        node.port_constraints.pace_equalities.push_back({in, out});
        node.port_constraints.size_equalities.push_back({in, out});
        node.operations.runtime.node_data = value.get();
        node.operations.runtime.declare_node = &declare_coverage_manual;
        node.operations.runtime.has_propagate_forward_coverage = true;
        node.operations.runtime.has_propagate_reverse_coverage = true;
        auto const bundle = graph.node_bundles.append_concrete(std::move(node));
        control.coverage_source = bundle;
        sources.push_back({bundle, std::move(value)});

        ConfiguredSampleConnection connection{
            .source_type = output.channel_layout.channel_type, .target_type = input.channel_layout.channel_type,
            .target_channels = graph.node_bundles.sample_input_channels(control.input),
        };
        for (std::size_t channel = 0; channel < connection.target_channels.size(); ++channel)
            connection.source_channels.push_back({.channel = {bundle, 0, channel}});
        connections.push_back(std::move(connection));
    }
    if (!sources.empty()) graph.connections = GraphBuilderConnections::from_configured_connections(
        connections, graph.connections.configured_event_connections());
    return sources;
}

inline void forward_coverage_manual(void const*, ReflectedNodeForwardCoverageContext const& context)
{
    auto const missing = full_index_coverage() - context.inputs.data()[0].coverage();
    auto const& output = context.outputs.data()[0];
    auto const previous = output.previous_coverage();
    std::vector<GlobalIndexRange> regions(missing.begin(), missing.end());
    output.publish_regions(regions);
    output.change(context.local_state_changed ? previous | missing : (previous - missing) | (missing - previous));
}

inline void reverse_coverage_manual(void const*, ReflectedNodeReverseCoverageContext const&) {}

inline void tock_coverage_manual(void const* data, ReflectedNodeTockCoverageContext const& context)
{
    auto const value = *static_cast<Sample const*>(data);
    for (auto run : context.outputs.data()[0].prepared_runs<Sample>()) {
        auto const channels = channel_count(run.channel_layout());
        for (std::size_t slot = 0; slot < run.size(); ++slot)
            for (std::size_t channel = 0; channel < channels; ++channel)
                run.value(slot, channel) = value;
    }
}

inline void forward_coverage_manual_batch(ReflectedNodeForwardCoverageBatchContext const& batch)
{
    for (auto const& lane : std::span<ReflectedNodeForwardCoverageBatchLane const>{batch.lanes})
        forward_coverage_manual(lane.node_data, lane.context);
}

inline void reverse_coverage_manual_batch(ReflectedNodeReverseCoverageBatchContext const&) {}

inline void tock_coverage_manual_batch(ReflectedNodeTockCoverageBatchContext const& batch)
{
    for (auto const& lane : std::span<ReflectedNodeTockCoverageBatchLane const>{batch.lanes})
        tock_coverage_manual(lane.node_data, lane.context);
}

} // namespace iv::graph_jit::detail
