#include <intravenous/graph/builder/connections.h>

#include <intravenous/graph/builder/topology.h>

#include <algorithm>
#include <charconv>
#include <system_error>
#include <unordered_map>

namespace iv {
namespace {
struct ParsedChannelPortName {
    ChannelTypeId channel_type = ChannelTypeId::mono;
    size_t family_ordinal = 0;
    size_t channel_ordinal = 0;
};

std::optional<ParsedChannelPortName> parse_channel_port_name(std::string const& name)
{
    auto const mono_prefix = std::string_view("__mono_center_");
    auto const stereo_left_prefix = std::string_view("__stereo_left_");
    auto const stereo_right_prefix = std::string_view("__stereo_right_");
    auto const parse_suffix = [&](std::string_view prefix) -> std::optional<size_t> {
        if (!std::string_view(name).starts_with(prefix)) {
            return std::nullopt;
        }
        size_t value = 0;
        auto const suffix = std::string_view(name).substr(prefix.size());
        auto const [ptr, ec] =
            std::from_chars(suffix.data(), suffix.data() + suffix.size(), value);
        if (ec != std::errc{} || ptr != suffix.data() + suffix.size()) {
            return std::nullopt;
        }
        return value;
    };

    if (auto const family_ordinal = parse_suffix(mono_prefix)) {
        return ParsedChannelPortName{
            .channel_type = ChannelTypeId::mono,
            .family_ordinal = *family_ordinal,
            .channel_ordinal = 0,
        };
    }
    if (auto const family_ordinal = parse_suffix(stereo_left_prefix)) {
        return ParsedChannelPortName{
            .channel_type = ChannelTypeId::stereo,
            .family_ordinal = *family_ordinal,
            .channel_ordinal = 0,
        };
    }
    if (auto const family_ordinal = parse_suffix(stereo_right_prefix)) {
        return ParsedChannelPortName{
            .channel_type = ChannelTypeId::stereo,
            .family_ordinal = *family_ordinal,
            .channel_ordinal = 1,
        };
    }
    return std::nullopt;
}

std::string multichannel_member_group_key(BuilderNode const& node, size_t node_i)
{
    bool has_channel_ports = false;
    for (auto const& input : node.inputs()) {
        has_channel_ports = has_channel_ports || parse_channel_port_name(input.name).has_value();
    }
    for (auto const& output : node.outputs()) {
        has_channel_ports = has_channel_ports || parse_channel_port_name(output.name).has_value();
    }
    if (!has_channel_ports) {
        return "#node:" + std::to_string(node_i);
    }
    for (auto const& info : node.source_annotations.infos) {
        if (!info.declaration_identity.empty()) {
            return "#decl:" + info.declaration_identity;
        }
    }
    return "#node:" + std::to_string(node_i);
}

std::unordered_map<std::string, std::unordered_set<std::string>> types_by_logical_id_for(
    GraphBuilderTopology const& topology)
{
    std::unordered_map<std::string, std::unordered_set<std::string>> types_by_logical_id;
    for (size_t node_i = 0; node_i < topology.node_count(); ++node_i) {
        auto const& node = topology.node(node_i);
        if (node.materialization.is_placeholder() || node.vacant_input_ownership.logical_node_id.empty()) {
            continue;
        }
        types_by_logical_id[node.vacant_input_ownership.logical_node_id].insert(node.type_identity.value);
    }
    return types_by_logical_id;
}

std::string final_logical_node_id_for(
    std::unordered_map<std::string, std::unordered_set<std::string>> const& types_by_logical_id,
    BuilderNode const& node)
{
    std::string final_logical_node_id = node.vacant_input_ownership.logical_node_id;
    if (auto it = types_by_logical_id.find(node.vacant_input_ownership.logical_node_id);
        it != types_by_logical_id.end() && it->second.size() > 1) {
        final_logical_node_id += "#type:" + details::stable_identity_suffix(node.type_identity.value);
    }
    return final_logical_node_id;
}
} // namespace

bool GraphBuilderConnections::sample_input_is_connected(PortId target) const
{
    return _placed_sample_inputs.contains(target);
}

bool GraphBuilderConnections::event_input_is_connected(PortId target) const
{
    return _placed_event_inputs.contains(target);
}

void GraphBuilderConnections::connect_sample_input(
    GraphBuilderTopology& topology,
    GraphBuilderIdentity const& identity,
    PortId target,
    SamplePortRef source
)
{
    if (source.graph_builder == nullptr) {
        details::error("builder " + identity.value + ": empty SamplePortRef");
    }
    if (target.node >= topology.node_count() || target.port >= topology.node(target.node).inputs().size()) {
        details::error("sample input target is out of bounds in builder " + identity.value);
    }
    _placed_sample_inputs.insert(target);
    topology.add_sample_edge(GraphEdge{ source, target });
}

void GraphBuilderConnections::connect_event_input(
    GraphBuilderTopology& topology,
    std::span<EventInputConfig const> graph_event_inputs,
    GraphBuilderIdentity const& identity,
    PortId target,
    EventPortRef source
)
{
    if (source.graph_builder == nullptr) {
        details::error("builder " + identity.value + ": empty EventPortRef");
    }
    if (target.node >= topology.node_count() || target.port >= topology.node(target.node).event_inputs().size()) {
        details::error("event input target is out of bounds in builder " + identity.value);
    }
    auto const source_type = (source.node_index == GRAPH_ID)
        ? graph_event_inputs[source.output_port].type
        : topology.node(source.node_index).event_outputs()[source.output_port].type;
    auto const target_type = topology.node(target.node).event_inputs()[target.port].type;
    _placed_event_inputs.insert(target);
    topology.add_event_edge(GraphEventEdge{
        PortId{ source.node_index, source.output_port },
        target,
        EventConversionRegistry::instance().plan(source_type, target_type)
    });
}

void GraphBuilderConnections::mark_runtime_filled_sample_input(PortId target)
{
    _runtime_filled_sample_inputs.insert(target);
}

void GraphBuilderConnections::mark_runtime_filled_event_input(PortId target)
{
    _runtime_filled_event_inputs.insert(target);
}

GraphBuilderVacantInputs GraphBuilderConnections::collect_vacant_inputs(GraphBuilderTopology const& topology) const
{
    GraphBuilderVacantInputs result;
    size_t const original_node_count = topology.node_count();
    std::unordered_map<std::string, std::unordered_set<std::string>> types_by_logical_id;
    for (size_t node_i = 0; node_i < original_node_count; ++node_i) {
        auto const& node = topology.node(node_i);
        if (node.materialization.is_placeholder() || node.vacant_input_ownership.logical_node_id.empty()) {
            continue;
        }
        types_by_logical_id[node.vacant_input_ownership.logical_node_id].insert(node.type_identity.value);
    }
    std::unordered_map<std::string, size_t> next_member_ordinal_by_logical_id;
    for (size_t node_i = 0; node_i < original_node_count; ++node_i) {
        auto const& node = topology.node(node_i);
        if (node.materialization.is_placeholder()) {
            continue;
        }
        std::string const logical_node_id = node.vacant_input_ownership.logical_node_id;
        if (logical_node_id.empty()) {
            continue;
        }
        std::string final_logical_node_id = logical_node_id;
        if (auto it = types_by_logical_id.find(logical_node_id);
            it != types_by_logical_id.end() && it->second.size() > 1) {
            final_logical_node_id += "#type:" + details::stable_identity_suffix(node.type_identity.value);
        }
        size_t const member_ordinal = next_member_ordinal_by_logical_id[final_logical_node_id]++;
        for (size_t input_i = 0; input_i < node.inputs().size(); ++input_i) {
            PortId const target_port{ node_i, input_i };
            if (_placed_sample_inputs.contains(target_port)) {
                continue;
            }
            result.sample.push_back(GraphBuilderVacantSampleInput{
                .target = target_port,
                .logical_node_id = final_logical_node_id,
                .member_ordinal = member_ordinal,
                .config = node.inputs()[input_i],
            });
        }
        for (size_t input_i = 0; input_i < node.event_inputs().size(); ++input_i) {
            PortId const target_port{ node_i, input_i };
            if (_placed_event_inputs.contains(target_port)) {
                continue;
            }
            result.event.push_back(GraphBuilderVacantEventInput{
                .target = target_port,
                .logical_node_id = final_logical_node_id,
                .member_ordinal = member_ordinal,
                .config = node.event_inputs()[input_i],
            });
        }
    }
    return result;
}

GraphBuilderLogicalInputs GraphBuilderConnections::collect_logical_inputs(
    GraphBuilderTopology const& topology) const
{
    GraphBuilderLogicalInputs result;
    size_t const original_node_count = topology.node_count();
    auto const types_by_logical_id = types_by_logical_id_for(topology);

    std::unordered_map<std::string, size_t> next_member_ordinal_by_logical_id;
    for (size_t node_i = 0; node_i < original_node_count; ++node_i) {
        auto const& node = topology.node(node_i);
        if (node.materialization.is_placeholder()) {
            continue;
        }
        if (node.vacant_input_ownership.logical_node_id.empty()) {
            continue;
        }
        std::string const final_logical_node_id = final_logical_node_id_for(types_by_logical_id, node);
        size_t const member_ordinal = next_member_ordinal_by_logical_id[final_logical_node_id]++;
        for (size_t input_i = 0; input_i < node.inputs().size(); ++input_i) {
            PortId const target_port{ node_i, input_i };
            result.sample.push_back(GraphBuilderLogicalSampleInput{
                .target = target_port,
                .logical_node_id = final_logical_node_id,
                .member_ordinal = member_ordinal,
                .config = node.inputs()[input_i],
                .has_existing_connection = _placed_sample_inputs.contains(target_port),
                .runtime_filled = _runtime_filled_sample_inputs.contains(target_port),
            });
        }
        for (size_t input_i = 0; input_i < node.event_inputs().size(); ++input_i) {
            PortId const target_port{ node_i, input_i };
            result.event.push_back(GraphBuilderLogicalEventInput{
                .target = target_port,
                .logical_node_id = final_logical_node_id,
                .member_ordinal = member_ordinal,
                .config = node.event_inputs()[input_i],
                .has_existing_connection = _placed_event_inputs.contains(target_port),
                .runtime_filled = _runtime_filled_event_inputs.contains(target_port),
            });
        }
    }
    return result;
}

GraphBuilderLogicalSampleInputFamilies
GraphBuilderConnections::collect_logical_sample_input_families(
    GraphBuilderTopology const& topology) const
{
    struct FamilyKey {
        std::string logical_node_id {};
        std::string member_group_key {};
        size_t family_ordinal = 0;

        bool operator==(FamilyKey const&) const = default;
    };

    struct FamilyKeyHash {
        size_t operator()(FamilyKey const& key) const noexcept
        {
            size_t hash = std::hash<std::string>{}(key.logical_node_id);
            hash ^= std::hash<std::string>{}(key.member_group_key) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
            hash ^= std::hash<size_t>{}(key.family_ordinal) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
            return hash;
        }
    };

    struct PendingFamily {
        std::string logical_node_id {};
        std::string member_group_key {};
        size_t family_ordinal = 0;
        std::string family_name {};
        InputConfig config {};
        ChannelTypeId channel_type = ChannelTypeId::mono;
        std::vector<GraphBuilderLogicalSampleInputChannel> channels {};
    };

    GraphBuilderLogicalSampleInputFamilies result;
    auto const types_by_logical_id = types_by_logical_id_for(topology);
    std::unordered_map<FamilyKey, PendingFamily, FamilyKeyHash> families_by_key;
    std::vector<FamilyKey> family_order;

    for (size_t node_i = 0; node_i < topology.node_count(); ++node_i) {
        auto const& node = topology.node(node_i);
        if (node.materialization.is_placeholder() || node.vacant_input_ownership.logical_node_id.empty()) {
            continue;
        }
        auto const final_logical_node_id = final_logical_node_id_for(types_by_logical_id, node);
        auto const member_group_key = multichannel_member_group_key(node, node_i);
        for (size_t input_i = 0; input_i < node.inputs().size(); ++input_i) {
            auto const& config = node.inputs()[input_i];
            auto const parsed = parse_channel_port_name(config.name);
            auto const channel_type =
                parsed.has_value() ? parsed->channel_type : ChannelTypeId::mono;
            auto const family_ordinal =
                parsed.has_value() ? parsed->family_ordinal : input_i;
            auto const channel_ordinal =
                parsed.has_value() ? parsed->channel_ordinal : size_t{0};

            FamilyKey key{
                .logical_node_id = final_logical_node_id,
                .member_group_key = member_group_key,
                .family_ordinal = family_ordinal,
            };
            if (!families_by_key.contains(key)) {
                family_order.push_back(key);
                families_by_key.emplace(key, PendingFamily{
                    .logical_node_id = final_logical_node_id,
                    .member_group_key = member_group_key,
                    .family_ordinal = family_ordinal,
                    .family_name = parsed.has_value()
                        ? "channel_" + std::to_string(family_ordinal)
                        : config.name,
                    .config = config,
                    .channel_type = channel_type,
                    .channels = std::vector<GraphBuilderLogicalSampleInputChannel>(
                        channel_count(channel_type)),
                });
            }

            auto& family = families_by_key.at(key);
            if (family.channel_type != channel_type) {
                details::error(
                    "conflicting channel family types for logical node " + final_logical_node_id);
            }
            if (channel_ordinal >= family.channels.size()) {
                details::error("channel ordinal out of bounds while collecting logical sample inputs");
            }
            auto& channel = family.channels[channel_ordinal];
            if (channel.target.has_value()) {
                details::error(
                    "duplicate channel contributor for logical sample input family on " + final_logical_node_id);
            }
            PortId const target_port{ node_i, input_i };
            channel.target = target_port;
            channel.has_existing_connection = _placed_sample_inputs.contains(target_port);
            channel.runtime_filled = _runtime_filled_sample_inputs.contains(target_port);
        }
    }

    std::unordered_map<std::string, std::vector<FamilyKey>> families_by_logical_member;
    for (auto const& key : family_order) {
        families_by_logical_member[key.logical_node_id + "\x1f" + key.member_group_key].push_back(key);
    }
    std::unordered_map<std::string, size_t> member_ordinal_by_logical_member;
    for (auto& [_, keys] : families_by_logical_member) {
        std::ranges::sort(keys, [](auto const& a, auto const& b) {
            return a.family_ordinal < b.family_ordinal;
        });
    }

    std::vector<std::pair<FamilyKey, size_t>> ordered_keys;
    ordered_keys.reserve(family_order.size());
    std::unordered_map<std::string, bool> seen_member_groups;
    for (auto const& key : family_order) {
        auto const member_key = key.logical_node_id + "\x1f" + key.member_group_key;
        if (!seen_member_groups.emplace(member_key, true).second) {
            continue;
        }
        auto& keys = families_by_logical_member.at(member_key);
        size_t const member_ordinal = member_ordinal_by_logical_member[key.logical_node_id]++;
        for (auto const& family_key : keys) {
            ordered_keys.emplace_back(family_key, member_ordinal);
        }
    }

    result.families.reserve(ordered_keys.size());
    for (auto const& [key, member_ordinal] : ordered_keys) {
        auto family = std::move(families_by_key.at(key));
        result.families.push_back(GraphBuilderLogicalSampleInputFamily{
            .logical_node_id = std::move(family.logical_node_id),
            .member_ordinal = member_ordinal,
            .family_ordinal = family.family_ordinal,
            .family_name = std::move(family.family_name),
            .config = family.config,
            .channel_type = family.channel_type,
            .channels = std::move(family.channels),
        });
    }
    return result;
}

GraphBuilderLogicalOutputs GraphBuilderConnections::collect_logical_outputs(
    GraphBuilderTopology const& topology) const
{
    GraphBuilderLogicalOutputs result;
    size_t const original_node_count = topology.node_count();

    std::unordered_set<PortId> connected_sample_sources;
    std::unordered_set<PortId> connected_event_sources;
    topology.for_each_sample_edge([&](GraphEdge const& edge) {
        connected_sample_sources.insert(edge.source);
    });
    topology.for_each_event_edge([&](GraphEventEdge const& edge) {
        connected_event_sources.insert(edge.source);
    });

    auto const types_by_logical_id = types_by_logical_id_for(topology);

    std::unordered_map<std::string, size_t> next_member_ordinal_by_logical_id;
    for (size_t node_i = 0; node_i < original_node_count; ++node_i) {
        auto const& node = topology.node(node_i);
        if (node.materialization.is_placeholder()) {
            continue;
        }
        if (node.vacant_input_ownership.logical_node_id.empty()) {
            continue;
        }
        std::string const final_logical_node_id = final_logical_node_id_for(types_by_logical_id, node);
        size_t const member_ordinal = next_member_ordinal_by_logical_id[final_logical_node_id]++;
        for (size_t output_i = 0; output_i < node.outputs().size(); ++output_i) {
            PortId const source_port{ node_i, output_i };
            result.sample.push_back(GraphBuilderLogicalSampleOutput{
                .source = source_port,
                .logical_node_id = final_logical_node_id,
                .member_ordinal = member_ordinal,
                .config = node.outputs()[output_i],
                .has_existing_downstream_connection = connected_sample_sources.contains(source_port),
            });
        }
        for (size_t output_i = 0; output_i < node.event_outputs().size(); ++output_i) {
            PortId const source_port{ node_i, output_i };
            result.event.push_back(GraphBuilderLogicalEventOutput{
                .source = source_port,
                .logical_node_id = final_logical_node_id,
                .member_ordinal = member_ordinal,
                .config = node.event_outputs()[output_i],
                .has_existing_downstream_connection = connected_event_sources.contains(source_port),
            });
        }
    }
    return result;
}

GraphBuilderLogicalSampleOutputFamilies
GraphBuilderConnections::collect_logical_sample_output_families(
    GraphBuilderTopology const& topology) const
{
    struct FamilyKey {
        std::string logical_node_id {};
        std::string member_group_key {};
        size_t family_ordinal = 0;

        bool operator==(FamilyKey const&) const = default;
    };

    struct FamilyKeyHash {
        size_t operator()(FamilyKey const& key) const noexcept
        {
            size_t hash = std::hash<std::string>{}(key.logical_node_id);
            hash ^= std::hash<std::string>{}(key.member_group_key) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
            hash ^= std::hash<size_t>{}(key.family_ordinal) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
            return hash;
        }
    };

    struct PendingFamily {
        std::string logical_node_id {};
        std::string member_group_key {};
        size_t family_ordinal = 0;
        std::string family_name {};
        OutputConfig config {};
        ChannelTypeId channel_type = ChannelTypeId::mono;
        std::vector<GraphBuilderLogicalSampleOutputChannel> channels {};
    };

    std::unordered_set<PortId> connected_sample_sources;
    topology.for_each_sample_edge([&](GraphEdge const& edge) {
        connected_sample_sources.insert(edge.source);
    });

    GraphBuilderLogicalSampleOutputFamilies result;
    auto const types_by_logical_id = types_by_logical_id_for(topology);
    std::unordered_map<FamilyKey, PendingFamily, FamilyKeyHash> families_by_key;
    std::vector<FamilyKey> family_order;

    for (size_t node_i = 0; node_i < topology.node_count(); ++node_i) {
        auto const& node = topology.node(node_i);
        if (node.materialization.is_placeholder() || node.vacant_input_ownership.logical_node_id.empty()) {
            continue;
        }
        auto const final_logical_node_id = final_logical_node_id_for(types_by_logical_id, node);
        auto const member_group_key = multichannel_member_group_key(node, node_i);
        for (size_t output_i = 0; output_i < node.outputs().size(); ++output_i) {
            auto const& config = node.outputs()[output_i];
            auto const parsed = parse_channel_port_name(config.name);
            auto const channel_type =
                parsed.has_value() ? parsed->channel_type : ChannelTypeId::mono;
            auto const family_ordinal =
                parsed.has_value() ? parsed->family_ordinal : output_i;
            auto const channel_ordinal =
                parsed.has_value() ? parsed->channel_ordinal : size_t{0};

            FamilyKey key{
                .logical_node_id = final_logical_node_id,
                .member_group_key = member_group_key,
                .family_ordinal = family_ordinal,
            };
            if (!families_by_key.contains(key)) {
                family_order.push_back(key);
                families_by_key.emplace(key, PendingFamily{
                    .logical_node_id = final_logical_node_id,
                    .member_group_key = member_group_key,
                    .family_ordinal = family_ordinal,
                    .family_name = parsed.has_value()
                        ? "channel_" + std::to_string(family_ordinal)
                        : config.name,
                    .config = config,
                    .channel_type = channel_type,
                    .channels = std::vector<GraphBuilderLogicalSampleOutputChannel>(
                        channel_count(channel_type)),
                });
            }

            auto& family = families_by_key.at(key);
            if (family.channel_type != channel_type) {
                details::error(
                    "conflicting channel family types for logical node " + final_logical_node_id);
            }
            if (channel_ordinal >= family.channels.size()) {
                details::error("channel ordinal out of bounds while collecting logical sample outputs");
            }
            auto& channel = family.channels[channel_ordinal];
            if (channel.source.has_value()) {
                details::error(
                    "duplicate channel contributor for logical sample output family on " + final_logical_node_id);
            }
            PortId const source_port{ node_i, output_i };
            channel.source = source_port;
            channel.has_existing_downstream_connection = connected_sample_sources.contains(source_port);
        }
    }

    std::unordered_map<std::string, std::vector<FamilyKey>> families_by_logical_member;
    for (auto const& key : family_order) {
        families_by_logical_member[key.logical_node_id + "\x1f" + key.member_group_key].push_back(key);
    }
    std::unordered_map<std::string, size_t> member_ordinal_by_logical_member;
    for (auto& [_, keys] : families_by_logical_member) {
        std::ranges::sort(keys, [](auto const& a, auto const& b) {
            return a.family_ordinal < b.family_ordinal;
        });
    }

    std::vector<std::pair<FamilyKey, size_t>> ordered_keys;
    ordered_keys.reserve(family_order.size());
    std::unordered_map<std::string, bool> seen_member_groups;
    for (auto const& key : family_order) {
        auto const member_key = key.logical_node_id + "\x1f" + key.member_group_key;
        if (!seen_member_groups.emplace(member_key, true).second) {
            continue;
        }
        auto& keys = families_by_logical_member.at(member_key);
        size_t const member_ordinal = member_ordinal_by_logical_member[key.logical_node_id]++;
        for (auto const& family_key : keys) {
            ordered_keys.emplace_back(family_key, member_ordinal);
        }
    }

    result.families.reserve(ordered_keys.size());
    for (auto const& [key, member_ordinal] : ordered_keys) {
        auto family = std::move(families_by_key.at(key));
        result.families.push_back(GraphBuilderLogicalSampleOutputFamily{
            .logical_node_id = std::move(family.logical_node_id),
            .member_ordinal = member_ordinal,
            .family_ordinal = family.family_ordinal,
            .family_name = std::move(family.family_name),
            .config = family.config,
            .channel_type = family.channel_type,
            .channels = std::move(family.channels),
        });
    }
    return result;
}

void GraphBuilderConnections::import_child(GraphBuilderConnections const& child, size_t child_node_offset)
{
    for (PortId const port : child._placed_sample_inputs) {
        if (port.node != GRAPH_ID) {
            _placed_sample_inputs.insert(PortId{ child_node_offset + port.node, port.port });
        }
    }
    for (PortId const port : child._placed_event_inputs) {
        if (port.node != GRAPH_ID) {
            _placed_event_inputs.insert(PortId{ child_node_offset + port.node, port.port });
        }
    }
    for (PortId const port : child._runtime_filled_sample_inputs) {
        if (port.node != GRAPH_ID) {
            _runtime_filled_sample_inputs.insert(PortId{ child_node_offset + port.node, port.port });
        }
    }
    for (PortId const port : child._runtime_filled_event_inputs) {
        if (port.node != GRAPH_ID) {
            _runtime_filled_event_inputs.insert(PortId{ child_node_offset + port.node, port.port });
        }
    }
}
}
