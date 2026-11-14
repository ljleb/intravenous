#pragma once

#include <intravenous/graph/builder/node_bundles.hpp>
#include <intravenous/continuous_value.h>
#include <intravenous/ports.h>

#include <algorithm>
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
    // Present only for the scalar Sample Sequential input supported by the
    // manual-control backend. Other continuous types are not numeric knobs.
    std::optional<Sample> manual_default{};
};

inline std::optional<Sample> manual_control_default_value(ContinuousInputConfig const& config)
{
    if (!is_sequential(config)) return std::nullopt;
    auto const& metadata = sequential_input_properties(config).value_metadata;
    if (metadata.size() != 1) return std::nullopt;
    if (auto const* sample = std::get_if<ContinuousValueType<Sample>::SequentialInputMetadata>(&metadata.front()))
        return sample->default_value;
    return std::nullopt;
}

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
    auto tick_input_owner = [&](auto&& self, NodeBundleHandle handle) -> bool {
        auto const& candidate = bundles.bundle(handle);
        if (candidate.is_concrete()) return bundles.materialize_concrete_description(handle).operations.runtime.tick_block != nullptr;
        if (candidate.is_tiled()) return std::ranges::all_of(candidate.tiled_members(), [&](auto member) { return self(self, member); });
        return candidate.is_subgraph();
    };
    bool const manual_owner = tick_input_owner(tick_input_owner, root);
    for (std::size_t index = 0; index < bundle.sample_input_count(); ++index) {
        auto const config = bundles.resolve_sample_input({root, PortKind::sample, index}).config;
        result.sample_inputs.push_back({
            .name = config.name, .index = index,
            .channel_type = config.channel_layout.channel_type,
            .value_types = std::visit([](auto const& access) {
                return continuous_introspection_metadata(access.value_metadata);
            }, config.access),
            .manual_default = manual_owner ? manual_control_default_value(config) : std::nullopt,
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
