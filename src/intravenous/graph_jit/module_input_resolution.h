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
    // Public input channels with an authored contribution, after the tiled
    // expansion that structural routing applies to targets.
    std::vector<SampleInputChannelId> connected;
    auto collect_target = [&](auto&& self, SampleInputChannelId target) -> void {
        auto const& bundle = authored.node_bundles.bundle(target.bundle);
        if (bundle.is_tiled()) {
            auto const members = bundle.tiled_members();
            if (target.channel < members.size())
                self(self, SampleInputChannelId{members[target.channel], target.port, 0});
            return;
        }
        connected.push_back(target);
    };
    for (auto const& connection : authored_samples) {
        for (auto const target : connection.target_channels) collect_target(collect_target, target);
    }
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
                .replay_mode = TickReplayMode::full,
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
                .replay_mode = TickReplayMode::full,
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
    // Authored connections keep targeting the public input, so structural
    // routing still sums its contributions, with their conversions and
    // detaches, as at any module boundary. Internal consumers read the stage,
    // and the stage reads the boundary's internal side of the public input.
    std::vector<ConfiguredSampleConnection> samples(authored_samples.begin(), authored_samples.end());
    for (auto& connection : samples) {
        for (auto& source : connection.source_channels) {
            auto owner = boundary_owners.find(source.channel.bundle);
            if (owner == boundary_owners.end()) continue;
            auto stage = sample_inputs.find({owner->second, source.channel.port});
            if (stage != sample_inputs.end())
                source.channel = {stage->second, 0, source.channel.channel};
        }
    }
    for (auto const& [input, stage] : sample_inputs) {
        auto const [owner, port] = input;
        auto const boundary = *authored.node_bundles.bundle(owner).subgraph_boundary_handle();
        NodeBundlePortId const stage_input{stage, PortKind::sample, 0};
        auto const targets = graph.node_bundles.sample_input_channels(stage_input);
        auto const type = graph.node_bundles.resolve_sample_input(stage_input)
            .config.channel_layout.channel_type;
        std::vector<std::size_t> present;
        for (std::size_t channel = 0; channel < targets.size(); ++channel) {
            if (std::ranges::contains(connected, SampleInputChannelId{owner, port, channel}))
                present.push_back(channel);
        }
        // A disconnected public input leaves the stage input disconnected, so
        // its default or manual value applies.
        if (present.size() == targets.size()) {
            ConfiguredSampleConnection connection{
                .source_type = type,
                .target_type = type,
                .target_channels = {targets.begin(), targets.end()},
            };
            for (std::size_t channel = 0; channel < targets.size(); ++channel)
                connection.source_channels.push_back({.channel = {boundary, port, channel}});
            samples.push_back(std::move(connection));
        } else {
            for (auto const channel : present) {
                samples.push_back({
                    .source_type = ChannelTypeId::mono,
                    .source_channels = {{.channel = {boundary, port, channel}}},
                    .target_type = ChannelTypeId::mono,
                    .target_channels = {targets[channel]},
                });
            }
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
