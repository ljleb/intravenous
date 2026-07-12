#include <intravenous/graph/builder/public_ports.h>

#include <intravenous/graph/builder/topology.h>

#include <algorithm>
#include <charconv>
#include <optional>
#include <system_error>
#include <type_traits>

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

template<class Config>
GraphBuilderPublicSamplePortFamilies collect_sample_port_families(
    std::span<Config const> configs,
    bool input)
{
    GraphBuilderPublicSamplePortFamilies result;
    result.families.reserve(configs.size());

    for (size_t port_ordinal = 0; port_ordinal < configs.size(); ++port_ordinal) {
        auto const& config = configs[port_ordinal];
        auto const parsed = parse_channel_port_name(config.name);
        auto const channel_type =
            parsed.has_value() ? parsed->channel_type : ChannelTypeId::mono;
        auto const family_ordinal =
            parsed.has_value() ? parsed->family_ordinal : port_ordinal;
        auto const channel_ordinal =
            parsed.has_value() ? parsed->channel_ordinal : size_t{0};

        auto family_it = std::find_if(
            result.families.begin(),
            result.families.end(),
            [&](GraphBuilderPublicSamplePortFamily const& family) {
                return family.family_ordinal == family_ordinal;
            });
        if (family_it == result.families.end()) {
            GraphBuilderPublicSamplePortFamily family{
                .family_ordinal = family_ordinal,
                .family_name = parsed.has_value()
                    ? "channel_" + std::to_string(family_ordinal)
                    : config.name,
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
                input
                    ? "conflicting public sample input channel families"
                    : "conflicting public sample output channel families");
        }
        if (channel_ordinal >= family_it->channels.size()) {
            details::error(
                input
                    ? "public sample input channel ordinal out of bounds"
                    : "public sample output channel ordinal out of bounds");
        }
        if (family_it->channels[channel_ordinal].port_ordinal.has_value()) {
            details::error(
                input
                    ? "duplicate public sample input channel contributor"
                    : "duplicate public sample output channel contributor");
        }
        family_it->channels[channel_ordinal].port_ordinal = port_ordinal;
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
        .min = min,
        .max = max,
    });
    return SamplePortRef(builder, GRAPH_ID, _sample_inputs.size() - 1);
}

EventPortRef GraphBuilderPublicPorts::add_event_input(GraphBuilder& builder, std::string_view name, EventTypeId type)
{
    if (!name.empty()) {
        _event_inputs.emplace_back(EventInputConfig{ .name = std::string(name), .type = type });
    } else {
        _event_inputs.emplace_back(EventInputConfig{ .type = type });
    }
    return EventPortRef(builder, GRAPH_ID, _event_inputs.size() - 1);
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
    size_t const first_output_ordinal = _sample_outputs.size();
    _sample_outputs.reserve(first_output_ordinal + refs.size());
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

        topology.add_sample_edge(GraphEdge{
            PortId{ ref.node_index, ref.output_port },
            PortId{ GRAPH_ID, first_output_ordinal + i },
        });
        _sample_outputs.push_back(config);
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
            : topology.node(ref.node_index).event_outputs()[ref.output_port].type;
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
    return collect_sample_port_families(std::span<InputConfig const>(_sample_inputs), true);
}

GraphBuilderPublicSamplePortFamilies GraphBuilderPublicPorts::sample_output_families() const
{
    return collect_sample_port_families(std::span<OutputConfig const>(_sample_outputs), false);
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
        });
    }
    return result;
}
}
