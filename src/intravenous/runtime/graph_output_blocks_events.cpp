#include <intravenous/runtime/graph_output_blocks_events.h>

namespace iv {
namespace {
void default_sample_published(LaneId, std::span<Sample const>)
{}

void default_event_published(LaneId, std::span<TimedEvent const>)
{}

void default_sample_requested(LaneId, GraphOutputBlocksSampleBlockBuilder &)
{}

void default_event_requested(LaneId, GraphOutputBlocksEventBlockBuilder &)
{}
}

IV_DEFINE_SINGLETON_EVENT(
    GraphOutputBlocksSamplePublishedEvent,
    iv_runtime_graph_output_blocks_sample_published_event,
    default_sample_published);
IV_DEFINE_SINGLETON_EVENT(
    GraphOutputBlocksEventPublishedEvent,
    iv_runtime_graph_output_blocks_event_published_event,
    default_event_published);
IV_DEFINE_SINGLETON_EVENT(
    GraphOutputBlocksSampleRequestedEvent,
    iv_runtime_graph_output_blocks_sample_requested_event,
    default_sample_requested);
IV_DEFINE_SINGLETON_EVENT(
    GraphOutputBlocksEventRequestedEvent,
    iv_runtime_graph_output_blocks_event_requested_event,
    default_event_requested);
}
