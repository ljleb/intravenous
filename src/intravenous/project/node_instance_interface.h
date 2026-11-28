#pragma once

#include <intravenous/graph/builder/node_bundles.hpp>
#include <intravenous/graph/builder/symbolic_nodes.hpp>
#include <intravenous/continuous_value.h>
#include <intravenous/node/manual_control.h>
#include <intravenous/ports.h>

#include <algorithm>
#include <optional>
#include <cstddef>
#include <string>
#include <variant>
#include <vector>

namespace iv {
struct ManualInputDefaults {
    Sample value{};
    bool enabled = false;
};

// An index counts ports of the same kind and direction, as in ProjectPortMatcher.
struct NodeInstancePortInfo {
    std::string name{};
    std::size_t index = 0;
    std::string display_name{};
    std::string short_display_name{};
    std::optional<ChannelTypeId> channel_type{};
    std::optional<EventTypeId> event_type{};
    std::vector<ContinuousValueIntrospectionMetadata> value_types{};
    std::optional<ManualInputDefaults> manual_defaults{};
    std::optional<InputAccessKind> access{};
    std::optional<OutputProductionKind> production{};
    std::optional<OutputRealization> realization{};
};

inline std::optional<ManualInputDefaults> manual_input_defaults(ContinuousInputConfig const& config)
{
    using Type = ContinuousValueType<Sample>;
    if (is_sequential(config)) {
        auto const& metadata = sequential_input_properties(config).value_metadata;
        if (metadata.size() == 1)
            if (auto const* sample = std::get_if<Type::SequentialInputMetadata>(&metadata.front())) {
                auto const range = circular_manual_control_range(config);
                return ManualInputDefaults{range ? range->canonical(sample->default_value) : sample->default_value,
                    sample->enabled_by_default};
            }
    } else if (supports_coverage_manual_control(config)) {
        auto const& metadata = std::get<CoverageContinuousInputProperties>(config.access).value_metadata;
        auto const& sample = std::get<Type::CoverageInputMetadata>(metadata.front());
        return ManualInputDefaults{sample.default_value, sample.enabled_by_default};
    }
    return std::nullopt;
}

struct NodeInstanceInterface {
    std::vector<NodeInstancePortInfo> sample_inputs{};
    std::vector<NodeInstancePortInfo> sample_outputs{};
    std::vector<NodeInstancePortInfo> event_inputs{};
    std::vector<NodeInstancePortInfo> event_outputs{};
    bool has_children = false;
    bool is_module = false;
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
            .name = config.name, .index = index, .display_name = config.display_name,
            .short_display_name = config.short_display_name,
            .channel_type = config.channel_layout.channel_type,
            .value_types = std::visit([](auto const& access) {
                return continuous_introspection_metadata(access.value_metadata);
            }, config.access),
            .manual_defaults = manual_owner || supports_coverage_manual_control(config)
                ? manual_input_defaults(config) : std::nullopt,
            .access = input_access(config),
        });
    }
    for (std::size_t index = 0; index < bundle.sample_output_count(); ++index) {
        auto const config = bundles.resolve_sample_output({root, PortKind::sample, index}).config;
        result.sample_outputs.push_back({
            .name = config.name, .index = index, .display_name = config.display_name,
            .short_display_name = config.short_display_name,
            .channel_type = config.channel_layout.channel_type,
            .value_types = std::visit([](auto const& production) {
                return continuous_introspection_metadata(production.value_metadata);
            }, config.production),
            .production = output_production(config), .realization = config.realization,
        });
    }
    for (std::size_t index = 0; index < bundle.event_input_count(); ++index) {
        auto const config = bundles.resolve_event_input({root, PortKind::event, index}).config;
        result.event_inputs.push_back({.name = config.name, .index = index,
            .display_name = config.display_name,
            .short_display_name = config.short_display_name, .event_type = config.type,
            .access = input_access(config)});
    }
    for (std::size_t index = 0; index < bundle.event_output_count(); ++index) {
        auto const config = bundles.resolve_event_output({root, PortKind::event, index}).config;
        result.event_outputs.push_back({.name = config.name, .index = index,
            .display_name = config.display_name,
            .short_display_name = config.short_display_name, .event_type = config.type,
            .production = output_production(config), .realization = config.realization});
    }
    result.is_module = bundle.is_subgraph();
    result.has_children = result.is_module;
    return result;
}

inline NodeInstanceInterface read_symbolic_node_interface(GraphBuilderNodeBundles const& bundles,
    SymbolicNodeRecord const& record, NodeBundleHandle representative)
{
    auto result = read_node_instance_interface(bundles, representative);
    if (record.type_identity == "sample-port" || record.type_identity == "event-port") {
        result = {};
        for (auto const& port : record.sample_outputs)
            result.sample_outputs.push_back({
                .name = port.name, .index = port.index, .channel_type = port.channel_layout.channel_type,
            });
        for (auto const& port : record.event_outputs)
            result.event_outputs.push_back({.name = port.name, .index = port.index, .event_type = port.type});
    }
    auto label = [](auto& ports, auto const& mappings, std::string_view fallback) {
        for (auto& port : ports) {
            auto const mapping = std::ranges::find(mappings, port.index, &std::decay_t<decltype(mappings.front())>::index);
            if (mapping != mappings.end()) {
                port.name = mapping->name;
                if (!mapping->display_name.empty()) {
                    port.display_name = mapping->display_name;
                    port.short_display_name = mapping->short_display_name;
                }
            }
            if (!port.name.empty() || !port.display_name.empty()) continue;
            port.display_name = fallback;
            if (ports.size() > 1) port.display_name += " " + std::to_string(port.index + 1);
        }
    };
    label(result.sample_inputs, record.sample_inputs, "Input");
    label(result.sample_outputs, record.sample_outputs, "Value");
    label(result.event_inputs, record.event_inputs, "Event Input");
    label(result.event_outputs, record.event_outputs, "Event Output");
    result.has_children = true;
    result.is_module = false;
    return result;
}
} // namespace iv
