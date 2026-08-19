#include <intravenous/graph/builder/public_ports.h>

#include <intravenous/graph/builder.h>
#include <intravenous/graph/builder/topology.h>

#include <algorithm>
#include <limits>
#include <type_traits>

namespace iv {
namespace {
template<class Config>
GraphBuilderPublicSamplePortFamilies collect_sample_port_families(
    std::span<Config const> configs,
    std::span<PublicSamplePortMember const> members,
    bool input)
{
    IV_ASSERT(configs.size() == members.size(), "public sample port metadata must align with configs");
    GraphBuilderPublicSamplePortFamilies result;
    result.families.reserve(configs.size());

    for (size_t port_ordinal = 0; port_ordinal < configs.size(); ++port_ordinal) {
        auto const& config = configs[port_ordinal];
        auto const& member = members[port_ordinal];
        auto const channel_type = member.channel_type;
        auto const family_ordinal = port_ordinal;
        auto const channel_index = member.channel_index;

        auto family_it = std::find_if(
            result.families.begin(),
            result.families.end(),
            [&](GraphBuilderPublicSamplePortFamily const& family) {
                return !member.family_name.empty()
                    && family.family_name == member.family_name;
            });
        if (family_it == result.families.end()) {
            GraphBuilderPublicSamplePortFamily family{
                .family_ordinal = family_ordinal,
                .family_name = member.family_name.empty() ? config.name : member.family_name,
                .channel_type = channel_type,
                .channels = std::vector<GraphBuilderPublicSamplePortChannel>(
                    channel_count(channel_type)),
            };
            if constexpr (std::is_same_v<Config, InputConfig>) {
                family.input_config = config;
            } else {
                family.output_config = config;
            }
            result.families.push_back(std::move(family));
            family_it = std::prev(result.families.end());
        }

        if (family_it->channel_type != channel_type) {
            details::error(
                (input
                    ? "conflicting public sample input channel types for '"
                    : "conflicting public sample output channel types for '")
                + config.name + "'");
        }
        if (member.whole_stream) {
            for (auto& channel : family_it->channels) {
                channel.port_ordinals.push_back(port_ordinal);
            }
            continue;
        }
        if (channel_index >= family_it->channels.size()) {
            details::error(
                input
                    ? "public sample input channel ordinal out of bounds"
                    : "public sample output channel ordinal out of bounds");
        }
        if (input && !family_it->channels[channel_index].port_ordinals.empty()) {
            details::error(
                input
                    ? "duplicate public sample input channel contributor"
                    : "duplicate public sample output channel contributor");
        }
        family_it->channels[channel_index].port_ordinals.push_back(port_ordinal);
    }

    return result;
}
} // namespace

SamplePortRef GraphBuilderPublicPorts::add_sample_input(
    GraphBuilder& builder,
    GraphBuilderNodeBundles& node_bundles,
    std::string_view name,
    Sample default_value,
    std::optional<Sample> min,
    std::optional<Sample> max)
{
    auto& boundary = node_bundles.bundle(_boundary);
    auto const ordinal = boundary.append_boundary_sample_input(InputConfig{
        .name = std::string(name),
        .default_value = default_value,
        .min = min.value_or(-std::numeric_limits<Sample::storage>::infinity()),
        .max = max.value_or(std::numeric_limits<Sample::storage>::infinity()),
    });
    _sample_input_source_infos.emplace_back();
    return SamplePortRef(
        builder, GraphInputPortId{PortKind::sample, ordinal});
}

void GraphBuilderPublicPorts::annotate_sample_input_source_info(
    size_t port_ordinal,
    std::string_view declaration_identity,
    std::string_view file_path,
    uint32_t begin,
    uint32_t end)
{
    if (port_ordinal >= _sample_input_source_infos.size()) {
        return;
    }
    SourceInfo info{
        .declaration_identity = std::string(declaration_identity),
        .span = SourceSpan{.file_path = std::string(file_path), .begin = begin, .end = end},
    };
    auto& infos = _sample_input_source_infos[port_ordinal];
    if (std::find(infos.begin(), infos.end(), info) == infos.end()) {
        infos.push_back(std::move(info));
    }
}

std::span<SourceInfo const> GraphBuilderPublicPorts::sample_input_source_infos(size_t port_ordinal) const
{
    if (port_ordinal >= _sample_input_source_infos.size()) {
        return {};
    }
    return _sample_input_source_infos[port_ordinal];
}

EventPortRef GraphBuilderPublicPorts::add_event_input(
    GraphBuilder& builder, GraphBuilderNodeBundles& node_bundles,
    std::string_view name, EventTypeId type)
{
    auto config = !name.empty()
        ? EventInputConfig{ .name = std::string(name), .type = type }
        : EventInputConfig{ .type = type };
    auto& boundary = node_bundles.bundle(_boundary);
    auto const ordinal = boundary.append_boundary_event_input(std::move(config));
    _event_input_source_infos.emplace_back();
    return EventPortRef(
        builder, GraphInputPortId{PortKind::event, ordinal});
}

void GraphBuilderPublicPorts::annotate_event_input_source_info(size_t ordinal, std::string_view identity,
    std::string_view file, uint32_t begin, uint32_t end)
{
    if (ordinal >= _event_input_source_infos.size() || identity.empty()) return;
    SourceInfo info{.declaration_identity = std::string(identity), .span = SourceSpan{.file_path = std::string(file), .begin = begin, .end = end}};
    auto &infos = _event_input_source_infos[ordinal];
    if (std::find(infos.begin(), infos.end(), info) == infos.end()) infos.push_back(std::move(info));
}

std::span<SourceInfo const> GraphBuilderPublicPorts::event_input_source_infos(size_t ordinal) const
{
    return ordinal < _event_input_source_infos.size() ? std::span<SourceInfo const>(_event_input_source_infos[ordinal]) : std::span<SourceInfo const>{};
}

bool GraphBuilderPublicPorts::sample_outputs_defined() const
{
    return _sample_outputs_defined;
}

void GraphBuilderPublicPorts::define_sample_outputs(
    GraphBuilder& builder,
    GraphBuilderTopology& topology,
    GraphBuilderNodeBundles& node_bundles,
    GraphBuilderIdentity const& identity,
    std::span<OutputRefConfig const> refs
)
{
    _last_sample_output_port_ordinals.clear();
    bool const require_names = refs.size() > 1;

    for (size_t i = 0; i < refs.size(); ++i) {
        auto const& ref = refs[i].ref;
        auto const& config = refs[i].config;
        if (ref.graph_builder == nullptr) {
            details::error("builder " + identity.value + ": outputs(...): empty SamplePortRef");
        }
        if (ref.graph_builder != &builder) {
            details::error(
                "builder " + identity.value + ": outputs(...): "
                "SamplePortRef at index " + std::to_string(i) + " "
                "belongs to another builder"
            );
        }
        if (require_names && config.name.empty()) {
            details::error(
                "builder " + identity.value
                + ": outputs(...) requires names when exposing more than one sample output"
            );
        }

        auto const existing = !refs[i].public_member.family_name.empty()
            ? std::find_if(_sample_output_members.begin(), _sample_output_members.end(), [&](PublicSamplePortMember const& candidate) {
                return candidate.family_name == refs[i].public_member.family_name
                    && candidate.channel_type == refs[i].public_member.channel_type
                    && candidate.channel_index == refs[i].public_member.channel_index
                    && candidate.whole_stream == refs[i].public_member.whole_stream;
            })
            : _sample_output_members.end();
        auto const output_ordinal = existing == _sample_output_members.end()
            ? node_bundles.bundle(_boundary).boundary_sample_outputs().size()
            : static_cast<size_t>(existing - _sample_output_members.begin());
        auto const source = builder.materialize_sample_output(ref).port;
        topology.add_sample_edge(TopologyEdge{
            source,
            TopologyPortId{ GRAPH_ID, output_ordinal },
        });
        if (existing == _sample_output_members.end()) {
            auto const appended = node_bundles.bundle(_boundary)
                .append_boundary_sample_output(config);
            IV_ASSERT(appended == output_ordinal,
                      "public sample output metadata must align with boundary ports");
            _sample_output_members.push_back(refs[i].public_member);
            _sample_output_source_infos.emplace_back();
        }
        _last_sample_output_port_ordinals.push_back(output_ordinal);
    }

    _sample_outputs_defined = true;
}

void GraphBuilderPublicPorts::define_event_outputs(
    GraphBuilder const& builder,
    GraphBuilderTopology& topology,
    GraphBuilderNodeBundles& node_bundles,
    GraphBuilderIdentity const& identity,
    std::span<EventOutputRefConfig const> refs
)
{
    auto& boundary = node_bundles.bundle(_boundary);
    boundary.clear_boundary_event_outputs();
    _event_output_source_infos.resize(refs.size());
    bool const require_names = refs.size() > 1;

    for (size_t i = 0; i < refs.size(); ++i) {
        auto const& ref = refs[i].ref;
        auto const& config = refs[i].config;
        if (ref.graph_builder == nullptr) {
            details::error("builder " + identity.value + ": event_outputs(...): empty EventPortRef");
        }
        if (ref.graph_builder != &builder) {
            details::error(
                "builder " + identity.value + ": event_outputs(...): "
                "EventPortRef at index " + std::to_string(i) + " "
                "belongs to another builder"
            );
        }
        if (require_names && config.name.empty()) {
            details::error(
                "builder " + identity.value
                + ": event_outputs(...) requires names when exposing more than one event output"
            );
        }

        auto const source_type = ref.graph_input_port
            ? boundary.boundary_event_inputs()[ref.graph_input_port->port_ordinal].type
            : ref.scope_boundary_port
                ? topology.scope_boundary_event_output(*ref.scope_boundary_port).type
                : topology.ports(ref.node_index).event_outputs()[ref.output_port].type;
        topology.add_event_edge(TopologyEventEdge{
            static_cast<TopologyPortId>(ref),
            TopologyPortId{ GRAPH_ID, i },
            EventConversionRegistry::instance().plan(source_type, source_type)
        });
        auto output_config = config;
        output_config.type = source_type;
        boundary.append_boundary_event_output(std::move(output_config));
    }
}

std::span<InputConfig const> GraphBuilderPublicPorts::sample_inputs(
    GraphBuilderNodeBundles const& node_bundles) const
{
    return node_bundles.bundle(_boundary).boundary_sample_inputs();
}

std::span<EventInputConfig const> GraphBuilderPublicPorts::event_inputs(
    GraphBuilderNodeBundles const& node_bundles) const
{
    return node_bundles.bundle(_boundary).boundary_event_inputs();
}

std::span<OutputConfig const> GraphBuilderPublicPorts::sample_outputs(
    GraphBuilderNodeBundles const& node_bundles) const
{
    return node_bundles.bundle(_boundary).boundary_sample_outputs();
}

std::span<EventOutputConfig const> GraphBuilderPublicPorts::event_outputs(
    GraphBuilderNodeBundles const& node_bundles) const
{
    return node_bundles.bundle(_boundary).boundary_event_outputs();
}

GraphBuilderPublicSamplePortFamilies GraphBuilderPublicPorts::sample_input_families(
    GraphBuilderNodeBundles const& node_bundles) const
{
    auto const configs = sample_inputs(node_bundles);
    std::vector<PublicSamplePortMember> members;
    members.reserve(configs.size());
    for (auto const& config : configs) {
        members.push_back(PublicSamplePortMember{
            .channel_type = config.channel_layout.channel_type,
        });
    }
    auto families = collect_sample_port_families(configs, members, true);
    for (auto& family : families.families) {
        for (auto& channel : family.channels) {
            for (auto const port_ordinal : channel.port_ordinals) {
            for (auto const& info : sample_input_source_infos(port_ordinal)) {
                if (std::find(family.source_infos.begin(), family.source_infos.end(), info)
                    == family.source_infos.end()) {
                    family.source_infos.push_back(info);
                }
            }}
        }
    }
    return families;
}

GraphBuilderPublicSamplePortFamilies GraphBuilderPublicPorts::sample_output_families(
    GraphBuilderNodeBundles const& node_bundles) const
{
    auto families = collect_sample_port_families(
        sample_outputs(node_bundles), _sample_output_members, false);
    for (auto& family : families.families) {
        for (auto& channel : family.channels) {
            for (auto const port_ordinal : channel.port_ordinals) {
            for (auto const& info : _sample_output_source_infos[port_ordinal]) {
                if (std::find(channel.source_infos.begin(), channel.source_infos.end(), info)
                    == channel.source_infos.end()) {
                    channel.source_infos.push_back(info);
                }
                if (std::find(family.source_infos.begin(), family.source_infos.end(), info) == family.source_infos.end()) {
                    family.source_infos.push_back(info);
                }
            }}
        }
    }
    return families;
}

std::vector<GraphBuilderPublicEventInput> GraphBuilderPublicPorts::collected_event_inputs(
    GraphBuilderNodeBundles const& node_bundles) const
{
    auto const configs = event_inputs(node_bundles);
    std::vector<GraphBuilderPublicEventInput> result;
    result.reserve(configs.size());
    for (size_t port_ordinal = 0; port_ordinal < configs.size(); ++port_ordinal) {
        result.push_back(GraphBuilderPublicEventInput{
            .port_ordinal = port_ordinal,
            .config = configs[port_ordinal],
        });
    }
    return result;
}

std::vector<GraphBuilderPublicEventOutput> GraphBuilderPublicPorts::collected_event_outputs(
    GraphBuilderNodeBundles const& node_bundles) const
{
    auto const configs = event_outputs(node_bundles);
    std::vector<GraphBuilderPublicEventOutput> result;
    result.reserve(configs.size());
    for (size_t port_ordinal = 0; port_ordinal < configs.size(); ++port_ordinal) {
        result.push_back(GraphBuilderPublicEventOutput{
            .port_ordinal = port_ordinal,
            .config = configs[port_ordinal],
            .source_infos = port_ordinal < _event_output_source_infos.size()
                ? _event_output_source_infos[port_ordinal]
                : std::vector<SourceInfo>{},
        });
    }
    return result;
}

void GraphBuilderPublicPorts::annotate_sample_output_source_info(size_t port_ordinal, SourceInfo info)
{
    if (port_ordinal >= _last_sample_output_port_ordinals.size()) return;
    auto& infos = _sample_output_source_infos[_last_sample_output_port_ordinals[port_ordinal]];
    if (std::find(infos.begin(), infos.end(), info) == infos.end()) infos.push_back(std::move(info));
}

void GraphBuilderPublicPorts::annotate_event_output_source_info(size_t port_ordinal, SourceInfo info)
{
    if (port_ordinal >= _event_output_source_infos.size()) return;
    auto& infos = _event_output_source_infos[port_ordinal];
    if (std::find(infos.begin(), infos.end(), info) == infos.end()) infos.push_back(std::move(info));
}
}
