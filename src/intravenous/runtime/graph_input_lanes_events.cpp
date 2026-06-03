#include <intravenous/runtime/graph_input_lanes_events.h>

#include <stdexcept>
#include <utility>

namespace iv {
void GraphInputLanesAckBuilder::succeed()
{
    handled = true;
    error_message.reset();
}

void GraphInputLanesAckBuilder::fail(std::string message)
{
    handled = false;
    error_message = std::move(message);
}

void GraphInputLanesAckBuilder::build() const
{
    if (error_message.has_value()) {
        throw std::runtime_error(*error_message);
    }
    if (!handled) {
        throw std::runtime_error("runtime graph input lanes event was not handled");
    }
}

} // namespace iv
