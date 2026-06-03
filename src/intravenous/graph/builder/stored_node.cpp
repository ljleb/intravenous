#include <intravenous/graph/builder/stored_node.h>

namespace iv {
std::vector<InputConfig> const& NodePorts::inputs() const
{
    return sample_inputs;
}

std::vector<OutputConfig> const& NodePorts::outputs() const
{
    return sample_outputs;
}

std::vector<EventInputConfig> const& NodePorts::event_inputs_view() const
{
    return event_inputs;
}

std::vector<EventOutputConfig> const& NodePorts::event_outputs_view() const
{
    return event_outputs;
}

bool NodeMaterialization::is_placeholder() const
{
    return !factory;
}

TypeErasedNode NodeMaterialization::make(size_t detach_id_offset) const
{
    return factory(detach_id_offset);
}

bool LoweredSubgraphBinding::active() const
{
    return count != 0
        || !sample_input_targets.empty()
        || !sample_output_sources.empty()
        || !event_input_targets.empty()
        || !event_output_sources.empty()
        || !kind.empty();
}

std::vector<InputConfig> const& BuilderNode::inputs() const
{
    return ports.inputs();
}

std::vector<OutputConfig> const& BuilderNode::outputs() const
{
    return ports.outputs();
}

std::vector<EventInputConfig> const& BuilderNode::event_inputs() const
{
    return ports.event_inputs_view();
}

std::vector<EventOutputConfig> const& BuilderNode::event_outputs() const
{
    return ports.event_outputs_view();
}
}
