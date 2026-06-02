#pragma once

#include "lane_node/traits.h"

#include <algorithm>
#include <cstddef>
#include <span>
#include <variant>
#include <vector>

namespace iv {
    struct CompiledSampleLaneInput {
        std::vector<std::span<Sample const>> sources {};
        Sample default_value = 0.0f;

        Sample sample_at(size_t index) const
        {
            if (sources.empty()) {
                return default_value;
            }
            Sample value = 0.0f;
            for (auto const source : sources) {
                if (index < source.size()) {
                    value += source[index];
                }
            }
            return value;
        }

        void read(size_t start_index, std::span<Sample> out) const
        {
            if (sources.empty()) {
                std::ranges::fill(out, default_value);
                return;
            }
            std::ranges::fill(out, 0.0f);
            for (auto const source : sources) {
                if (start_index >= source.size()) {
                    continue;
                }
                size_t const available = std::min(out.size(), source.size() - start_index);
                for (size_t i = 0; i < available; ++i) {
                    out[i] += source[start_index + i];
                }
            }
        }
    };

    struct CompiledEventLaneInput {
        std::vector<std::span<TimedEvent const>> sources {};
        std::vector<TimedEvent> merged {};

        std::span<TimedEvent const> events(size_t start_index, size_t count)
        {
            merged.clear();
            size_t const end_index = start_index + count;
            for (auto const source : sources) {
                for (auto const& event : source) {
                    if (event.time >= start_index && event.time < end_index) {
                        merged.push_back(event);
                    }
                }
            }
            std::ranges::sort(merged, {}, &TimedEvent::time);
            return merged;
        }
    };

    struct RealtimeSampleLaneInput {
        using GetBlockFn = std::span<Sample const> (*)(void*, size_t, size_t);

        void* context = nullptr;
        GetBlockFn get_block_fn = nullptr;
        Sample default_value = 0.0f;
        size_t active_start_index = 0;
        size_t active_count = 0;
        bool has_source = false;

        bool connected() const
        {
            return has_source;
        }

        Sample get(size_t index) const
        {
            auto const block = get_block(index, 1);
            return block.empty() ? default_value : block[0];
        }

        std::span<Sample const> get_block(size_t start_index, size_t count) const
        {
            if (!get_block_fn) {
                return {};
            }
            return get_block_fn(context, start_index, count);
        }

        std::span<Sample const> get_block() const
        {
            return get_block(active_start_index, active_count);
        }
    };

    struct RealtimeEventLaneInput {
        using GetEventsFn = std::span<TimedEvent const> (*)(void*, size_t, size_t);

        void* context = nullptr;
        GetEventsFn get_events_fn = nullptr;
        size_t active_start_index = 0;
        size_t active_count = 0;

        std::span<TimedEvent const> get_block(size_t start_index, size_t count) const
        {
            if (!get_events_fn) {
                return {};
            }
            return get_events_fn(context, start_index, count);
        }

        std::span<TimedEvent const> get_block() const
        {
            return get_block(active_start_index, active_count);
        }
    };

    struct CompiledSampleLaneOutput {
        std::vector<Sample>* samples = nullptr;

        void write(size_t start_index, std::span<Sample const> values)
        {
            if (!samples) {
                return;
            }
            if (samples->size() < start_index + values.size()) {
                samples->resize(start_index + values.size());
            }
            std::ranges::copy(values, samples->begin() + static_cast<std::ptrdiff_t>(start_index));
        }
    };

    struct CompiledEventLaneOutput {
        std::vector<TimedEvent>* events = nullptr;

        void push(TimedEvent const& event)
        {
            if (!events) {
                return;
            }
            events->push_back(event);
        }

        void push_block(std::span<TimedEvent const> block)
        {
            if (!events) {
                return;
            }
            events->insert(events->end(), block.begin(), block.end());
        }
    };

    class RealtimeSampleLaneOutput {
        BlockView<Sample> _block {};
        size_t _position = 0;

    public:
        RealtimeSampleLaneOutput() = default;
        explicit RealtimeSampleLaneOutput(BlockView<Sample> block) :
            _block(block)
        {}

        void push(Sample value)
        {
            if (_position >= _block.size()) {
                return;
            }
            _block[_position++] = value;
        }

        void push_block(BlockView<Sample const> samples)
        {
            for (size_t i = 0; i < samples.size(); ++i) {
                push(samples[i]);
            }
        }

        BlockView<Sample> block() const
        {
            return _block;
        }
    };

    class RealtimeEventLaneOutput {
        std::vector<TimedEvent>* _events = nullptr;

    public:
        RealtimeEventLaneOutput() = default;
        explicit RealtimeEventLaneOutput(std::vector<TimedEvent>& events) :
            _events(&events)
        {}

        void push(TimedEvent const& event) const
        {
            if (!_events) {
                return;
            }
            _events->push_back(event);
        }

        void push_block(BlockView<TimedEvent const> events) const
        {
            for (TimedEvent const& event : events) {
                push(event);
            }
        }
    };

    using LaneOutputView = std::variant<
        CompiledSampleLaneOutput,
        CompiledEventLaneOutput,
        RealtimeSampleLaneOutput,
        RealtimeEventLaneOutput
    >;

    template<typename OutputDeclaration>
    struct LaneOutputViewFor {
        using Type = LaneOutputView;
    };

    template<>
    struct LaneOutputViewFor<CompiledSampleLaneOutputConfig> {
        using Type = CompiledSampleLaneOutput;
    };

    template<>
    struct LaneOutputViewFor<CompiledEventLaneOutputConfig> {
        using Type = CompiledEventLaneOutput;
    };

    template<>
    struct LaneOutputViewFor<RealtimeSampleLaneOutputConfig> {
        using Type = RealtimeSampleLaneOutput;
    };

    template<>
    struct LaneOutputViewFor<RealtimeEventLaneOutputConfig> {
        using Type = RealtimeEventLaneOutput;
    };

    template<>
    struct LaneOutputViewFor<LaneOutputConfig> {
        using Type = LaneOutputView;
    };

    struct TimelineOutputRequest {
        size_t start_index = 0;
        size_t count = 0;

        bool operator==(TimelineOutputRequest const&) const = default;
    };

    struct UntypedTimelineGenerateContext {
        TimelineOutputRequest output_request {};
        std::span<CompiledSampleLaneInput> compiled_sample_inputs {};
        std::span<CompiledEventLaneInput> compiled_event_inputs {};
        std::span<RealtimeSampleLaneInput> realtime_sample_inputs {};
        std::span<RealtimeEventLaneInput> realtime_event_inputs {};
        LaneOutputView output { CompiledSampleLaneOutput {} };
    };

    template<typename LaneNode>
    class TimelineGenerateContext {
        UntypedTimelineGenerateContext& _untyped;

        template<typename>
        friend class TimelineGenerateContext;

    public:
        using OutputView = typename LaneOutputViewFor<
            std::remove_cvref_t<LaneOutputDeclaration<LaneNode>>
        >::Type;

        explicit TimelineGenerateContext(UntypedTimelineGenerateContext& untyped) :
            _untyped(untyped)
        {}

        template<typename OtherLaneNode>
        explicit TimelineGenerateContext(TimelineGenerateContext<OtherLaneNode>& other) :
            _untyped(other._untyped)
        {}

        size_t start_index() const
        {
            return _untyped.output_request.start_index;
        }

        size_t count() const
        {
            return _untyped.output_request.count;
        }

        std::span<CompiledSampleLaneInput> compiled_sample_inputs() const
        {
            return _untyped.compiled_sample_inputs;
        }

        std::span<CompiledEventLaneInput> compiled_event_inputs() const
        {
            return _untyped.compiled_event_inputs;
        }

        std::span<RealtimeSampleLaneInput> realtime_sample_inputs() const
        {
            return _untyped.realtime_sample_inputs;
        }

        std::span<RealtimeEventLaneInput> realtime_event_inputs() const
        {
            return _untyped.realtime_event_inputs;
        }

        CompiledSampleLaneInput& compiled_sample_input(size_t index) const
        {
            return _untyped.compiled_sample_inputs[index];
        }

        CompiledEventLaneInput& compiled_event_input(size_t index) const
        {
            return _untyped.compiled_event_inputs[index];
        }

        RealtimeSampleLaneInput& realtime_sample_input(size_t index) const
        {
            return _untyped.realtime_sample_inputs[index];
        }

        RealtimeEventLaneInput& realtime_event_input(size_t index) const
        {
            return _untyped.realtime_event_inputs[index];
        }

        OutputView& out() const
        {
            if constexpr (std::same_as<OutputView, LaneOutputView>) {
                return _untyped.output;
            } else {
                return std::get<OutputView>(_untyped.output);
            }
        }
    };

    namespace lane_node_details {
        template<typename LaneNode>
        concept has_generate = requires(LaneNode& node, TimelineGenerateContext<LaneNode>& ctx)
        {
            node.generate(ctx);
        };
    }

    template<typename LaneNode>
    void do_timeline_generate(LaneNode& node, TimelineGenerateContext<LaneNode>& ctx)
    {
        if constexpr (lane_node_details::has_generate<LaneNode>) {
            node.generate(ctx);
        } else {
            static_assert(lane_node_details::has_generate<LaneNode>, "lane node must define generate()");
        }
    }
}
