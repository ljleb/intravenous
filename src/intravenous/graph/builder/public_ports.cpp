#include <intravenous/graph/builder/public_ports.h>

#include <intravenous/graph/builder/topology.h>

namespace iv {
SamplePortRef GraphBuilderPublicPorts::add_sample_input(GraphBuilder& builder, std::string_view name, Sample default_value)
{
    if (!name.empty()) {
        _sample_inputs.emplace_back(InputConfig{ .name = std::string(name), .default_value = default_value });
    } else {
        _sample_inputs.emplace_back(InputConfig{});
    }
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
    if (_sample_outputs_defined) {
        details::error("outputs(...) was already called on builder " + identity.value);
    }

    _sample_outputs.clear();
    _sample_outputs.reserve(refs.size());
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
            PortId{ GRAPH_ID, i },
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
}
