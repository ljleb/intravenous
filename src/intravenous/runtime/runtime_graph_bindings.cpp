#include <intravenous/runtime/runtime_graph_bindings.h>

#include <intravenous/runtime/graph_input_lanes_events.h>
#include <intravenous/runtime/timeline_execution_events.h>

#include <algorithm>

namespace iv {
namespace {

void read_timeline_sample_block(
    LaneId timeline_lane,
    size_t source_channel,
    size_t source_channel_offset,
    size_t,
    size_t block_size,
    ChannelLayout target_layout,
    std::span<Sample> target)
{
    TimelineExecutionRealtimeSampleBlockBuilder builder;
    IV_INVOKE_SINGLETON_EVENT(
        iv_runtime_timeline_execution_realtime_sample_block_requested_event,
        timeline_lane,
        builder);
    auto const view = builder.build().view();
    auto const channels = channel_count(target_layout);
    SampleBlockView<Sample> target_view(target, target_layout, block_size);
    if (target_layout.sample_layout == SampleStreamLayout::planar) {
        for (size_t channel = 0; channel < channels; ++channel) {
            auto target_channel = target_view.channel_span(channel);
            for (size_t frame = 0; frame < block_size; ++frame) {
                target_channel[frame] =
                    frame < view.frames() &&
                        source_channel + source_channel_offset + channel <
                            view.channels()
                    ? view.get(
                          frame,
                          source_channel + source_channel_offset + channel)
                    : Sample{0.0f};
            }
        }
        return;
    }
    for (size_t frame = 0; frame < block_size; ++frame) {
        auto* target_frame = target_view.interleaved_frame_ptr(frame);
        for (size_t channel = 0; channel < channels; ++channel) {
            target_frame[channel] =
                frame < view.frames() &&
                    source_channel + source_channel_offset + channel < view.channels()
                    ? view.get(frame, source_channel + source_channel_offset + channel)
                    : Sample{0.0f};
        }
    }
}

RuntimeEventBlockView read_timeline_event_block(
    LaneId timeline_lane,
    size_t,
    size_t)
{
    TimelineExecutionRealtimeEventBlockBuilder builder;
    IV_INVOKE_SINGLETON_EVENT(
        iv_runtime_timeline_execution_realtime_event_block_requested_event,
        timeline_lane,
        builder);
    auto const events = builder.build();
    return {.data = events.data(), .size = events.size()};
}

void publish_sample_block(
    LaneId target_lane,
    std::span<Sample const> samples,
    ChannelLayout layout,
    size_t frame_count)
{
    IV_INVOKE_SINGLETON_EVENT(
        iv_runtime_graph_input_lanes_sample_block_published_event,
        target_lane,
        BorrowedSampleBlock{
            .samples = samples,
            .channel_layout = layout,
            .frame_count = frame_count,
        });
}

void publish_event_block(
    LaneId target_lane,
    std::span<TimedEvent const> events)
{
    IV_INVOKE_SINGLETON_EVENT(
        iv_runtime_graph_input_lanes_event_block_published_event,
        target_lane,
        events);
}

} // namespace

RuntimeBindingResources GraphRuntimeBindings::resources()
{
    return RuntimeBindingResources{
        .abi_version = RUNTIME_BINDING_RESOURCE_ABI_VERSION,
        .struct_size = sizeof(RuntimeBindingResources),
        .owner = this,
        .resolve_sample_input = +[](
            void* owner,
            char const* key,
            size_t size) -> RuntimeSampleInputBinding const* {
            return static_cast<GraphRuntimeBindings*>(owner)
                ->sample_input(std::string(key, size)).get();
        },
        .resolve_event_input = +[](
            void* owner,
            char const* key,
            size_t size) -> RuntimeEventInputBinding const* {
            return static_cast<GraphRuntimeBindings*>(owner)
                ->event_input(std::string(key, size)).get();
        },
        .resolve_output = +[](
            void* owner,
            char const* key,
            size_t size) -> RuntimeOutputBinding const* {
            return static_cast<GraphRuntimeBindings*>(owner)
                ->output(std::string(key, size)).get();
        },
    };
}

std::shared_ptr<GraphRuntimeBindings> make_graph_runtime_bindings()
{
    return std::make_shared<GraphRuntimeBindings>(GraphRuntimeBindings::Callbacks{
        .read_timeline_sample_block = &read_timeline_sample_block,
        .read_timeline_event_block = &read_timeline_event_block,
        .publish_sample_block = &publish_sample_block,
        .publish_event_block = &publish_event_block,
    });
}

} // namespace iv
