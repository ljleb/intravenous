struct LatencyCompensationProbe {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t last_index = 0;
        std::uint64_t last_block_size = 0;
        std::uint64_t mismatches = 0;
        float fast_first = 0.0f;
        float slow_first = 0.0f;
        float fast_last = 0.0f;
        float slow_last = 0.0f;
        float max_abs_difference = 0.0f;
        std::uint32_t marker = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{
            iv::input<iv::Sample>({"fast", "Fast"}, iv::InputAccess::sequential),
            iv::input<iv::Sample>({"slow", "Slow"}, iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(iv::TickBlockContext<LatencyCompensationProbe> const& ctx) const
    {
        auto& state = ctx.state();
        auto const fast = ctx.template input<0>().block();
        auto const slow = ctx.template input<1>().block();
        ++state.calls;
        state.last_index = ctx.index.whole().bits();
        state.last_block_size = ctx.block_size;
        state.marker = 0x71ac0deu;
        if (ctx.block_size != 0) {
            state.fast_first = fast[0];
            state.slow_first = slow[0];
            state.fast_last = fast[ctx.block_size - 1];
            state.slow_last = slow[ctx.block_size - 1];
        }
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            auto difference = static_cast<float>(fast[i] - slow[i]);
            if (difference < 0.0f) difference = -difference;
            if (difference != 0.0f) ++state.mismatches;
            if (difference > state.max_abs_difference) {
                state.max_abs_difference = difference;
            }
        }
    }
};

struct InterleavedLatencyCompensationProbe {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t last_index = 0;
        std::uint64_t last_block_size = 0;
        std::uint64_t mismatches = 0;
        float fast_first = 0.0f;
        float slow_first = 0.0f;
        float fast_last = 0.0f;
        float slow_last = 0.0f;
        float max_abs_difference = 0.0f;
        std::uint32_t marker = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{
            iv::input<iv::Sample>({"fast", "Fast"}, iv::InputAccess::sequential,
                {.channel_layout = {
                    .channel_type = iv::ChannelTypeId::mono,
                    .sample_layout = iv::SampleStreamLayout::interleaved,
                }}),
            iv::input<iv::Sample>({"slow", "Slow"}, iv::InputAccess::sequential,
                {.channel_layout = {
                    .channel_type = iv::ChannelTypeId::mono,
                    .sample_layout = iv::SampleStreamLayout::interleaved,
                }}),
        };
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(
        iv::TickBlockContext<InterleavedLatencyCompensationProbe> const& ctx) const
    {
        auto& state = ctx.state();
        auto const fast = ctx.template input<0>().block();
        auto const slow = ctx.template input<1>().block();
        ++state.calls;
        state.last_index = ctx.index.whole().bits();
        state.last_block_size = ctx.block_size;
        state.marker = 0x1a7e2e0u;
        if (ctx.block_size != 0) {
            state.fast_first = fast[0];
            state.slow_first = slow[0];
            state.fast_last = fast[ctx.block_size - 1];
            state.slow_last = slow[ctx.block_size - 1];
        }
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            auto difference = static_cast<float>(fast[i] - slow[i]);
            if (difference < 0.0f) difference = -difference;
            if (difference != 0.0f) ++state.mismatches;
            if (difference > state.max_abs_difference) {
                state.max_abs_difference = difference;
            }
        }
    }
};

struct DisconnectedSampleInputProbe {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t last_index = 0;
        std::uint64_t last_block_size = 0;
        float current_left = 0.0f;
        float current_right = 0.0f;
        float history_left = 0.0f;
        float history_right = 0.0f;
        float block_first = 0.0f;
        float block_last = 0.0f;
        float block_sum = 0.0f;
        std::uint32_t marker = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>({"in", "In"}, iv::InputAccess::sequential,
            {.channel_layout = {
                 .channel_type = iv::ChannelTypeId::stereo,
                 .sample_layout = iv::SampleStreamLayout::interleaved,
             },
             .default_value = iv::Sample{0.375f}})};
    }

    void constrain_ports(
        iv::ConstrainPortsContext<DisconnectedSampleInputProbe>& ctx) const
    {
        ctx.input<"in">().history() = 5;
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(
        iv::TickBlockContext<DisconnectedSampleInputProbe> const& ctx) const
    {
        auto& state = ctx.state();
        auto input = ctx.template input<0>();
        auto const block = input.block(std::size_t{0});
        ++state.calls;
        state.last_index = ctx.index.whole().bits();
        state.last_block_size = ctx.block_size;
        state.current_left = input.get(0, 0);
        state.current_right = input.get(0, 1);
        state.history_left = input.get(5, 0);
        state.history_right = input.get(5, 1);
        state.block_first = block.empty() ? 0.0f : static_cast<float>(block[0]);
        state.block_last = block.empty()
            ? 0.0f
            : static_cast<float>(block[block.size() - 1]);
        state.block_sum = 0.0f;
        for (auto const sample : block) state.block_sum += sample;
        state.marker = 0xd15c0a11u;
    }
};

struct ManualControlSliceProbe {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t value_count = 0;
        std::array<float, 12> values{};
        std::array<float, 6> history_before_slice{};
    };

    static constexpr auto inputs()
    {
        return std::array{
            iv::input<iv::Sample>({"value", "Value"}, iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs() { return std::tuple{}; }

    std::size_t max_block_size() const { return 2; }

    void constrain_ports(
        iv::ConstrainPortsContext<ManualControlSliceProbe>& ctx) const
    {
        ctx.input<"value">().history() = 4;
    }

    void tick_block(
        iv::TickBlockContext<ManualControlSliceProbe> const& ctx) const
    {
        auto& state = ctx.state();
        auto const input = ctx.input<"value">();
        auto const values = input.block();
        if (state.calls < state.history_before_slice.size()) {
            state.history_before_slice[state.calls] = input.get(1);
        }
        ++state.calls;
        for (auto const value : values) {
            if (state.value_count < state.values.size()) {
                state.values[state.value_count] = value;
            }
            ++state.value_count;
        }
    }
};

struct TriggerEventSource {
    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::array{
            iv::event_output({"trigger", "Trigger"}, iv::EventTypeId::trigger, iv::OutputProduction::sequential),
        };
    }

    void tick_block(iv::TickBlockContext<TriggerEventSource> const& ctx) const
    {
        if (ctx.block_size == 0) return;
        ctx.template output<0>().push(
            iv::TriggerEvent{}, 3);
        ctx.template output<0>().push(
            iv::TriggerEvent{}, ctx.block_size - 1);
    }
};

struct MidiEventSource {
    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::array{
            iv::event_output({"midi", "MIDI"}, iv::EventTypeId::midi, iv::OutputProduction::sequential),
        };
    }

    void tick_block(iv::TickBlockContext<MidiEventSource> const& ctx) const
    {
        if (ctx.block_size < 18) return;

        iv::MidiEvent note_on_a{};
        note_on_a.bytes = {0x90, 60, 100};
        note_on_a.size = 3;
        iv::MidiEvent note_off{};
        note_off.bytes = {0x80, 60, 0};
        note_off.size = 3;
        iv::MidiEvent note_on_b{};
        note_on_b.bytes = {0x90, 64, 96};
        note_on_b.size = 3;

        ctx.template output<0>().push(
            note_on_a, 5);
        ctx.template output<0>().push(
            note_off, 9);
        ctx.template output<0>().push(
            note_on_b, 17);
    }
};

struct LimitedTriggerEventSource {
    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::array{
            iv::event_output({"trigger", "Trigger"}, iv::EventTypeId::trigger, iv::OutputProduction::sequential),
        };
    }

    std::size_t max_block_size() const { return 16; }

    void tick_block(iv::TickBlockContext<LimitedTriggerEventSource> const& ctx) const
    {
        if (ctx.block_size == 0) return;
        ctx.template output<0>().push(
            iv::TriggerEvent{}, 3);
        ctx.template output<0>().push(
            iv::TriggerEvent{}, ctx.block_size - 1);
    }
};

struct FanInBurstEventSource {
    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::event_output({"out", "Out"}, iv::EventTypeId::trigger, iv::OutputProduction::sequential, {.max_events_per_index = 0.125})};
    }

    void tick_block(iv::TickBlockContext<FanInBurstEventSource> const& ctx) const
    {
        if (ctx.block_size == 0) return;
        for (std::size_t i = 0; i < 8; ++i) {
            ctx.template output<0>().push(
                iv::TriggerEvent{}, 3);
        }
    }
};

struct FanInSparseEventSource {
    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::event_output({"out", "Out"}, iv::EventTypeId::trigger, iv::OutputProduction::sequential, {.max_events_per_index = 0.125})};
    }

    void tick_block(iv::TickBlockContext<FanInSparseEventSource> const& ctx) const
    {
        if (ctx.block_size == 0) return;
        for (std::size_t i = 0; i < 4; ++i) {
            ctx.template output<0>().push(
                iv::TriggerEvent{}, 3);
        }
    }
};

struct EventFeedbackA {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t scc_feedback_latency = 0;
        std::array<std::uint64_t, 24> indices{};
        std::array<std::uint64_t, 24> block_sizes{};
        std::array<std::uint64_t, 24> input_counts{};
        std::array<std::uint64_t, 24> first_input_times{};
        std::uint32_t marker = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{
            iv::event_input({"in", "In"}, iv::EventTypeId::trigger, iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{iv::event_output({"out", "Out"}, iv::EventTypeId::trigger, iv::OutputProduction::sequential, {.max_events_per_index = 0.25})};
    }

    void tick_block(iv::TickBlockContext<EventFeedbackA> const& ctx) const
    {
        auto& state = ctx.state();
        auto const index = static_cast<std::size_t>(state.calls);
        auto const events = ctx.template input<0>().events();
        if (index < state.indices.size()) {
            state.indices[index] = ctx.index.whole().bits();
            state.block_sizes[index] = ctx.block_size;
            state.input_counts[index] = events.size();
            state.first_input_times[index] =
                events.empty() ? 0 : events[0].time.bits();
        }
        ++state.calls;
        state.scc_feedback_latency = ctx.scc_feedback_latency;
        state.marker = 0xa11ce001u;
        if (ctx.block_size != 0) {
            auto const offset = std::min<std::size_t>(1, ctx.block_size - 1);
            ctx.template output<0>().push(
                iv::TriggerEvent{}, offset);
        }
    }
};

struct RetainedFanInEventFeedbackA {
    using State = EventFeedbackA::State;

    static constexpr auto inputs()
    {
        return std::array{
            iv::event_input({"in", "In"}, iv::EventTypeId::trigger, iv::InputAccess::sequential),
        };
    }

    void constrain_ports(
        iv::ConstrainPortsContext<RetainedFanInEventFeedbackA>& ctx) const
    {
        ctx.input<"in">().history() = 8;
    }

    static constexpr auto outputs()
    {
        return EventFeedbackA::outputs();
    }

    void tick_block(
        iv::TickBlockContext<RetainedFanInEventFeedbackA> const& ctx) const
    {
        auto& state = ctx.state();
        auto const index = static_cast<std::size_t>(state.calls);
        constexpr std::size_t history = 8;
        auto const events = ctx.template input<0>().events(
            ctx.index.retreated_by({.whole = history}).whole(), ctx.block_size + history);
        if (index < state.indices.size()) {
            state.indices[index] = ctx.index.whole().bits();
            state.block_sizes[index] = ctx.block_size;
            state.input_counts[index] = events.size();
            state.first_input_times[index] =
                events.empty() ? 0 : events[0].time.bits();
        }
        ++state.calls;
        state.scc_feedback_latency = ctx.scc_feedback_latency;
        state.marker = 0xa11ce008u;
        if (ctx.block_size != 0) {
            auto const offset = std::min<std::size_t>(1, ctx.block_size - 1);
            ctx.template output<0>().push(
                iv::TriggerEvent{}, offset);
        }
    }
};

struct LatentEventFeedbackA {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t scc_feedback_latency = 0;
        std::array<std::uint64_t, 24> indices{};
        std::array<std::uint64_t, 24> block_sizes{};
        std::array<std::uint64_t, 24> input_counts{};
        std::array<std::uint64_t, 24> first_input_times{};
        std::uint32_t marker = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{
            iv::event_input({"in", "In"}, iv::EventTypeId::trigger, iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{iv::event_output({"out", "Out"}, iv::EventTypeId::trigger, iv::OutputProduction::sequential, {.max_events_per_index = 0.25})};
    }

    void constrain_ports(
        iv::ConstrainPortsContext<LatentEventFeedbackA>& ctx) const
    {
        ctx.output<"out">().latency() = 16;
    }

    void tick_block(iv::TickBlockContext<LatentEventFeedbackA> const& ctx) const
    {
        auto& state = ctx.state();
        auto const index = static_cast<std::size_t>(state.calls);
        auto const events = ctx.template input<0>().events();
        if (index < state.indices.size()) {
            state.indices[index] = ctx.index.whole().bits();
            state.block_sizes[index] = ctx.block_size;
            state.input_counts[index] = events.size();
            state.first_input_times[index] =
                events.empty() ? 0 : events[0].time.bits();
        }
        ++state.calls;
        state.scc_feedback_latency = ctx.scc_feedback_latency;
        state.marker = 0xa11ce016u;

        // Authored in the latency window; read one position after the slice.
        ctx.template output<0>().push(iv::TimedEvent{
            .time = ctx.index.whole()
                .advanced_by(ctx.block_size + 1)
                .retreated_by(16),
            .value = iv::TriggerEvent{},
        });
    }
};

struct PersistentLatentEventFeedbackA {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t scc_feedback_latency = 0;
        std::array<std::uint64_t, 24> indices{};
        std::array<std::uint64_t, 24> block_sizes{};
        std::array<std::uint64_t, 24> input_counts{};
        std::array<std::uint64_t, 24> first_input_times{};
        std::uint32_t marker = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{
            iv::event_input({"in", "In"}, iv::EventTypeId::trigger, iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{iv::event_output({"out", "Out"}, iv::EventTypeId::trigger, iv::OutputProduction::sequential, {.max_events_per_index = 0.25})};
    }

    void constrain_ports(
        iv::ConstrainPortsContext<PersistentLatentEventFeedbackA>& ctx) const
    {
        ctx.output<"out">().latency() = 320;
    }

    void tick_block(iv::TickBlockContext<PersistentLatentEventFeedbackA> const& ctx) const
    {
        auto& state = ctx.state();
        auto const index = static_cast<std::size_t>(state.calls);
        auto const events = ctx.template input<0>().events();
        if (index < state.indices.size()) {
            state.indices[index] = ctx.index.whole().bits();
            state.block_sizes[index] = ctx.block_size;
            state.input_counts[index] = events.size();
            state.first_input_times[index] =
                events.empty() ? 0 : events[0].time.bits();
        }
        ++state.calls;
        state.scc_feedback_latency = ctx.scc_feedback_latency;
        state.marker = 0xa11ce320u;

        // The large latency forces a persistent ring. Authored in the latency
        // window; read one position after the slice.
        ctx.template output<0>().push(iv::TimedEvent{
            .time = ctx.index.whole()
                .advanced_by(ctx.block_size + 1)
                .retreated_by(320),
            .value = iv::TriggerEvent{},
        });
    }
};

struct PersistentLatentBoundaryEventFeedbackA {
    static constexpr auto inputs()
    {
        return std::array{
            iv::event_input({"in", "In"}, iv::EventTypeId::boundary, iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{iv::event_output({"out", "Out"}, iv::EventTypeId::boundary, iv::OutputProduction::sequential, {.max_events_per_index = 0.25})};
    }

    void constrain_ports(
        iv::ConstrainPortsContext<PersistentLatentBoundaryEventFeedbackA>& ctx) const
    {
        ctx.output<"out">().latency() = 320;
    }

    void tick_block(
        iv::TickBlockContext<PersistentLatentBoundaryEventFeedbackA> const& ctx) const
    {
        (void)ctx.template input<0>().events();
        // Authored in the latency window; read one position after the slice.
        ctx.template output<0>().push(iv::TimedEvent{
            .time = ctx.index.whole()
                .advanced_by(ctx.block_size + 1)
                .retreated_by(320),
            .value = iv::BoundaryEvent{.is_begin = true},
        });
    }
};

struct EventFeedbackBurstA {
    static constexpr auto inputs()
    {
        return std::array{
            iv::event_input({"in", "In"}, iv::EventTypeId::trigger, iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{iv::event_output({"out", "Out"}, iv::EventTypeId::trigger, iv::OutputProduction::sequential, {.max_events_per_index = 16.0})};
    }

    void tick_block(iv::TickBlockContext<EventFeedbackBurstA> const& ctx) const
    {
        if (ctx.block_size != 1) return;
        // Publish the full declared one-sample rate at one timestamp to stress
        // delayed feedback carry across tiny root calls.
        for (std::size_t i = 0; i < 16; ++i) {
            ctx.template output<0>().push(
                iv::TriggerEvent{}, 0);
        }
    }
};

struct EventFeedbackFanoutA {
    static constexpr auto inputs()
    {
        return std::array{
            iv::event_input({"in_a", "In a"}, iv::EventTypeId::trigger, iv::InputAccess::sequential),
            iv::event_input({"in_b", "In b"}, iv::EventTypeId::trigger, iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{iv::event_output({"out", "Out"}, iv::EventTypeId::trigger, iv::OutputProduction::sequential, {.max_events_per_index = 0.25})};
    }

    void tick_block(iv::TickBlockContext<EventFeedbackFanoutA> const& ctx) const
    {
        // Force both explicit feed-forward dependencies to remain observable to
        // the package/JIT interfaces even though this probe only needs to emit.
        (void)ctx.template input<0>().events();
        (void)ctx.template input<1>().events();
        if (ctx.block_size == 0) return;
        auto const offset = std::min<std::size_t>(1, ctx.block_size - 1);
        ctx.template output<0>().push(
            iv::TriggerEvent{}, offset);
    }
};

struct EventFeedbackB {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t scc_feedback_latency = 0;
        std::array<std::uint64_t, 24> indices{};
        std::array<std::uint64_t, 24> block_sizes{};
        std::array<std::uint64_t, 24> input_counts{};
        std::array<std::uint64_t, 24> first_input_times{};
        std::uint64_t marker = 0;
        std::uint64_t distinct_padding = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{
            iv::event_input({"in", "In"}, iv::EventTypeId::trigger, iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{iv::event_output({"out", "Out"}, iv::EventTypeId::trigger, iv::OutputProduction::sequential, {.max_events_per_index = 0.25})};
    }

    void tick_block(iv::TickBlockContext<EventFeedbackB> const& ctx) const
    {
        auto& state = ctx.state();
        auto const index = static_cast<std::size_t>(state.calls);
        auto const events = ctx.template input<0>().events();
        if (index < state.indices.size()) {
            state.indices[index] = ctx.index.whole().bits();
            state.block_sizes[index] = ctx.block_size;
            state.input_counts[index] = events.size();
            state.first_input_times[index] =
                events.empty() ? 0 : events[0].time.bits();
        }
        ++state.calls;
        state.scc_feedback_latency = ctx.scc_feedback_latency;
        state.marker = 0xb22ce002ull;
        if (ctx.block_size != 0) {
            auto const offset = std::min<std::size_t>(2, ctx.block_size - 1);
            ctx.template output<0>().push(
                iv::TriggerEvent{}, offset);
        }
    }
};
