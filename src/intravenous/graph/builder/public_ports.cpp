#include <intravenous/graph/builder/public_ports.h>

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
    std::string_view name,
    Sample default_value,
    std::optional<Sample> min,
    std::optional<Sample> max)
{
    _sample_inputs.emplace_back(InputConfig{
        .name = std::string(name),
        .default_value = default_value,
        .min = min.value_or(-std::numeric_limits<Sample::storage>::infinity()),
        .max = max.value_or(std::numeric_limits<Sample::storage>::infinity()),
    });
    _sample_input_source_infos.emplace_back();
    return SamplePortRef(builder, GRAPH_ID, _sample_inputs.size() - 1);
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

EventPortRef GraphBuilderPublicPorts::add_event_input(GraphBuilder& builder, std::string_view name, EventTypeId type)
{
    if (!name.empty()) {
        _event_inputs.emplace_back(EventInputConfig{ .name = std::string(name), .type = type });
    } else {
        _event_inputs.emplace_back(EventInputConfig{ .type = type });
    }
    _event_input_source_infos.emplace_back();
    return EventPortRef(builder, GRAPH_ID, _event_inputs.size() - 1);
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
    GraphBuilder const& builder,
    GraphBuilderTopology& topology,
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
            ? _sample_outputs.size()
            : static_cast<size_t>(existing - _sample_output_members.begin());
        topology.add_sample_edge(GraphEdge{
            PortId{ ref.node_index, ref.output_port },
            PortId{ GRAPH_ID, output_ordinal },
        });
        if (existing == _sample_output_members.end()) {
            _sample_outputs.push_back(config);
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
    GraphBuilderIdentity const& identity,
    std::span<EventOutputRefConfig const> refs
)
{
    _event_outputs.clear();
    _event_outputs.reserve(refs.size());
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

        auto const source_type = (ref.node_index == GRAPH_ID)
            ? _event_inputs[ref.output_port].type
            : topology.ports(ref.node_index).event_outputs()[ref.output_port].type;
        topology.add_event_edge(GraphEventEdge{
            PortId{ ref.node_index, ref.output_port },
            PortId{ GRAPH_ID, i },
            EventConversionRegistry::instance().plan(source_type, source_type)
        });
        _event_outputs.emplace_back(config);
        _event_outputs.back().type = source_type;
    }
}

std::span<InputConfig const> GraphBuilderPublicPorts::sample_inputs() const
{
    return _sample_inputs;
}

std::span<EventInputConfig const> GraphBuilderPublicPorts::event_inputs() const
{
    return _event_inputs;
}

std::span<OutputConfig const> GraphBuilderPublicPorts::sample_outputs() const
{
    return _sample_outputs;
}

std::span<EventOutputConfig const> GraphBuilderPublicPorts::event_outputs() const
{
    return _event_outputs;
}

GraphBuilderPublicSamplePortFamilies GraphBuilderPublicPorts::sample_input_families() const
{
    std::vector<PublicSamplePortMember> members;
    members.reserve(_sample_inputs.size());
    for (auto const& config : _sample_inputs) {
        members.push_back(PublicSamplePortMember{
            .channel_type = config.channel_layout.channel_type,
        });
    }
    auto families = collect_sample_port_families(
        std::span<InputConfig const>(_sample_inputs), members, true);
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

GraphBuilderPublicSamplePortFamilies GraphBuilderPublicPorts::sample_output_families() const
{
    auto families = collect_sample_port_families(
        std::span<OutputConfig const>(_sample_outputs), _sample_output_members, false);
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

std::vector<GraphBuilderPublicEventInput> GraphBuilderPublicPorts::collected_event_inputs() const
{
    std::vector<GraphBuilderPublicEventInput> result;
    result.reserve(_event_inputs.size());
    for (size_t port_ordinal = 0; port_ordinal < _event_inputs.size(); ++port_ordinal) {
        result.push_back(GraphBuilderPublicEventInput{
            .port_ordinal = port_ordinal,
            .config = _event_inputs[port_ordinal],
        });
    }
    return result;
}

std::vector<GraphBuilderPublicEventOutput> GraphBuilderPublicPorts::collected_event_outputs() const
{
    std::vector<GraphBuilderPublicEventOutput> result;
    result.reserve(_event_outputs.size());
    for (size_t port_ordinal = 0; port_ordinal < _event_outputs.size(); ++port_ordinal) {
        result.push_back(GraphBuilderPublicEventOutput{
            .port_ordinal = port_ordinal,
            .config = _event_outputs[port_ordinal],
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
