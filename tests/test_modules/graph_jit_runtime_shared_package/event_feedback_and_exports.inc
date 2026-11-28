struct RetainedDualEventFeedbackA {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t scc_feedback_latency = 0;
        std::array<std::uint64_t, 24> indices{};
        std::array<std::uint64_t, 24> block_sizes{};
        std::array<std::uint64_t, 24> exact_counts{};
        std::array<std::uint64_t, 24> exact_first_times{};
        std::array<std::uint64_t, 24> exact_last_times{};
        std::array<std::uint64_t, 24> converted_counts{};
        std::array<std::uint64_t, 24> converted_first_times{};
        std::array<std::uint64_t, 24> converted_last_times{};
        std::uint64_t marker = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{
            iv::event_input({"exact", "Exact"}, iv::EventTypeId::trigger, iv::InputAccess::sequential),
            iv::event_input({"converted", "Converted"}, iv::EventTypeId::trigger, iv::InputAccess::sequential),
        };
    }

    void constrain_ports(
        iv::ConstrainPortsContext<RetainedDualEventFeedbackA>& ctx) const
    {
        ctx.input<"exact">().history() = 8;
        ctx.input<"converted">().history() = 8;
    }

    static constexpr auto outputs()
    {
        return std::array{iv::event_output({"out", "Out"}, iv::EventTypeId::trigger, iv::OutputProduction::sequential, {.max_events_per_index = 0.25})};
    }

    void tick_block(
        iv::TickBlockContext<RetainedDualEventFeedbackA> const& ctx) const
    {
        auto& state = ctx.state();
        auto const index = static_cast<std::size_t>(state.calls++);
        constexpr std::size_t history = 8;
        auto const begin = ctx.index.retreated_by({.whole = history}).whole();
        auto const count = ctx.block_size + history;
        auto const exact = ctx.template input<0>().events(begin, count);
        auto const converted = ctx.template input<1>().events(begin, count);
        if (index < state.indices.size()) {
            state.indices[index] = ctx.index.whole().bits();
            state.block_sizes[index] = ctx.block_size;
            state.exact_counts[index] = exact.size();
            state.converted_counts[index] = converted.size();
            if (!exact.empty()) {
                state.exact_first_times[index] = exact[0].time.bits();
                state.exact_last_times[index] =
                    exact[exact.size() - 1].time.bits();
            }
            if (!converted.empty()) {
                state.converted_first_times[index] = converted[0].time.bits();
                state.converted_last_times[index] =
                    converted[converted.size() - 1].time.bits();
            }
        }
        state.scc_feedback_latency = ctx.scc_feedback_latency;
        state.marker = 0xa11ce808u;
        auto const offset = std::min<std::size_t>(1, ctx.block_size - 1);
        ctx.template output<0>().push(
            iv::TriggerEvent{}, offset);
    }
};

struct DualEventFeedbackB {
    static constexpr auto inputs()
    {
        return std::array{
            iv::event_input({"in", "In"}, iv::EventTypeId::trigger, iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{
            iv::event_output({"exact", "Exact"}, iv::EventTypeId::trigger, iv::OutputProduction::sequential, {.max_events_per_index = 0.25}),
            iv::event_output({"converted", "Converted"}, iv::EventTypeId::boundary, iv::OutputProduction::sequential, {.max_events_per_index = 0.25}),
        };
    }

    void tick_block(iv::TickBlockContext<DualEventFeedbackB> const& ctx) const
    {
        (void)ctx.template input<0>().events();
        auto const offset = std::min<std::size_t>(2, ctx.block_size - 1);
        ctx.template output<0>().push(
            iv::TriggerEvent{}, offset);
        ctx.template output<1>().push(
            iv::BoundaryEvent{.is_begin = true},
            offset);
    }
};

struct TriggerEventConsumer {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t last_index = 0;
        std::uint64_t last_block_size = 0;
        std::uint64_t event_count = 0;
        std::uint64_t trigger_count = 0;
        std::uint64_t first_time = 0;
        std::uint64_t last_time = 0;
        std::uint32_t marker = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{
            iv::event_input({"trigger", "Trigger"}, iv::EventTypeId::trigger, iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(iv::TickBlockContext<TriggerEventConsumer> const& ctx) const
    {
        auto& state = ctx.state();
        auto const events = ctx.template input<0>().events();
        ++state.calls;
        state.last_index = ctx.index.whole().bits();
        state.last_block_size = ctx.block_size;
        state.event_count = events.size();
        state.trigger_count = 0;
        state.marker = 0xe71e17u;
        if (!events.empty()) {
            state.first_time = events[0].time.bits();
            state.last_time = events[events.size() - 1].time.bits();
        }
        for (auto const& event : events) {
            if (std::holds_alternative<iv::TriggerEvent>(event.value)) {
                ++state.trigger_count;
            }
        }
    }
};

struct RetainedMidiEventConsumer {
    struct State {
        std::uint64_t calls = 0;
        std::array<std::uint64_t, 4> indices{};
        std::array<std::uint64_t, 4> event_counts{};
        std::array<std::uint64_t, 4> midi_counts{};
        std::array<std::uint64_t, 4> first_times{};
        std::array<std::uint64_t, 4> last_times{};
    };

    static constexpr auto inputs()
    {
        return std::array{
            iv::event_input({"midi", "MIDI"}, iv::EventTypeId::midi, iv::InputAccess::sequential),
        };
    }

    void constrain_ports(
        iv::ConstrainPortsContext<RetainedMidiEventConsumer>& ctx) const
    {
        ctx.input<"midi">().history() = 160;
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(iv::TickBlockContext<RetainedMidiEventConsumer> const& ctx) const
    {
        auto& state = ctx.state();
        auto const index = state.calls++;
        if (index >= state.indices.size()) return;
        constexpr std::size_t history = 160;
        auto const events = ctx.template input<0>().events(
            ctx.index.retreated_by({.whole = history}).whole(), ctx.block_size + history);
        state.indices[index] = ctx.index.whole().bits();
        state.event_counts[index] = events.size();
        state.midi_counts[index] = 0;
        if (!events.empty()) {
            state.first_times[index] = events[0].time.bits();
            state.last_times[index] = events[events.size() - 1].time.bits();
        }
        for (auto const& event : events) {
            if (std::holds_alternative<iv::MidiEvent>(event.value)) {
                ++state.midi_counts[index];
            }
        }
    }
};

struct BoundaryEventFeedbackA {
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

    void tick_block(iv::TickBlockContext<BoundaryEventFeedbackA> const& ctx) const
    {
        (void)ctx.template input<0>().events();
        auto const offset = std::min<std::size_t>(1, ctx.block_size - 1);
        ctx.template output<0>().push(
            iv::BoundaryEvent{.is_begin = true},
            offset);
    }
};

struct BoundaryEventFeedbackB {
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

    void tick_block(iv::TickBlockContext<BoundaryEventFeedbackB> const& ctx) const
    {
        (void)ctx.template input<0>().events();
        auto const offset = std::min<std::size_t>(2, ctx.block_size - 1);
        ctx.template output<0>().push(
            iv::BoundaryEvent{.is_begin = true},
            offset);
    }
};

struct LimitedTriggerEventConsumer {
    struct State {
        std::uint64_t calls = 0;
        std::array<std::uint64_t, 4> indices{};
        std::array<std::uint64_t, 4> block_sizes{};
        std::array<std::uint64_t, 4> event_counts{};
        std::array<std::uint64_t, 4> trigger_counts{};
        std::array<std::uint64_t, 4> first_times{};
        std::array<std::uint64_t, 4> last_times{};
    };

    static constexpr auto inputs()
    {
        return std::array{
            iv::event_input({"trigger", "Trigger"}, iv::EventTypeId::trigger, iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    std::size_t max_block_size() const { return 32; }

    void tick_block(iv::TickBlockContext<LimitedTriggerEventConsumer> const& ctx) const
    {
        auto& state = ctx.state();
        auto const index = state.calls++;
        if (index >= state.indices.size()) return;
        auto const events = ctx.template input<0>().events();
        state.indices[index] = ctx.index.whole().bits();
        state.block_sizes[index] = ctx.block_size;
        state.event_counts[index] = events.size();
        state.trigger_counts[index] = 0;
        if (!events.empty()) {
            state.first_times[index] = events[0].time.bits();
            state.last_times[index] = events[events.size() - 1].time.bits();
        }
        for (auto const& event : events) {
            if (std::holds_alternative<iv::TriggerEvent>(event.value)) {
                ++state.trigger_counts[index];
            }
        }
    }
};

struct RetainedTriggerEventSource {
    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::array{
            iv::event_output({"trigger", "Trigger"}, iv::EventTypeId::trigger, iv::OutputProduction::sequential, {.max_events_per_index = 0.5}),
        };
    }

    void constrain_ports(
        iv::ConstrainPortsContext<RetainedTriggerEventSource>& ctx) const
    {
        ctx.output<"trigger">().latency() = 8;
    }

    void tick_block(iv::TickBlockContext<RetainedTriggerEventSource> const& ctx) const
    {
        if (ctx.block_size < 8) return;
        // In the latency window, before the previous block's end - 3 event.
        ctx.template output<0>().push(iv::TimedEvent{
            .time = ctx.index.whole().retreated_by(5),
            .value = iv::TriggerEvent{},
        });
        ctx.template output<0>().push(
            iv::TriggerEvent{}, 5);
        ctx.template output<0>().push(
            iv::TriggerEvent{}, ctx.block_size - 3);
    }
};

struct RetainedTriggerEventConsumer {
    struct State {
        std::uint64_t calls = 0;
        std::array<std::uint64_t, 3> indices{};
        std::array<std::uint64_t, 3> event_counts{};
        std::array<std::uint64_t, 3> first_times{};
        std::array<std::uint64_t, 3> second_times{};
        std::array<std::uint64_t, 3> last_times{};
    };

    static constexpr auto inputs()
    {
        return std::array{
            iv::event_input({"trigger", "Trigger"}, iv::EventTypeId::trigger, iv::InputAccess::sequential),
        };
    }

    void constrain_ports(
        iv::ConstrainPortsContext<RetainedTriggerEventConsumer>& ctx) const
    {
        ctx.input<"trigger">().history() = 8;
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(iv::TickBlockContext<RetainedTriggerEventConsumer> const& ctx) const
    {
        auto& state = ctx.state();
        auto const index = state.calls++;
        if (index >= state.indices.size()) return;
        constexpr std::size_t history = 8;
        auto const events = ctx.template input<0>().events(
            ctx.index.retreated_by({.whole = history}).whole(), ctx.block_size + history);
        state.indices[index] = ctx.index.whole().bits();
        state.event_counts[index] = events.size();
        if (!events.empty()) {
            state.first_times[index] = events[0].time.bits();
            state.last_times[index] = events[events.size() - 1].time.bits();
        }
        if (events.size() > 1) {
            state.second_times[index] = events[1].time.bits();
        }
    }
};

struct PersistentEventRingSource {
    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::array{
            iv::event_output({"trigger", "Trigger"}, iv::EventTypeId::trigger, iv::OutputProduction::sequential, {.max_events_per_index = 0.5}),
        };
    }

    void tick_block(iv::TickBlockContext<PersistentEventRingSource> const& ctx) const
    {
        if (ctx.block_size < 8) return;
        // Concentrating all events at one timestamp also proves the sizing-rate
        // field is not interpreted as a per-sample runtime limiter.
        for (std::size_t i = 0; i < 32; ++i) {
            ctx.template output<0>().push(
                iv::TriggerEvent{}, 5);
        }
    }
};

struct PersistentEventRingConsumer {
    struct State {
        std::uint64_t calls = 0;
        std::array<std::uint64_t, 5> indices{};
        std::array<std::uint64_t, 5> event_counts{};
        std::array<std::uint64_t, 5> first_times{};
        std::array<std::uint64_t, 5> last_times{};
    };

    static constexpr auto inputs()
    {
        return std::array{
            iv::event_input({"trigger", "Trigger"}, iv::EventTypeId::trigger, iv::InputAccess::sequential),
        };
    }

    void constrain_ports(
        iv::ConstrainPortsContext<PersistentEventRingConsumer>& ctx) const
    {
        ctx.input<"trigger">().history() = 160;
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(iv::TickBlockContext<PersistentEventRingConsumer> const& ctx) const
    {
        auto& state = ctx.state();
        auto const index = state.calls++;
        if (index >= state.indices.size()) return;
        constexpr std::size_t history = 160;
        auto const events = ctx.template input<0>().events(
            ctx.index.retreated_by({.whole = history}).whole(), ctx.block_size + history);
        state.indices[index] = ctx.index.whole().bits();
        state.event_counts[index] = events.size();
        if (!events.empty()) {
            state.first_times[index] = events[0].time.bits();
            state.last_times[index] = events[events.size() - 1].time.bits();
        }
    }
};

struct PersistentEventFeedbackConsumer {
    struct State {
        std::uint64_t calls = 0;
        std::array<std::uint64_t, 4> indices{};
        std::array<std::uint64_t, 4> event_counts{};
        std::array<std::uint64_t, 4> first_times{};
        std::array<std::uint64_t, 4> last_times{};
        std::uint64_t marker = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{
            iv::event_input({"trigger", "Trigger"}, iv::EventTypeId::trigger, iv::InputAccess::sequential),
        };
    }

    void constrain_ports(
        iv::ConstrainPortsContext<PersistentEventFeedbackConsumer>& ctx) const
    {
        ctx.input<"trigger">().history() = 320;
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(
        iv::TickBlockContext<PersistentEventFeedbackConsumer> const& ctx) const
    {
        auto& state = ctx.state();
        auto const index = state.calls++;
        if (index >= state.indices.size()) return;
        constexpr std::size_t history = 320;
        auto const events = ctx.template input<0>().events(
            ctx.index.retreated_by({.whole = history}).whole(), ctx.block_size + history);
        state.indices[index] = ctx.index.whole().bits();
        state.event_counts[index] = events.size();
        if (!events.empty()) {
            state.first_times[index] = events[0].time.bits();
            state.last_times[index] = events[events.size() - 1].time.bits();
        }
        state.marker = 0xfeed320u;
    }
};

struct PortedProbe {
    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::array<iv::SequentialOutputConfig<iv::Sample>, 1>{};
    }

    void tick(iv::TickSampleContext<PortedProbe> const& ctx) const
    {
        ctx.template output<0>().push(iv::Sample{0.25f});
    }
};

void stateful_module(iv::GraphBuilder& graph)
{
    (void)graph.node<"iv.test.graph_jit.state_context.stateful">();
    graph.outputs();
}

void state_only_module(iv::GraphBuilder& graph)
{
    (void)graph.node<"iv.test.graph_jit.state_context.state_only">();
    graph.outputs();
}

void background_only_module(iv::GraphBuilder& graph)
{
    (void)graph.node<"iv.test.graph_jit.state_context.background_only">();
    graph.outputs();
}

void background_execution_module(iv::GraphBuilder& graph)
{
    (void)graph.node<"iv.test.graph_jit.state_context.background_evaluation_call">();
    graph.outputs();
}

void stateless_module(iv::GraphBuilder& graph)
{
    (void)graph.node<"iv.test.graph_jit.state_context.stateless">();
    graph.outputs();
}

void configured_module(iv::GraphBuilder& graph)
{
    (void)graph.node<"iv.test.graph_jit.state_context.configured">(
        std::size_t{0x12345678u}, std::size_t{0xabcdef01u});
    graph.outputs();
}

void pointer_configured_module(iv::GraphBuilder& graph)
{
    (void)graph.node<"iv.test.graph_jit.state_context.pointer_configured">(
        std::uint32_t{0x89abcdefu},
        pointer_probe_values.data() + 1,
        pointer_probe_values.data() + 3,
        static_cast<std::uint64_t const*>(nullptr),
        std::uint16_t{0x4567u});
    graph.outputs();
}

void multiple_module(iv::GraphBuilder& graph)
{
    (void)graph.node<"iv.test.graph_jit.state_context.stateful">();
    (void)graph.node<"iv.test.graph_jit.state_context.configured">(
        std::size_t{1}, std::size_t{2});
    graph.outputs();
}

void skippable_pair_module(iv::GraphBuilder& graph)
{
    (void)graph.node<"iv.test.graph_jit.state_context.stateful">();
    (void)graph.node<"iv.test.graph_jit.state_context.stateful">();
    graph.outputs();
}

void limited_block_module(iv::GraphBuilder& graph)
{
    (void)graph.node<"iv.test.graph_jit.state_context.limited_block">();
    graph.outputs();
}

void disconnected_sample_input_module(iv::GraphBuilder& graph)
{
    (void)graph.node<"iv.test.graph_jit.state_context.disconnected_sample_input_probe">();
    graph.outputs();
}

void manual_control_slice_module(iv::GraphBuilder& graph)
{
    (void)graph.node<
        "iv.test.graph_jit.state_context.manual_control_slice_probe">();
    graph.outputs();
}

void circular_manual_control_probe_module(iv::GraphBuilder& graph)
{
    auto const azimuth = graph.input<iv::Sample>("Azimuth",
        {.default_value = 179, .min = -180, .max = 180, .geometry = iv::KnobGeometry::circle});
    auto probe = graph.node<"iv.test.graph_jit.state_context.manual_control_slice_probe">();
    probe(azimuth);
    graph.outputs();
}

void circular_manual_control_slice_module(iv::GraphBuilder& graph)
{
    (void)graph.node<"iv.test.graph_jit.circular_manual_control_probe">();
    graph.outputs();
}

void disconnected_sample_output_module(iv::GraphBuilder& graph)
{
    auto source =
        graph.node<"iv.test.graph_jit.state_context.history_ramp_source">();
    (void)source;
    graph.outputs();
}

void tick_capture_module(iv::GraphBuilder& graph)
{
    (void)graph.node<"iv.test.graph_jit.state_context.tick_capture_source">();
    graph.outputs();
}

void direct_sample_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.sample_consumer">();
    sink(source);
    graph.outputs();
}

void transient_sample_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.limited_sample_ramp_source">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.sample_consumer">();
    sink(source);
    graph.outputs();
}

void reused_sample_arena_module(iv::GraphBuilder& graph)
{
    auto source_a = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto sink_a = graph.node<"iv.test.graph_jit.state_context.sample_consumer">();
    sink_a(source_a);
    auto source_b = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto sink_b = graph.node<"iv.test.graph_jit.state_context.sample_consumer">();
    sink_b(source_b);
    graph.outputs();
}

void sample_fanout_conversion_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto mono_sink = graph.node<"iv.test.graph_jit.state_context.sample_consumer">();
    auto mono_interleaved_sink = graph.node<"iv.test.graph_jit.state_context.mono_interleaved_consumer">();
    auto stereo_sink_a = graph.node<"iv.test.graph_jit.state_context.stereo_sample_consumer">();
    auto stereo_sink_b = graph.node<"iv.test.graph_jit.state_context.stereo_sample_consumer">();
    mono_sink(source);
    mono_interleaved_sink(source);
    stereo_sink_a(source);
    stereo_sink_b(source);
    graph.outputs();
}

void sample_revision_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.revising_sample_source">();
    auto mono_sink = graph.node<"iv.test.graph_jit.state_context.sample_consumer">();
    auto stereo_sink = graph.node<"iv.test.graph_jit.state_context.stereo_sample_consumer">();
    mono_sink(source);
    stereo_sink(source);
    graph.outputs();
}

void persistent_sample_revision_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.persistent_revising_sample_source">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.sample_consumer">();
    sink(source);
    graph.outputs();
}

void composed_sample_revision_module(iv::GraphBuilder& graph)
{
    auto left = graph.node<"iv.test.graph_jit.state_context.revising_sample_source">();
    auto right = graph.node<"iv.test.graph_jit.state_context.revising_sample_source">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.stereo_sample_consumer">();
    sink(graph.tile<iv::stereo>(left, right));
    graph.outputs();
}

void tick_fallback_sample_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.tick_fallback_sample_source">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.sample_consumer">();
    sink(source);
    graph.outputs();
}

void stereo_conversion_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.stereo_ramp_source">();
    auto mono_sink = graph.node<"iv.test.graph_jit.state_context.sample_consumer">();
    auto stereo_planar_sink = graph.node<"iv.test.graph_jit.state_context.stereo_planar_consumer">();
    mono_sink(source);
    stereo_planar_sink(source);
    graph.outputs();
}

void stereo_history_conversion_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.stereo_ramp_source">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.history_consumer">();
    sink(source);
    graph.outputs();
}

void history_fanout_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.history_ramp_source">();
    auto mono_sink = graph.node<"iv.test.graph_jit.state_context.history_consumer">();
    auto stereo_sink = graph.node<"iv.test.graph_jit.state_context.stereo_history_consumer">();
    mono_sink(source);
    stereo_sink(source);
    graph.outputs();
}

void persistent_history_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.large_history_ramp_source">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.large_history_consumer">();
    sink(source);
    graph.outputs();
}

void large_history_rewire_module(iv::GraphBuilder& graph)
{
    auto first_source =
        graph.node<"iv.test.graph_jit.state_context.large_history_ramp_source">();
    auto second_source =
        graph.node<"iv.test.graph_jit.state_context.large_history_ramp_source">();
    auto first_consumer =
        graph.node<"iv.test.graph_jit.state_context.large_history_consumer">();
    auto second_consumer =
        graph.node<"iv.test.graph_jit.state_context.large_history_consumer">();
    first_consumer(first_source);
    second_consumer(second_source);
    graph.outputs();
}

void latency_compensation_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto delayed = graph.node<"iv.test.graph_jit.state_context.five_sample_delay">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.latency_compensation_probe">();
    delayed(source);
    sink(source, delayed);
    graph.outputs();
}

void latency_conversion_fanout_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto delayed = graph.node<"iv.test.graph_jit.state_context.five_sample_delay">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.interleaved_latency_compensation_probe">();
    auto observer = graph.node<"iv.test.graph_jit.state_context.mono_interleaved_consumer">();
    delayed(source);
    sink(source, delayed);
    observer(source);
    graph.outputs();
}

void composed_latency_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto delayed = graph.node<"iv.test.graph_jit.state_context.five_sample_delay">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.stereo_sample_consumer">();
    delayed(source);
    sink(graph.tile<iv::stereo>(source, delayed));
    graph.outputs();
}

void composed_history_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto delayed = graph.node<"iv.test.graph_jit.state_context.five_sample_delay">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.stereo_history_consumer">();
    delayed(source);
    sink(graph.tile<iv::stereo>(source, delayed));
    graph.outputs();
}

void projected_composition_module(iv::GraphBuilder& graph)
{
    auto stereo_source = graph.node<"iv.test.graph_jit.state_context.stereo_ramp_source">();
    auto mono_source = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto delayed = graph.node<"iv.test.graph_jit.state_context.five_sample_delay">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.stereo_sample_consumer">();
    delayed(mono_source);
    sink(graph.tile<iv::stereo>(stereo_source[iv::stereo::right], delayed));
    graph.outputs();
}

void direct_event_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.trigger_event_source">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.trigger_event_consumer">();
    sink.connect_event_input(0, source.event_port());
    graph.outputs();
}

void transient_event_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.limited_trigger_event_source">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.limited_trigger_event_consumer">();
    sink.connect_event_input(0, source.event_port());
    graph.outputs();
}

void converted_event_fanout_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.midi_event_source">();
    auto sink_a = graph.node<"iv.test.graph_jit.state_context.trigger_event_consumer">();
    auto sink_b = graph.node<"iv.test.graph_jit.state_context.trigger_event_consumer">();
    sink_a.connect_event_input(0, source.event_port());
    sink_b.connect_event_input(0, source.event_port());
    graph.outputs();
}

void retained_event_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.retained_trigger_event_source">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.retained_trigger_event_consumer">();
    sink.connect_event_input(0, source.event_port());
    graph.outputs();
}

void persistent_event_ring_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.persistent_event_ring_source">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.persistent_event_ring_consumer">();
    sink.connect_event_input(0, source.event_port());
    graph.outputs();
}

void retained_converted_event_fanout_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.midi_event_source">();
    auto retained = graph.node<"iv.test.graph_jit.state_context.retained_midi_event_consumer">();
    auto trigger_a = graph.node<"iv.test.graph_jit.state_context.trigger_event_consumer">();
    auto trigger_b = graph.node<"iv.test.graph_jit.state_context.limited_trigger_event_consumer">();
    retained.connect_event_input(0, source.event_port());
    trigger_a.connect_event_input(0, source.event_port());
    trigger_b.connect_event_input(0, source.event_port());
    graph.outputs();
}

void ported_module(iv::GraphBuilder& graph)
{
    graph.outputs(graph.node<"iv.test.graph_jit.state_context.ported">());
}
} // namespace

IV_NODE("iv.test.graph_jit.state_context.stateful", StatefulProbe, "Stateful");
IV_NODE("iv.test.graph_jit.state_context.state_only", StateOnlyProbe, "State only");
IV_NODE("iv.test.graph_jit.state_context.background_only", BackgroundOnlyProbe, "Background only");
IV_NODE("iv.test.graph_jit.state_context.background_evaluation_call", BackgroundExecutionProbe, "Background evaluation call");
IV_NODE("iv.test.graph_jit.state_context.stateless", StatelessProbe, "Stateless");
IV_NODE("iv.test.graph_jit.state_context.configured", ConfiguredProbe, "Configured");
IV_NODE(
    "iv.test.graph_jit.state_context.message_replacement",
    MessageReplacementProbe, "Message replacement");
IV_NODE(
    "iv.test.graph_jit.state_context.message_successor",
    MessageSuccessorProbe, "Message successor");
IV_NODE("iv.test.graph_jit.state_context.pointer_configured", PointerConfiguredProbe, "Pointer configured");
IV_NODE("iv.test.graph_jit.state_context.limited_block", LimitedBlockProbe, "Limited block");
IV_NODE("iv.test.graph_jit.state_context.sample_ramp_source", SampleRampSource, "Sample ramp source");
IV_NODE("iv.test.graph_jit.state_context.revising_sample_source", RevisingSampleSource, "Revising sample source");
IV_NODE("iv.test.graph_jit.state_context.tick_capture_source", TickCaptureSource, "Tick capture source");
IV_NODE("iv.test.graph_jit.state_context.persistent_revising_sample_source", PersistentRevisingSampleSource, "Persistent revising sample source");
IV_NODE("iv.test.graph_jit.state_context.tick_fallback_sample_source", TickFallbackSampleSource, "Tick fallback sample source");
IV_NODE("iv.test.graph_jit.state_context.limited_sample_ramp_source", LimitedSampleRampSource, "Limited sample ramp source");
IV_NODE("iv.test.graph_jit.state_context.sample_consumer", SampleConsumerProbe, "Sample consumer");
IV_NODE("iv.test.graph_jit.state_context.sample_feedback_a", SampleFeedbackA, "Sample feedback a");
IV_NODE("iv.test.graph_jit.state_context.sample_feedback_b", SampleFeedbackB, "Sample feedback b");
IV_NODE("iv.test.graph_jit.state_context.multi_branch_sample_feedback", MultiBranchSampleFeedback, "Multi branch sample feedback");
IV_NODE("iv.test.graph_jit.state_context.temporal_sample_feedback", TemporalSampleFeedback, "Temporal sample feedback");
IV_NODE("iv.test.graph_jit.state_context.revising_sample_feedback", RevisingSampleFeedback, "Revising sample feedback");
IV_NODE("iv.test.graph_jit.state_context.projected_revising_sample_feedback", ProjectedRevisingSampleFeedback, "Projected revising sample feedback");
IV_NODE("iv.test.graph_jit.state_context.converted_sample_feedback", ConvertedSampleFeedback, "Converted sample feedback");
IV_NODE("iv.test.graph_jit.state_context.mono_interleaved_consumer", MonoInterleavedConsumerProbe, "Mono interleaved consumer");
IV_NODE("iv.test.graph_jit.state_context.stereo_ramp_source", StereoRampSource, "Stereo ramp source");
IV_NODE("iv.test.graph_jit.state_context.stereo_planar_consumer", StereoPlanarConsumerProbe, "Stereo planar consumer");
IV_NODE("iv.test.graph_jit.state_context.stereo_sample_consumer", StereoSampleConsumerProbe, "Stereo sample consumer");
IV_NODE("iv.test.graph_jit.state_context.history_ramp_source", HistoryRampSource, "History ramp source");
IV_NODE("iv.test.graph_jit.state_context.history_consumer", HistoryConsumerProbe, "History consumer");
IV_NODE("iv.test.graph_jit.state_context.stereo_history_consumer", StereoHistoryConsumerProbe, "Stereo history consumer");
IV_NODE("iv.test.graph_jit.state_context.large_history_ramp_source", LargeHistoryRampSource, "Large history ramp source");
IV_NODE("iv.test.graph_jit.state_context.large_history_consumer", LargeHistoryConsumerProbe, "Large history consumer");
IV_NODE("iv.test.graph_jit.state_context.five_sample_delay", FiveSampleDelay, "Five sample delay");
IV_NODE("iv.test.graph_jit.state_context.latency_compensation_probe", LatencyCompensationProbe, "Latency compensation probe");
IV_NODE("iv.test.graph_jit.state_context.interleaved_latency_compensation_probe", InterleavedLatencyCompensationProbe, "Interleaved latency compensation probe");
IV_NODE("iv.test.graph_jit.state_context.disconnected_sample_input_probe", DisconnectedSampleInputProbe, "Disconnected sample input probe");
IV_NODE("iv.test.graph_jit.state_context.manual_control_slice_probe", ManualControlSliceProbe, "Manual control slice probe");
IV_NODE("iv.test.graph_jit.state_context.trigger_event_source", TriggerEventSource, "Trigger event source");
IV_NODE("iv.test.graph_jit.state_context.midi_event_source", MidiEventSource, "MIDI event source");
IV_NODE("iv.test.graph_jit.state_context.limited_trigger_event_source", LimitedTriggerEventSource, "Limited trigger event source");
IV_NODE("iv.test.graph_jit.state_context.fan_in_burst_event_source", FanInBurstEventSource, "Fan in burst event source");
IV_NODE("iv.test.graph_jit.state_context.fan_in_sparse_event_source", FanInSparseEventSource, "Fan in sparse event source");
IV_NODE("iv.test.graph_jit.state_context.event_feedback_a", EventFeedbackA, "Event feedback a");
IV_NODE("iv.test.graph_jit.state_context.retained_fan_in_event_feedback_a", RetainedFanInEventFeedbackA, "Retained fan in event feedback a");
IV_NODE("iv.test.graph_jit.state_context.latent_event_feedback_a", LatentEventFeedbackA, "Latent event feedback a");
IV_NODE("iv.test.graph_jit.state_context.persistent_latent_event_feedback_a", PersistentLatentEventFeedbackA, "Persistent latent event feedback a");
IV_NODE("iv.test.graph_jit.state_context.persistent_latent_boundary_event_feedback_a", PersistentLatentBoundaryEventFeedbackA, "Persistent latent boundary event feedback a");
IV_NODE("iv.test.graph_jit.state_context.event_feedback_b", EventFeedbackB, "Event feedback b");
IV_NODE("iv.test.graph_jit.state_context.boundary_event_feedback_a", BoundaryEventFeedbackA, "Boundary event feedback a");
IV_NODE("iv.test.graph_jit.state_context.boundary_event_feedback_b", BoundaryEventFeedbackB, "Boundary event feedback b");
IV_NODE("iv.test.graph_jit.state_context.event_feedback_burst_a", EventFeedbackBurstA, "Event feedback burst a");
IV_NODE("iv.test.graph_jit.state_context.event_feedback_fanout_a", EventFeedbackFanoutA, "Event feedback fanout a");
IV_NODE("iv.test.graph_jit.state_context.retained_dual_event_feedback_a", RetainedDualEventFeedbackA, "Retained dual event feedback a");
IV_NODE("iv.test.graph_jit.state_context.dual_event_feedback_b", DualEventFeedbackB, "Dual event feedback b");
IV_NODE("iv.test.graph_jit.state_context.trigger_event_consumer", TriggerEventConsumer, "Trigger event consumer");
IV_NODE("iv.test.graph_jit.state_context.limited_trigger_event_consumer", LimitedTriggerEventConsumer, "Limited trigger event consumer");
IV_NODE("iv.test.graph_jit.state_context.retained_trigger_event_source", RetainedTriggerEventSource, "Retained trigger event source");
IV_NODE("iv.test.graph_jit.state_context.retained_trigger_event_consumer", RetainedTriggerEventConsumer, "Retained trigger event consumer");
IV_NODE("iv.test.graph_jit.state_context.persistent_event_ring_source", PersistentEventRingSource, "Persistent event ring source");
IV_NODE("iv.test.graph_jit.state_context.persistent_event_ring_consumer", PersistentEventRingConsumer, "Persistent event ring consumer");
IV_NODE("iv.test.graph_jit.state_context.persistent_event_feedback_consumer", PersistentEventFeedbackConsumer, "Persistent event feedback consumer");
IV_NODE("iv.test.graph_jit.state_context.retained_midi_event_consumer", RetainedMidiEventConsumer, "Retained MIDI event consumer");
IV_NODE("iv.test.graph_jit.state_context.ported", PortedProbe, "Ported");
IV_MODULE("iv.test.graph_jit.state_context.stateful_module", stateful_module, "Stateful module");
IV_MODULE("iv.test.graph_jit.state_context.state_only_module", state_only_module, "State only module");
IV_MODULE("iv.test.graph_jit.state_context.background_only_module", background_only_module, "Background only module");
IV_MODULE("iv.test.graph_jit.state_context.background_execution_module", background_execution_module, "Background execution module");
IV_MODULE("iv.test.graph_jit.state_context.stateless_module", stateless_module, "Stateless module");
IV_MODULE("iv.test.graph_jit.state_context.configured_module", configured_module, "Configured module");
IV_MODULE("iv.test.graph_jit.state_context.pointer_configured_module", pointer_configured_module, "Pointer configured module");
IV_MODULE("iv.test.graph_jit.state_context.multiple_module", multiple_module, "Multiple module");
IV_MODULE("iv.test.graph_jit.state_context.skippable_pair_module", skippable_pair_module, "Skippable pair module");
IV_MODULE("iv.test.graph_jit.state_context.limited_block_module", limited_block_module, "Limited block module");
IV_MODULE("iv.test.graph_jit.state_context.disconnected_sample_input_module", disconnected_sample_input_module, "Disconnected sample input module");
IV_MODULE("iv.test.graph_jit.state_context.manual_control_slice_module", manual_control_slice_module, "Manual control slice module");
IV_MODULE("iv.test.graph_jit.circular_manual_control_probe", circular_manual_control_probe_module, "Circular manual control probe");
IV_MODULE("iv.test.graph_jit.circular_manual_control_slice", circular_manual_control_slice_module, "Circular manual control slice");
IV_MODULE("iv.test.graph_jit.state_context.disconnected_sample_output_module", disconnected_sample_output_module, "Disconnected sample output module");
IV_MODULE("iv.test.graph_jit.state_context.tick_capture_module", tick_capture_module, "Tick capture module");
IV_MODULE("iv.test.graph_jit.state_context.direct_sample_module", direct_sample_module, "Direct sample module");
IV_MODULE("iv.test.graph_jit.state_context.transient_sample_module", transient_sample_module, "Transient sample module");
IV_MODULE("iv.test.graph_jit.state_context.reused_sample_arena_module", reused_sample_arena_module, "Reused sample arena module");
IV_MODULE("iv.test.graph_jit.state_context.sample_fanout_conversion_module", sample_fanout_conversion_module, "Sample fanout conversion module");
IV_MODULE("iv.test.graph_jit.state_context.sample_revision_module", sample_revision_module, "Sample revision module");
IV_MODULE("iv.test.graph_jit.state_context.persistent_sample_revision_module", persistent_sample_revision_module, "Persistent sample revision module");
IV_MODULE("iv.test.graph_jit.state_context.composed_sample_revision_module", composed_sample_revision_module, "Composed sample revision module");
IV_MODULE("iv.test.graph_jit.state_context.tick_fallback_sample_module", tick_fallback_sample_module, "Tick fallback sample module");
IV_MODULE("iv.test.graph_jit.state_context.stereo_conversion_module", stereo_conversion_module, "Stereo conversion module");
IV_MODULE("iv.test.graph_jit.state_context.stereo_history_conversion_module", stereo_history_conversion_module, "Stereo history conversion module");
IV_MODULE("iv.test.graph_jit.state_context.history_fanout_module", history_fanout_module, "History fanout module");
IV_MODULE("iv.test.graph_jit.state_context.persistent_history_module", persistent_history_module, "Persistent history module");
IV_MODULE(
    "iv.test.graph_jit.state_context.large_history_rewire_module",
    large_history_rewire_module, "Large history rewire module");
IV_MODULE("iv.test.graph_jit.state_context.latency_compensation_module", latency_compensation_module, "Latency compensation module");
IV_MODULE("iv.test.graph_jit.state_context.latency_conversion_fanout_module", latency_conversion_fanout_module, "Latency conversion fanout module");
IV_MODULE("iv.test.graph_jit.state_context.composed_latency_module", composed_latency_module, "Composed latency module");
IV_MODULE("iv.test.graph_jit.state_context.composed_history_module", composed_history_module, "Composed history module");
IV_MODULE("iv.test.graph_jit.state_context.projected_composition_module", projected_composition_module, "Projected composition module");
IV_MODULE("iv.test.graph_jit.state_context.direct_event_module", direct_event_module, "Direct event module");
IV_MODULE("iv.test.graph_jit.state_context.transient_event_module", transient_event_module, "Transient event module");
IV_MODULE("iv.test.graph_jit.state_context.converted_event_fanout_module", converted_event_fanout_module, "Converted event fanout module");
IV_MODULE("iv.test.graph_jit.state_context.retained_event_module", retained_event_module, "Retained event module");
IV_MODULE("iv.test.graph_jit.state_context.persistent_event_ring_module", persistent_event_ring_module, "Persistent event ring module");
IV_MODULE("iv.test.graph_jit.state_context.retained_converted_event_fanout_module", retained_converted_event_fanout_module, "Retained converted event fanout module");
IV_MODULE("iv.test.graph_jit.state_context.ported_module", ported_module, "Ported module");
