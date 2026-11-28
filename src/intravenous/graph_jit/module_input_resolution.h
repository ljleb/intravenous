#pragma once

#include <intravenous/graph/configured_graph.hpp>
#include <intravenous/graph_jit/module_input_resolution_runtime.h>

#include <algorithm>
#include <map>
#include <utility>
#include <vector>

namespace iv::graph_jit::detail {

// Uses the same normalized structural routes as connection analysis.
void configure_module_input_event_bounds(ConfiguredGraph& graph);

// Compiler-private graph expansion. Authored bundles keep their handles and
// identities. One independently scheduled producer resolves each Sequential
// public input before its internal routing, conversion, and fan-out.
// Region and Coverage interfaces remain structural aliases.
inline ConfiguredGraph resolve_module_inputs(ConfiguredGraph const& authored)
{
    auto graph = authored;
    using Port = std::pair<NodeBundleHandle, std::size_t>;
    std::map<Port, NodeBundleHandle> sample_inputs, event_inputs;
    std::map<NodeBundleHandle, NodeBundleHandle> boundary_owners;
    auto const bundle_count = authored.node_bundles.size();
    auto const authored_samples = authored.connections.configured_sample_connections();
    for (NodeBundleHandle bundle = 0; bundle < bundle_count; ++bundle) {
        auto const boundary = authored.node_bundles.bundle(bundle).subgraph_boundary_handle();
        if (!boundary) continue;
        boundary_owners.emplace(*boundary, bundle);
        auto const& ports = authored.node_bundles.bundle(*boundary).boundary_ports();
        for (std::size_t port = 0; port < ports.sample_input_count(); ++port) {
            auto input = ports.sample_input(port);
            if (!is_sequential(input)) continue;
            auto output = authored.node_bundles.bundle(*boundary).sample_output_descriptor(port).config;
            ConcreteNode stage{
                .ports = {.input_configs = {make_input_config(input)},
                          .output_configs = {make_output_config(output)}},
                .replay_mode = TickReplayMode::enabled,
            };
            PortConstraintRef const in{PortConstraintDirection::input, PortKind::sample, 0};
            PortConstraintRef const out{PortConstraintDirection::output, PortKind::sample, 0};
            stage.port_constraints.value_type_equalities.push_back({in, out});
            stage.port_constraints.size_equalities.push_back({in, out});
            stage.port_constraints.pace_equalities.push_back({in, out});
            auto const operations = module_input_resolution_operations();
            stage.operations.runtime.declare_node = operations.declare_node;
            stage.operations.runtime.tick_block = operations.tick_block;
            stage.operations.runtime.skip_block = operations.skip_block;
            sample_inputs.emplace(Port{bundle, port},
                graph.node_bundles.append_input_resolution(std::move(stage),
                    {bundle, PortKind::sample, port}));
        }
        for (std::size_t port = 0; port < ports.event_input_count(); ++port) {
            auto input = ports.event_input(port);
            if (input.access != InputAccessKind::sequential) continue;
            auto output = authored.node_bundles.bundle(*boundary).event_output_descriptor(port).config;
            ConcreteNode stage{
                .ports = {.input_configs = {make_input_config(input)},
                          .output_configs = {make_output_config(output)}},
                .replay_mode = TickReplayMode::enabled,
            };
            auto const operations = module_input_resolution_operations();
            stage.operations.runtime.declare_node = operations.declare_node;
            stage.operations.runtime.tick_block = operations.tick_block;
            stage.operations.runtime.skip_block = operations.skip_block;
            event_inputs.emplace(Port{bundle, port},
                graph.node_bundles.append_input_resolution(std::move(stage),
                    {bundle, PortKind::event, port}));
        }
    }
    // Each wire enters the resolution stage separately so takeover can use
    // its original destination and ownership. Internal consumers read the
    // resolved stream.
    auto sample_input_stage = [&](auto&& self, SampleInputChannelId target)
        -> std::optional<SampleInputChannelId> {
        auto const& bundle = authored.node_bundles.bundle(target.bundle);
        if (bundle.is_tiled()) {
            auto const members = bundle.tiled_members();
            if (target.channel >= members.size()) return {};
            return self(self, SampleInputChannelId{members[target.channel], target.port, 0});
        }
        auto const stage = sample_inputs.find({target.bundle, target.port});
        if (stage == sample_inputs.end()) return {};
        return SampleInputChannelId{stage->second, 0, target.channel};
    };
    std::vector<ConfiguredSampleConnection> samples(authored_samples.begin(), authored_samples.end());
    for (auto& connection : samples) {
        for (auto& target : connection.target_channels) {
            if (auto stage = sample_input_stage(sample_input_stage, target)) target = *stage;
        }
        for (auto& source : connection.source_channels) {
            auto owner = boundary_owners.find(source.channel.bundle);
            if (owner == boundary_owners.end()) continue;
            auto stage = sample_inputs.find({owner->second, source.channel.port});
            if (stage != sample_inputs.end())
                source.channel = {stage->second, 0, source.channel.channel};
        }
    }
    auto expand_event_target = [&](auto&& self, EventInputPortId target,
                                   std::vector<EventInputPortId>& targets) -> void {
        auto const& bundle = authored.node_bundles.bundle(target.bundle);
        if (bundle.is_tiled()) {
            for (auto member : bundle.tiled_members()) self(self, {member, target.port}, targets);
            return;
        }
        auto stage = event_inputs.find({target.bundle, target.port});
        targets.push_back(stage != event_inputs.end() ? EventInputPortId{stage->second, 0} : target);
    };
    auto const authored_events = authored.connections.configured_event_connections();
    std::vector<ConfiguredEventConnection> events(authored_events.begin(), authored_events.end());
    for (auto& connection : events) {
        std::vector<EventInputPortId> targets;
        for (auto target : connection.targets)
            expand_event_target(expand_event_target, target, targets);
        connection.targets = std::move(targets);
        for (auto& source : connection.sources) {
            auto owner = boundary_owners.find(source.port.bundle);
            if (owner == boundary_owners.end()) continue;
            auto stage = event_inputs.find({owner->second, source.port.port});
            if (stage != event_inputs.end()) source.port = {stage->second, 0};
        }
    }
    graph.connections = GraphBuilderConnections::from_configured_connections(samples, events);
    for (auto& control : graph.manual_controls) {
        auto stage = sample_inputs.find({control.input.node_bundle_handle, control.input.port_index});
        if (stage != sample_inputs.end()) control.input = {stage->second, PortKind::sample, 0};
    }
    configure_module_input_event_bounds(graph);
    return graph;
}

} // namespace iv::graph_jit::detail
