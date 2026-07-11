#include <intravenous/runtime/graph_input_lanes_events.h>

#include <stdexcept>
#include <utility>

namespace iv {
namespace {
void default_sample_block_published(LaneId, BorrowedSampleBlock const &)
{
}

void default_event_block_published(LaneId, std::span<TimedEvent const>)
{
}

void default_sample_block_requested(LaneId, GraphInputLanesSampleBlockBuilder &)
{
}

void default_event_block_requested(LaneId, GraphInputLanesEventBlockBuilder &)
{
}
}

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

IV_DEFINE_SINGLETON_EVENT(
    GraphInputLanesSampleBlockPublishedEvent,
    iv_runtime_graph_input_lanes_sample_block_published_event,
    default_sample_block_published);
IV_DEFINE_SINGLETON_EVENT(
    GraphInputLanesEventBlockPublishedEvent,
    iv_runtime_graph_input_lanes_event_block_published_event,
    default_event_block_published);
IV_DEFINE_SINGLETON_EVENT(
    GraphInputLanesSampleBlockRequestedEvent,
    iv_runtime_graph_input_lanes_sample_block_requested_event,
    default_sample_block_requested);
IV_DEFINE_SINGLETON_EVENT(
    GraphInputLanesEventBlockRequestedEvent,
    iv_runtime_graph_input_lanes_event_block_requested_event,
    default_event_block_requested);

} // namespace iv
