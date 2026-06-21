#pragma once

#include <intravenous/node/lifecycle.h>
#include <intravenous/runtime/graph_input_lanes_events.h>
#include <intravenous/runtime/timeline_execution_events.h>

#include <array>
#include <atomic>
#include <cassert>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace iv {
    struct ValueSource {
        Sample const* _value = nullptr;
        std::atomic<Sample::storage> const* _atomic_value = nullptr;

        explicit ValueSource(Sample const* value) :
            _value(value)
        {
            IV_ASSERT(_value, "ValueSource requires a non-null value pointer");
        }

        explicit ValueSource(std::atomic<Sample::storage> const* value) :
            _atomic_value(value)
        {
            IV_ASSERT(_atomic_value, "ValueSource requires a non-null atomic value pointer");
        }

        constexpr auto outputs() const
        {
            return std::array { OutputConfig { .name = "value" } };
        }

        void tick(auto const& ctx) const
        {
            if (_atomic_value != nullptr) {
                ctx.outputs[0].push(Sample{_atomic_value->load(std::memory_order_relaxed)});
            } else {
                ctx.outputs[0].push(*_value);
            }
        }
    };

    class BufferSource {
        Sample* _source;
        size_t _size;
        size_t _time_offset;

    public:
        explicit BufferSource(Sample* source, size_t size, size_t time_offset = 0) :
            _source(source),
            _size(size),
            _time_offset(time_offset)
        {
            IV_ASSERT(_size >= 1, "BufferSource requires at least one sample");
        }

        auto outputs() const
        {
            return std::array<OutputConfig, 1>{};
        }

        void tick(TickSampleContext<BufferSource> const& ctx) const
        {
            auto& out = ctx.outputs[0];
            if (ctx.index < _time_offset) {
                out.push(_source[0]);
            } else if (ctx.index >= _time_offset + _size) {
                out.push(_source[_size - 1]);
            } else {
                out.push(_source[ctx.index - _time_offset]);
            }
        }
    };

    class LaneInputValue {
        LaneId _lane {};
        size_t _channel_index = 0;
        std::string _identity;

    public:
        static std::string nominal_identity(LaneId lane, size_t channel_index = 0)
        {
            return "timeline-input-lane:" + std::to_string(lane.value)
                + ":channel:" + std::to_string(channel_index);
        }

        explicit LaneInputValue(
            LaneId lane,
            size_t channel_index = 0
        ) :
            _lane(lane),
            _channel_index(channel_index),
            _identity(nominal_identity(lane, channel_index))
        {}

        constexpr auto outputs() const
        {
            return std::array { OutputConfig { .name = "value" } };
        }

        std::string identity() const
        {
            return _identity;
        }

        void tick_block(TickBlockContext<LaneInputValue> const& ctx) const
        {
            TimelineExecutionRealtimeSampleBlockBuilder builder;
            IV_INVOKE_SINGLETON_EVENT(
                iv_runtime_timeline_execution_realtime_sample_block_requested_event,
                _lane,
                builder);
            auto const block = builder.build();
            auto const view = block.view();
            for (size_t i = 0; i < ctx.block_size; ++i) {
                ctx.outputs[0].push(
                    i < view.frames() && _channel_index < view.channels()
                        ? view.get(i, _channel_index)
                        : Sample{0.0f});
            }
        }
    };

    class GraphSampleOutputSink {
        LaneId _lane {};
        ChannelTypeId _channel_type = ChannelTypeId::mono;
        std::string _identity;

    public:
        static std::string nominal_identity(LaneId lane)
        {
            return "graph-output-sample-sink:" + std::to_string(lane.value);
        }

        explicit GraphSampleOutputSink(LaneId lane, ChannelTypeId channel_type = ChannelTypeId::mono) :
            _lane(lane),
            _channel_type(channel_type),
            _identity(nominal_identity(lane))
        {}

        auto inputs() const
        {
            return std::vector<InputConfig>(channel_count(_channel_type));
        }

        std::string identity() const
        {
            return _identity;
        }

        void tick_block(TickBlockContext<GraphSampleOutputSink> const& ctx) const
        {
            std::vector<Sample> samples(channel_count(_channel_type) * ctx.block_size);
            for (size_t channel = 0; channel < channel_count(_channel_type); ++channel) {
                auto const block = ctx.inputs[channel].get_block(ctx.block_size);
                for (size_t i = 0; i < ctx.block_size; ++i) {
                    samples[channel * ctx.block_size + i] =
                        i < block.size() ? block[i] : Sample{0.0f};
                }
            }
            auto published = BorrowedSampleBlock{
                .samples = samples,
                .channel_layout = ChannelLayout{
                    .channel_type = _channel_type,
                    .sample_layout = SampleStreamLayout::planar,
                },
                .frame_count = ctx.block_size,
            };
            IV_INVOKE_SINGLETON_EVENT(
                iv_runtime_graph_input_lanes_sample_block_published_event,
                _lane,
                published);
        }
    };

    class GraphEventOutputSink {
        LaneId _lane {};
        EventTypeId _type {};
        std::string _identity;

    public:
        static std::string nominal_identity(LaneId lane)
        {
            return "graph-output-event-sink:" + std::to_string(lane.value);
        }

        explicit GraphEventOutputSink(LaneId lane, EventTypeId type) :
            _lane(lane),
            _type(type),
            _identity(nominal_identity(lane))
        {}

        auto event_inputs() const
        {
            return std::array<EventInputConfig, 1>{{
                { .type = _type },
            }};
        }

        std::string identity() const
        {
            return _identity;
        }

        void tick_block(TickBlockContext<GraphEventOutputSink> const& ctx) const
        {
            auto const events = ctx.event_inputs[0].get_block(ctx.index, ctx.block_size);
            std::vector<TimedEvent> stored(events.begin(), events.end());
            IV_INVOKE_SINGLETON_EVENT(
                iv_runtime_graph_input_lanes_event_block_published_event,
                _lane,
                std::span<TimedEvent const>(stored));
        }
    };
}
