#include <intravenous/runtime/runtime_graph_bindings.h>

#include <intravenous/runtime/graph_input_lanes_events.h>
#include <intravenous/runtime/timeline_execution_events.h>
#include <intravenous/node/layout.h>

#include <algorithm>
#include <stdexcept>

namespace iv {
namespace {

template<typename Binding, typename Bindings, typename Initialize>
void materialize_bindings(
    NodeStorage& storage,
    Bindings& bindings,
    Initialize initialize)
{
    if (!storage.layout) return;
    for (auto const& exported : storage.layout->exported_arrays) {
        if (exported.element_type !=
            NodeLayoutBuilder::array_type_token<Binding const*>())
            continue;
        auto const& semantic_key = exported.id;
        auto& binding = bindings[semantic_key];
        if (!binding) {
            binding = std::make_shared<Binding>();
            initialize(*binding);
        }
        auto slots = storage.resolve_exported_array_storage<Binding const*>(
            semantic_key);
        if (slots.empty()) continue;
        if (slots.size() != 1) {
            throw std::logic_error(
                "runtime binding export must resolve to exactly one slot: "
                + semantic_key);
        }
        slots.front() = binding.get();
    }
}

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

void GraphRuntimeBindings::materialize(NodeStorage& storage)
{
    std::scoped_lock lock(mutex_);
    materialize_bindings<RuntimeSampleInputBinding>(
        storage,
        sample_inputs_,
        [&](auto& binding) {
            binding.read_timeline_block =
                callbacks_.read_timeline_sample_block;
        });
    materialize_bindings<RuntimeEventInputBinding>(
        storage,
        event_inputs_,
        [&](auto& binding) {
            binding.read_timeline_block =
                callbacks_.read_timeline_event_block;
        });
    materialize_bindings<RuntimeOutputBinding>(
        storage,
        outputs_,
        [&](auto& binding) {
            binding.publish_sample_block = callbacks_.publish_sample_block;
            binding.publish_event_block = callbacks_.publish_event_block;
        });
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
