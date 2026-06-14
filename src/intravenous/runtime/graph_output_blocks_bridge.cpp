#include <intravenous/runtime/graph_output_blocks_bridge.h>

#include <intravenous/runtime/graph_output_blocks.h>
#include <intravenous/runtime/graph_output_blocks_events.h>

namespace iv {
namespace {
GraphOutputBlocks *bound_blocks = nullptr;

void handle_sample_published(LaneId lane, std::span<Sample const> block)
{
    if (bound_blocks == nullptr) {
        return;
    }
    bound_blocks->publish_sample_block(lane, block);
}

void handle_event_published(LaneId lane, std::span<TimedEvent const> events)
{
    if (bound_blocks == nullptr) {
        return;
    }
    bound_blocks->publish_event_block(lane, events);
}

void handle_sample_requested(LaneId lane, GraphOutputBlocksSampleBlockBuilder &builder)
{
    if (bound_blocks == nullptr) {
        return;
    }
    builder.succeed(bound_blocks->sample_block(lane));
}

void handle_event_requested(LaneId lane, GraphOutputBlocksEventBlockBuilder &builder)
{
    if (bound_blocks == nullptr) {
        return;
    }
    builder.succeed(bound_blocks->event_block(lane));
}

IV_SUBSCRIBE_SINGLETON_EVENT(
    GraphOutputBlocksSamplePublishedEvent,
    iv_runtime_graph_output_blocks_sample_published_event,
    handle_sample_published);
IV_SUBSCRIBE_SINGLETON_EVENT(
    GraphOutputBlocksEventPublishedEvent,
    iv_runtime_graph_output_blocks_event_published_event,
    handle_event_published);
IV_SUBSCRIBE_SINGLETON_EVENT(
    GraphOutputBlocksSampleRequestedEvent,
    iv_runtime_graph_output_blocks_sample_requested_event,
    handle_sample_requested);
IV_SUBSCRIBE_SINGLETON_EVENT(
    GraphOutputBlocksEventRequestedEvent,
    iv_runtime_graph_output_blocks_event_requested_event,
    handle_event_requested);
} // namespace

void bind_graph_output_blocks_bridge(GraphOutputBlocks &blocks)
{
    bound_blocks = &blocks;
}

void unbind_graph_output_blocks_bridge(GraphOutputBlocks const &blocks)
{
    if (bound_blocks == &blocks) {
        bound_blocks = nullptr;
    }
}
} // namespace iv
