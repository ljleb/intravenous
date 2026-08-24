#include <intravenous/graph/builder/stored_node.h>

namespace iv {
bool LoweredSubgraphBinding::active() const
{
    return count != 0
        || !sample_input_targets.empty()
        || !sample_output_sources.empty()
        || !event_input_targets.empty()
        || !event_output_sources.empty()
        || !kind.empty();
}

std::vector<InputConfig> const& ConcreteNode::inputs() const
{
    return ports.inputs();
}

std::vector<OutputConfig> const& ConcreteNode::outputs() const
{
    return ports.outputs();
}

std::vector<EventInputConfig> const& ConcreteNode::event_inputs() const
{
    return ports.event_inputs();
}

std::vector<EventOutputConfig> const& ConcreteNode::event_outputs() const
{
    return ports.event_outputs();
}

std::vector<InputConfig> const& SubgraphNode::inputs() const { return ports.inputs(); }
std::vector<OutputConfig> const& SubgraphNode::outputs() const { return ports.outputs(); }
std::vector<EventInputConfig> const& SubgraphNode::event_inputs() const { return ports.event_inputs(); }
std::vector<EventOutputConfig> const& SubgraphNode::event_outputs() const { return ports.event_outputs(); }

}
