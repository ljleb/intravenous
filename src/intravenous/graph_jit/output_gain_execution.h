#pragma once

#include <intravenous/graph/configured_graph.hpp>

#include <algorithm>
#include <map>

namespace iv::graph_jit::detail {
inline void classify_output_gain_consumers(ConfiguredGraph& graph)
{
    if (std::ranges::none_of(graph.manual_controls, [](auto const& control) { return control.output.has_value(); })) return;

    struct Consumer {
        NodeBundleHandle bundle;
        bool random_access;
        std::optional<std::size_t> occurrence;
    };
    auto const& bundles = graph.node_bundles;
    std::map<NodeBundlePortId, std::vector<Consumer>, NodeBundlePortIdLess> consumers;
    std::vector<std::vector<NodeBundleHandle>> sources(bundles.size());
    std::vector<bool> prepares(bundles.size(), false);
    std::vector<NodeBundleHandle> work;
    auto require_preparation = [&](NodeBundleHandle bundle) {
        if (prepares[bundle]) return;
        prepares[bundle] = true;
        work.push_back(bundle);
    };
    auto alias = [&](NodeBundleHandle left, NodeBundleHandle right) {
        sources[left].push_back(right);
        sources[right].push_back(left);
    };
    bundles.for_each_configured_bundle([&, bundle = NodeBundleHandle{}](ConfiguredNodeBundleView const& view) mutable {
        if (view.operations && !view.operations->runtime.tick_block) require_preparation(bundle);
        auto const& node = bundles.bundle(bundle);
        if (auto boundary = node.subgraph_boundary_handle()) alias(bundle, *boundary);
        if (node.is_tiled()) for (auto member : node.tiled_members()) alias(bundle, member);
        ++bundle;
    });
    for (auto const& connection : graph.connections.configured_sample_connections()) {
        for (auto const target : connection.target_channels) {
            auto const config = bundles.resolve_sample_input({target.bundle, PortKind::sample, target.port}).config;
            auto const random_access = !is_sequential(config);
            for (auto const& contribution : connection.source_channels) {
                auto channel = contribution.channel;
                auto occurrence = contribution.node_ref_occurrence;
                sources[target.bundle].push_back(channel.bundle);
                if (random_access) require_preparation(channel.bundle);
                for (;;) {
                    consumers[{channel.bundle, PortKind::sample, channel.port}].push_back({target.bundle, random_access, occurrence});
                    // Gain on a tiled member also applies to that channel of
                    // a connection authored on the tiled output.
                    if (!bundles.bundle(channel.bundle).is_tiled()) break;
                    channel = {bundles.tiled_member(channel.bundle, channel.channel), channel.port, 0};
                    occurrence.reset();
                }
            }
        }
    }
    for (auto const& connection : graph.connections.configured_event_connections()) {
        for (auto const target : connection.targets) {
            auto const config = bundles.resolve_event_input({target.bundle, PortKind::event, target.port}).config;
            for (auto const& source : connection.sources) {
                sources[target.bundle].push_back(source.port.bundle);
                if (!is_sequential(config)) require_preparation(source.port.bundle);
            }
        }
    }
    for (std::size_t index = 0; index < work.size(); ++index)
        for (auto source : sources[work[index]]) require_preparation(source);

    for (auto& control : graph.manual_controls) {
        if (!control.output) continue;
        auto found = consumers.find(*control.output);
        if (found == consumers.end()) continue;
        control.requires_preparation |= std::ranges::any_of(found->second, [&](Consumer const& consumer) {
            if (!consumer.random_access && !prepares[consumer.bundle]) return false;
            if (!control.output_symbolic_node) return true;
            auto const occurrences = bundles.bundle(control.output->node_bundle_handle).node_ref_occurrences();
            for (auto index = consumer.occurrence; index && *index < occurrences.size(); index = occurrences[*index].upstream)
                if (occurrences[*index].symbolic_node_handle == control.output_symbolic_node) return true;
            return false;
        });
    }
}
} // namespace iv::graph_jit::detail
