#pragma once

#include <intravenous/graph/builder/node_bundles.hpp>
#include <intravenous/continuous_value.h>
#include <intravenous/ports.h>

#include <optional>
#include <cstddef>
#include <string>
#include <variant>
#include <vector>

namespace iv {
// An index counts ports of the same kind and direction, as in ProjectPortMatcher.
struct NodeInstancePortInfo {
    std::string name{};
    std::size_t index = 0;
    std::optional<ChannelTypeId> channel_type{};
    std::optional<EventTypeId> event_type{};
    std::vector<ContinuousValueIntrospectionMetadata> value_types{};
};

struct NodeInstanceInterface {
    std::vector<NodeInstancePortInfo> sample_inputs{};
    std::vector<NodeInstancePortInfo> sample_outputs{};
    std::vector<NodeInstancePortInfo> event_inputs{};
    std::vector<NodeInstancePortInfo> event_outputs{};
    bool has_children = false;
};

// Copy the public interface while its builder/configured-result owners are alive.
// Reading it neither finishes a graph nor produces an executable generation.
inline NodeInstanceInterface read_node_instance_interface(
    GraphBuilderNodeBundles const& bundles, NodeBundleHandle root)
{
    NodeInstanceInterface result;
    auto const& bundle = bundles.bundle(root);
    for (std::size_t index = 0; index < bundle.sample_input_count(); ++index) {
        auto const config = bundles.resolve_sample_input({root, PortKind::sample, index}).config;
        result.sample_inputs.push_back({
            .name = config.name, .index = index,
            .channel_type = config.channel_layout.channel_type,
            .value_types = std::visit([](auto const& access) {
                return continuous_introspection_metadata(access.value_metadata);
            }, config.access),
        });
    }
    for (std::size_t index = 0; index < bundle.sample_output_count(); ++index) {
        auto const config = bundles.resolve_sample_output({root, PortKind::sample, index}).config;
        result.sample_outputs.push_back({
            .name = config.name, .index = index,
            .channel_type = config.channel_layout.channel_type,
            .value_types = std::visit([](auto const& production) {
                return continuous_introspection_metadata(production.value_metadata);
            }, config.production),
        });
    }
    for (std::size_t index = 0; index < bundle.event_input_count(); ++index) {
        auto const config = bundles.resolve_event_input({root, PortKind::event, index}).config;
        result.event_inputs.push_back({.name = config.name, .index = index, .event_type = config.type});
    }
    for (std::size_t index = 0; index < bundle.event_output_count(); ++index) {
        auto const config = bundles.resolve_event_output({root, PortKind::event, index}).config;
        result.event_outputs.push_back({.name = config.name, .index = index, .event_type = config.type});
    }
    result.has_children = bundle.is_subgraph();
    return result;
}
} // namespace iv
