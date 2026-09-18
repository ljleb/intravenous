#pragma once

#include <intravenous/node/lifecycle.h>

#include <array>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace iv {
    template<class ChannelType>
    class ChannelPack {
    public:
        static constexpr auto inputs()
        {
            return std::array<InputConfig, ChannelType::channel_count>{};
        }

        static constexpr auto outputs()
        {
            return std::array<OutputConfig, 1>{realtime_sample_output("out", {
                .channel_layout = ChannelLayout{
                    .channel_type = ChannelTypeTraits<ChannelType>::id,
                    .sample_layout = SampleStreamLayout::planar,
                },
            })};
        }

        void tick_block(TickBlockContext<ChannelPack> const& ctx) const
        {
            for_each_channel_port<ChannelType>([&]<auto channel>() {
                ctx.template output<"out">()[channel].write_block(
                    ctx.inputs[port_index(channel)].get_block(ctx.block_size));
            });
        }
    };

    template<class ChannelType>
    class ChannelUnpack {
    public:
        static constexpr auto inputs()
        {
            return std::array<InputConfig, 1>{realtime_sample_input("in", {
                .channel_layout = ChannelLayout{
                    .channel_type = ChannelTypeTraits<ChannelType>::id,
                    .sample_layout = SampleStreamLayout::planar,
                },
            })};
        }

        static constexpr auto outputs()
        {
            auto configs = std::array<OutputConfig, ChannelType::channel_count>{};
            for (size_t channel = 0; channel < ChannelType::channel_count; ++channel) {
                configs[channel].name = ChannelType::channel_names[channel];
            }
            return configs;
        }

        void tick(TickSampleContext<ChannelUnpack> const& ctx) const
        {
            for_each_channel_port<ChannelType>([&]<auto channel>() {
                ctx.outputs[port_index(channel)].push(
                    ctx.inputs[0].get(0, port_index(channel)));
            });
        }
    };

    struct DetachArrayId {
        size_t id;

        constexpr DetachArrayId(size_t id): id(id) {}

        operator std::string() const {
            return "detach:" + std::to_string(id);
        }
    };


    struct EventDetachWriterNode {
        DetachArrayId id;
        size_t loop_extra_latency = 1;
        EventTypeId type = EventTypeId::empty;
        double max_events_per_sample = DEFAULT_MAX_EVENTS_PER_SAMPLE;

        constexpr explicit EventDetachWriterNode(
            DetachArrayId id_,
            size_t loop_extra_latency_ = 1,
            EventTypeId type_ = EventTypeId::empty,
            double max_events_per_sample_ = DEFAULT_MAX_EVENTS_PER_SAMPLE)
            : id(id_)
            , loop_extra_latency(loop_extra_latency_)
            , type(type_)
            , max_events_per_sample(max_events_per_sample_)
        {}

        struct State {
            std::span<TimedEvent> events;
            std::span<size_t> control;
        };

        constexpr auto inputs() const
        {
            return std::array{realtime_event_input({}, type)};
        }

        void declare(DeclarationContext<EventDetachWriterNode> const& ctx) const
        {
            auto& state = ctx.state();
            if (loop_extra_latency
                > std::numeric_limits<size_t>::max() - ctx.max_block_size()) {
                throw std::logic_error(
                    "event detach temporal span is not representable");
            }
            auto const span = loop_extra_latency + ctx.max_block_size();
            auto const capacity = event_count_for_sample_span(
                max_events_per_sample, span);
            if (!capacity) {
                throw std::logic_error(
                    "event detach capacity is not representable");
            }
            ctx.local_array(state.events, std::max<size_t>(1, *capacity));
            ctx.local_array(state.control, 3);
            ctx.export_array(std::string(id) + ":events", state.events);
            ctx.export_array(std::string(id) + ":control", state.control);
        }

        void initialize(InitializationContext<EventDetachWriterNode> const& ctx) const
        {
            std::ranges::fill(ctx.state().control, size_t{0});
        }

        void tick_block(TickBlockContext<EventDetachWriterNode> const& ctx) const
        {
            auto& state = ctx.state();
            auto& tail = state.control[1];
            auto& count = state.control[2];
            auto const capacity = state.events.size();
            auto const input = ctx.event_inputs[0].get_block(ctx.index, ctx.block_size);
            for (auto const& event : input) {
                IV_ASSERT(count < capacity, "event detach buffer capacity exceeded");
                auto delayed = event;
                delayed.time = saturating_sample_index_add(
                    delayed.time, loop_extra_latency);
                state.events[tail] = std::move(delayed);
                tail = (tail + 1) % capacity;
                ++count;
            }
        }
    };

    struct EventDetachReaderNode {
        DetachArrayId id;
        size_t loop_extra_latency = 1;
        EventTypeId type = EventTypeId::empty;
        double max_events_per_sample = DEFAULT_MAX_EVENTS_PER_SAMPLE;

        constexpr explicit EventDetachReaderNode(
            DetachArrayId id_,
            size_t loop_extra_latency_ = 1,
            EventTypeId type_ = EventTypeId::empty,
            double max_events_per_sample_ = DEFAULT_MAX_EVENTS_PER_SAMPLE)
            : id(id_)
            , loop_extra_latency(loop_extra_latency_)
            , type(type_)
            , max_events_per_sample(max_events_per_sample_)
        {}

        struct State {
            std::span<TimedEvent> events;
            std::span<size_t> control;
        };

        constexpr auto outputs() const
        {
            return std::array{realtime_event_output(
                {}, EventOutputProperties{
                    .type = type,
                    .max_events_per_sample = max_events_per_sample,
                })};
        }

        void declare(DeclarationContext<EventDetachReaderNode> const& ctx) const
        {
            auto& state = ctx.state();
            ctx.import_array(std::string(id) + ":events", state.events);
            ctx.import_array(std::string(id) + ":control", state.control);
        }

        void tick_block(TickBlockContext<EventDetachReaderNode> const& ctx) const
        {
            auto& state = ctx.state();
            auto& head = state.control[0];
            auto& count = state.control[2];
            auto const capacity = state.events.size();
            auto const begin = ctx.index;
            auto const end = saturating_sample_index_add(ctx.index, ctx.block_size);

            while (count != 0 && state.events[head].time < begin) {
                head = (head + 1) % capacity;
                --count;
            }
            while (count != 0 && state.events[head].time < end) {
                ctx.event_outputs[0].push(state.events[head]);
                head = (head + 1) % capacity;
                --count;
            }
        }
    };

    struct BroadcastEvent {
        size_t _num_outputs;
        EventTypeId _type;

        constexpr explicit BroadcastEvent(size_t num_outputs, EventTypeId type) :
            _num_outputs(num_outputs),
            _type(type)
        {}

        constexpr auto inputs() const
        {
            return std::array { realtime_event_input({}, _type) };
        }

        constexpr auto outputs() const
        {
            return std::vector<OutputConfig>(
                _num_outputs, realtime_event_output({}, _type));
        }

        void tick_block(TickBlockContext<BroadcastEvent> const& ctx) const
        {
            auto events = ctx.event_inputs[0].get_block(ctx.index, ctx.block_size);
            for (auto& output : ctx.event_outputs) {
                output.push_block(events);
            }
        }
    };

    struct EventConcatenation {
        size_t _num_inputs;
        EventTypeId _type;

        struct State {
            std::span<size_t> cursors;
        };

        constexpr explicit EventConcatenation(size_t num_inputs, EventTypeId type) :
            _num_inputs(num_inputs),
            _type(type)
        {}

        constexpr auto inputs() const
        {
            return std::vector<InputConfig>(_num_inputs, realtime_event_input({}, _type));
        }

        constexpr auto outputs() const
        {
            return std::array { realtime_event_output({}, _type) };
        }

        void declare(DeclarationContext<EventConcatenation> const& ctx) const
        {
            auto const& state = ctx.state();
            ctx.local_array(state.cursors, _num_inputs);
        }

        void tick_block(TickBlockContext<EventConcatenation> const& ctx) const
        {
            auto& state = ctx.state();
            auto const num_inputs = ctx.event_inputs.size();
            std::ranges::fill(state.cursors, 0);

            while (true) {
                size_t selected_input = num_inputs;
                TimedEvent selected_event {};
                for (size_t input_i = 0; input_i < num_inputs; ++input_i) {
                    auto const block = ctx.event_inputs[input_i].get_block(ctx.index, ctx.block_size);
                    if (state.cursors[input_i] >= block.size()) {
                        continue;
                    }
                    TimedEvent const event = block[state.cursors[input_i]];
                    if (selected_input == num_inputs || event.time < selected_event.time) {
                        selected_input = input_i;
                        selected_event = event;
                    }
                }

                if (selected_input == num_inputs) {
                    break;
                }

                ctx.event_outputs[0].push(selected_event);
                ++state.cursors[selected_input];
            }
        }
    };

struct DetachWriterNode {
    DetachArrayId id;
    size_t loop_extra_latency = 1;

    constexpr explicit DetachWriterNode(
        DetachArrayId id_, size_t loop_extra_latency_ = 1)
        : id(id_)
        , loop_extra_latency(loop_extra_latency_)
    {}

        struct State {
            std::span<Sample> samples;
        };

        static constexpr auto inputs()
        {
            return std::array<InputConfig, 1>{};
        }

        void declare(DeclarationContext<DetachWriterNode> const& ctx) const
        {
            auto const& state = ctx.state();
            size_t const min_size = loop_extra_latency + ctx.max_block_size();
            ctx.local_array(state.samples, next_power_of_2(min_size));
            ctx.export_array(id, state.samples);
        }

        void initialize(InitializationContext<DetachWriterNode> const& ctx) const
        {
            auto& state = ctx.state();
            std::ranges::fill(state.samples, Sample{});
        }

        void tick_block(TickBlockContext<DetachWriterNode> const& ctx) const
        {
            auto& state = ctx.state();
            auto const& src = ctx.inputs[0].get_block(ctx.block_size);
            auto const& dst = make_block_view(state.samples, ctx.index & (state.samples.size() - 1), ctx.block_size);
            src.copy_to(dst);
        }
    };

struct DetachReaderNode {
    DetachArrayId id;
    size_t loop_extra_latency = 1;

    constexpr explicit DetachReaderNode(
        DetachArrayId id_, size_t loop_extra_latency_ = 1)
        : id(id_)
        , loop_extra_latency(loop_extra_latency_)
    {}

        struct State {
            std::span<Sample> samples;
        };

        static constexpr auto outputs()
        {
            return std::array<OutputConfig, 1>{};
        }

        void declare(DeclarationContext<DetachReaderNode> const& ctx) const
        {
            auto const& state = ctx.state();
            ctx.import_array(id, state.samples);
        }

        void tick_block(TickBlockContext<DetachReaderNode> const& ctx) const
        {
            auto& state = ctx.state();
            auto const& samples = state.samples;
            auto const n = samples.size();

            auto const total = ctx.block_size;
            auto const start = (ctx.index + n - loop_extra_latency) & (n - 1);
            BlockView<Sample const> const samples_block {
                std::span<Sample const>(samples.data() + start, std::min(total, n - start)),
                std::span<Sample const>(samples.data(), total - std::min(total, n - start)),
            };
            ctx.outputs[0].push_block(samples_block);
        }
    };

    struct DummySink {
        static constexpr auto inputs()
        {
            return std::array<InputConfig, 1>{};
        }

        void tick(TickSampleContext<DummySink> const&) const
        {}
    };

    struct DummyEventSink {
        static constexpr auto inputs()
        {
            return std::array { realtime_event_input({}, EventTypeId::empty) };
        }

        void tick_block(TickBlockContext<DummyEventSink> const&) const
        {}
    };
}
