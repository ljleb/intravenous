#pragma once

#include <intravenous/graph/configured_graph.hpp>

#include <algorithm>
#include <expected>
#include <map>
#include <set>
#include <string>

namespace iv::graph_jit::detail {

inline std::expected<SampleInputChannelId, std::string> resolve_structural_sample_target(
    ConfiguredGraph const& graph, SampleInputChannelId target)
{
    while (true) {
        if (target.bundle >= graph.node_bundles.size())
            return std::unexpected("GraphJit structural sample route references a missing target bundle");
        auto const& bundle = graph.node_bundles.bundle(target.bundle);
        if (!bundle.is_tiled()) return target;
        auto const members = bundle.tiled_members();
        if (target.channel >= members.size())
            return std::unexpected("GraphJit tiled sample target channel is outside its member set");
        target = {members[target.channel], target.port, 0};
    }
}

// Project wiring claims a port even in gaps. A settled knob also claims its
// selected port, independently of whether a project cable has coverage there.
// An enabled concrete fallback knob also makes symbolic contributions
// unreachable. Keep the configured wires intact and exclude the replaced
// ones from port constraints and execution. Gestures are handled separately.
class SampleInputTakeovers {
    struct Claim {
        bool concrete_knob = false;
    };
    ConfiguredGraph const& graph_;
    std::map<NodeBundlePortId, Claim, NodeBundlePortIdLess> inputs_;

    void claim(SampleInputChannelId target, bool concrete_knob = false)
    {
        if (auto const port = resolve_structural_sample_target(graph_, target))
            inputs_[{port->bundle, PortKind::sample, port->port}].concrete_knob |= concrete_knob;
    }

public:
    explicit SampleInputTakeovers(ConfiguredGraph const& graph) : graph_(graph)
    {
        auto const samples = graph.connections.configured_sample_connections();
        if (std::ranges::none_of(samples, [](auto const& connection) {
                return connection.source_owned || connection.target_symbolic_node.has_value();
            })) return;

        std::set<NodeBundleHandle> manual_sources;
        for (auto const& control : graph.manual_controls) {
            if (control.coverage_source) manual_sources.insert(*control.coverage_source);
            if (control.id.kind != ManualControlKind::input || control.output || !control.enabled) continue;
            if (control.input.node_bundle_handle >= graph.node_bundles.size()) continue;
            auto const& bundle = graph.node_bundles.bundle(control.input.node_bundle_handle);
            bool const concrete_knob = !control.input_symbolic_node
                && control.participation == ManualControlParticipation::disconnected_only;
            if (!bundle.is_tiled()) {
                inputs_[control.input].concrete_knob |= concrete_knob;
                continue;
            }
            for (auto const target : graph.node_bundles.sample_input_channels(control.input)) claim(target, concrete_knob);
        }
        for (auto const& connection : samples) {
            // The generated Coverage wire carries the knob's selected value.
            // It cannot make a disabled knob count as a settled takeover.
            if (connection.source_owned || std::ranges::all_of(connection.source_channels,
                    [&](auto const& source) { return manual_sources.contains(source.channel.bundle); })) continue;
            for (auto const target : connection.target_channels) claim(target);
        }
    }

    bool replaces(ConfiguredSampleConnection const& connection, SampleInputChannelId target) const
    {
        if ((!connection.source_owned && !connection.target_symbolic_node) || inputs_.empty()) return false;
        auto const resolved = resolve_structural_sample_target(graph_, target);
        if (!resolved) return false;
        auto const claim = inputs_.find({resolved->bundle, PortKind::sample, resolved->port});
        return claim != inputs_.end() && (connection.source_owned || claim->second.concrete_knob);
    }
};

} // namespace iv::graph_jit::detail
