struct NativeBatchFeedbackProbe {
    std::uint64_t id = 0;

    struct State {
        std::uint64_t tick_batch_calls = 0;
        std::uint64_t last_lane_count = 0;
        std::uint64_t configured_id = 0;
        float last_input = 0.0f;
        float last_output = 0.0f;
    };

    constexpr explicit NativeBatchFeedbackProbe(std::uint64_t id_) : id(id_) {}

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>(
            {"in", "In"}, iv::InputAccess::sequential)};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>(
            {"out", "Out"}, iv::OutputProduction::sequential)};
    }

    static void tick_block_batch(
        iv::TickBlockBatchContext<NativeBatchFeedbackProbe> const& context)
    {
        for (auto lane : context) {
            auto input = lane.template input<"in">().block();
            auto output = lane.template output<"out">().block();
            auto& state = lane.state();
            ++state.tick_batch_calls;
            state.last_lane_count = context.size();
            state.configured_id = lane.node().id;
            for (std::size_t i = 0; i < output.size(); ++i) {
                output[i] = input[i] + static_cast<float>(lane.node().id);
                state.last_input = input[i];
                state.last_output = output[i];
            }
        }
    }
};

void native_batch_feedback_module(iv::GraphBuilder& graph)
{
    auto first_head = graph.node<
        "iv.test.graph_jit.state_context.native_batch_feedback">(
        std::uint64_t{1});
    auto first_tail = graph.node<
        "iv.test.graph_jit.state_context.native_batch_feedback">(
        std::uint64_t{2});
    auto second_head = graph.node<
        "iv.test.graph_jit.state_context.native_batch_feedback">(
        std::uint64_t{10});
    auto second_tail = graph.node<
        "iv.test.graph_jit.state_context.native_batch_feedback">(
        std::uint64_t{20});
    first_head(static_cast<iv::SamplePortRef>(first_tail).detach(1));
    first_tail(first_head);
    second_head(static_cast<iv::SamplePortRef>(second_tail).detach(1));
    second_tail(second_head);
    graph.outputs();
}

IV_NODE(
    "iv.test.graph_jit.state_context.native_batch_feedback",
    NativeBatchFeedbackProbe, "Native batch feedback");
IV_MODULE(
    "iv.test.graph_jit.state_context.native_batch_feedback_module",
    native_batch_feedback_module, "Native batch feedback module");

// A MIDI source read by a MIDI consumer with history 160, and with `added`
// also by a trigger consumer with history 320, through a conversion.
void added_event_consumer_module(iv::GraphBuilder& graph, bool added = false)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.midi_event_source">();
    auto first = graph.node<"iv.test.graph_jit.state_context.retained_midi_event_consumer">();
    first.connect_event_input(0, source.event_port());
    if (added) {
        auto second = graph.node<"iv.test.graph_jit.state_context.persistent_event_feedback_consumer">();
        second.connect_event_input(0, source.event_port());
    }
    graph.outputs();
}
IV_MODULE("iv.test.graph_jit.state_context.added_event_consumer_module", added_event_consumer_module, "Added event consumer module");

// A sample input and a trigger input, which latency compensation aligns:
// records the trigger positions each call reads.
struct CompensatedTriggerProbe {
    struct State {
        std::uint64_t calls = 0;
        std::array<std::uint64_t, 4> counts{};
        std::array<std::uint64_t, 4> first_times{};
        std::array<std::uint64_t, 4> last_times{};
    };
    static constexpr auto inputs()
    {
        return std::tuple{
            iv::input<iv::Sample>({"audio", "Audio"}, iv::InputAccess::sequential),
            iv::event_input({"trigger", "Trigger"}, iv::EventTypeId::trigger, iv::InputAccess::sequential),
        };
    }
    static constexpr auto outputs() { return std::tuple{}; }
    void tick_block(iv::TickBlockContext<CompensatedTriggerProbe> const& ctx) const
    {
        auto& state = ctx.state();
        auto const call = state.calls++;
        if (call >= state.counts.size()) return;
        auto const events = ctx.input<"trigger">().events();
        state.counts[call] = events.size();
        if (!events.empty()) {
            state.first_times[call] = events[0].time.bits();
            state.last_times[call] = events[events.size() - 1].time.bits();
        }
    }
};
IV_NODE("iv.test.graph_jit.state_context.compensated_trigger_probe", CompensatedTriggerProbe, "Compensated trigger probe");

// The probe's sample input reads a ramp of latency 7, so its trigger input
// reads the trigger source with compensation 7.
void compensated_trigger_module(iv::GraphBuilder& graph)
{
    auto ramp = graph.node<"iv.test.graph_jit.state_context.latency_argument_ramp_source">(std::size_t{7});
    auto source = graph.node<"iv.test.graph_jit.state_context.trigger_event_source">();
    auto probe = graph.node<"iv.test.graph_jit.state_context.compensated_trigger_probe">();
    probe(ramp);
    probe.connect_event_input(0, source.event_port());
    graph.outputs();
}
IV_MODULE("iv.test.graph_jit.state_context.compensated_trigger_module", compensated_trigger_module, "Compensated trigger module");

// A trigger source drives a subgraph's event input, which feeds the probe,
// whose sample input reads a ramp of latency 7, and a consumer (history 8)
// without other inputs: one authored connection with two targets, which need
// compensation 7 and 0 for the same source.
void routed_unequal_compensation_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.trigger_event_source">();
    auto routed = graph.subgraph([&](iv::SubgraphBuilder& boundary) {
        auto trigger = boundary.event_input({"Trigger"}, iv::EventTypeId::trigger);
        auto ramp = graph.node<"iv.test.graph_jit.state_context.latency_argument_ramp_source">(std::size_t{7});
        auto probe = graph.node<"iv.test.graph_jit.state_context.compensated_trigger_probe">();
        probe(ramp);
        probe.connect_event_input(0, trigger);
        auto plain = graph.node<"iv.test.graph_jit.state_context.retained_trigger_event_consumer">();
        plain.connect_event_input(0, trigger);
        boundary.event_outputs();
    });
    routed.connect_event_input(0, source.event_port());
    graph.outputs();
}
IV_MODULE("iv.test.graph_jit.state_context.routed_unequal_compensation_module", routed_unequal_compensation_module, "Routed unequal compensation module");

// A trigger output of history 8 and the latency its argument gives. Each
// block authors one trigger just before itself, in its latency window, one at
// 5, and one 2 positions before its end, which is not final when it ends.
struct LatentTriggerSource {
    std::size_t latency = 1;
    explicit LatentTriggerSource(std::size_t latency_ = 1) : latency(latency_) {}
    static constexpr auto inputs() { return std::tuple{}; }
    static constexpr auto outputs()
    {
        return std::array{iv::event_output({"trigger", "Trigger"}, iv::EventTypeId::trigger, iv::OutputProduction::sequential, {.max_events_per_index = 0.5})};
    }
    void constrain_ports(iv::ConstrainPortsContext<LatentTriggerSource>& ctx) const
    {
        ctx.output<"trigger">().history() = 8;
        ctx.output<"trigger">().latency() = latency;
    }
    void tick_block(iv::TickBlockContext<LatentTriggerSource> const& ctx) const
    {
        if (ctx.block_size < 8) return;
        ctx.output<"trigger">().push(iv::TimedEvent{
            .time = ctx.index.whole().retreated_by(1),
            .value = iv::TriggerEvent{},
        });
        ctx.output<"trigger">().push(iv::TriggerEvent{}, 5);
        ctx.output<"trigger">().push(iv::TriggerEvent{}, ctx.block_size - 2);
    }
};
IV_NODE("iv.test.graph_jit.state_context.latent_trigger_source", LatentTriggerSource, "Latent trigger source");

// The latent source read by a consumer of history 8, which reads the
// source's own retained stream: the source keeps at least that history.
void latent_direct_event_module(iv::GraphBuilder& graph, std::size_t latency = 4)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.latent_trigger_source">(latency);
    auto sink = graph.node<"iv.test.graph_jit.state_context.retained_trigger_event_consumer">();
    sink.connect_event_input(0, source.event_port());
    graph.outputs();
}
IV_MODULE("iv.test.graph_jit.state_context.latent_direct_event_module", latent_direct_event_module, "Latent direct event module");

// A persisted sample output at pace 2 with latency 1 (one value). Each value is
// first written as its value index and revised, while the next value is
// written, to 1000 plus its value index.
struct RevisingPacedCaptureSource {
    static constexpr auto inputs() { return std::tuple{}; }
    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>({"samples", "Samples"}, iv::OutputProduction::sequential, iv::OutputRealization::buffered)};
    }
    void constrain_ports(iv::ConstrainPortsContext<RevisingPacedCaptureSource>& ctx) const
    {
        ctx.output<"samples">().pace() = 2;
        ctx.output<"samples">().latency() = 1;
    }
    void tick_block(iv::TickBlockContext<RevisingPacedCaptureSource> const& ctx) const
    {
        auto samples = ctx.output<"samples">();
        for (std::size_t offset = 0; offset < samples.block_size(); ++offset) {
            auto const index = static_cast<std::int64_t>(samples.value_index() + offset);
            samples.update(iv::Sample{static_cast<float>(999 + index)});
            samples.push(iv::Sample{static_cast<float>(index)});
        }
    }
};
IV_NODE("iv.test.graph_jit.state_context.revising_paced_capture_source", RevisingPacedCaptureSource, "Revising paced capture source");
void revising_paced_capture_module(iv::GraphBuilder& graph)
{
    (void)graph.node<"iv.test.graph_jit.state_context.revising_paced_capture_source">();
    graph.outputs();
}
IV_MODULE("iv.test.graph_jit.state_context.revising_paced_capture_module", revising_paced_capture_module, "Revising paced capture module");

// author_latent_window() with latency 2 and the history its argument gives.
struct LatentWindowHistorySource {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t checked = 0;
        std::uint64_t mismatches = 0;
    };
    std::size_t history = 3;
    explicit LatentWindowHistorySource(std::size_t history_ = 3) : history(history_) {}
    static constexpr auto inputs() { return std::tuple{}; }
    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>({"out", "Out"}, iv::OutputProduction::sequential)};
    }
    void constrain_ports(iv::ConstrainPortsContext<LatentWindowHistorySource>& ctx) const
    {
        ctx.output<"out">().history() = history;
        ctx.output<"out">().latency() = 2;
    }
    void tick_block(iv::TickBlockContext<LatentWindowHistorySource> const& ctx) const
    {
        author_latent_window(
            ctx.state(), ctx.output<"out">(), ctx.index.whole().signed_value(), ctx.block_size);
    }
};
IV_NODE("iv.test.graph_jit.state_context.latent_window_history_source", LatentWindowHistorySource, "Latent window history source");

// The source read by LatentWindowConsumer (history 4), which reads its ring.
void latent_window_growth_module(iv::GraphBuilder& graph, std::size_t history = 3)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.latent_window_history_source">(history);
    auto consumer = graph.node<"iv.test.graph_jit.state_context.latent_window_consumer">();
    consumer(source);
    graph.outputs();
}
IV_MODULE("iv.test.graph_jit.state_context.latent_window_growth_module", latent_window_growth_module, "Latent window growth module");

// A ramp (value = position) whose input only closes a feedback loop.
struct LoopRampSource {
    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>({"in", "In"}, iv::InputAccess::sequential)};
    }
    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>({"out", "Out"}, iv::OutputProduction::sequential)};
    }
    void tick_block(iv::TickBlockContext<LoopRampSource> const& ctx) const
    {
        auto output = ctx.output<"out">();
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            output.push(iv::Sample{static_cast<float>(
                ctx.index.advanced_by({.whole = i}).whole().signed_value())});
        }
    }
};
IV_NODE("iv.test.graph_jit.state_context.loop_ramp_source", LoopRampSource, "Loop ramp source");

// Compares its two inputs at every position and passes the first through.
struct AlignedPairPassthrough {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t checked = 0;
        std::uint64_t mismatches = 0;
        float largest_difference = 0.0f;
    };
    static constexpr auto inputs()
    {
        return std::array{
            iv::input<iv::Sample>({"ramp", "Ramp"}, iv::InputAccess::sequential),
            iv::input<iv::Sample>({"loop", "Loop"}, iv::InputAccess::sequential),
        };
    }
    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>({"out", "Out"}, iv::OutputProduction::sequential)};
    }
    void tick_block(iv::TickBlockContext<AlignedPairPassthrough> const& ctx) const
    {
        auto& state = ctx.state();
        ++state.calls;
        auto const ramp = ctx.input<"ramp">().block();
        auto const loop = ctx.input<"loop">().block();
        auto output = ctx.output<"out">();
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            auto difference = static_cast<float>(ramp[i] - loop[i]);
            if (difference < 0.0f) difference = -difference;
            ++state.checked;
            if (difference != 0.0f) ++state.mismatches;
            if (difference > state.largest_difference) state.largest_difference = difference;
            output.push(ramp[i]);
        }
    }
};
IV_NODE("iv.test.graph_jit.state_context.aligned_pair_passthrough", AlignedPairPassthrough, "Aligned pair passthrough");

// A loop: the passthrough reads a ramp of latency 7 and the loop ramp, which
// reads the passthrough back through detach(8). A probe after the loop compares
// the passthrough's output with a plain ramp.
void scc_compensation_module(iv::GraphBuilder& graph)
{
    auto latent = graph.node<"iv.test.graph_jit.state_context.latency_argument_ramp_source">(std::size_t{7});
    auto loop = graph.node<"iv.test.graph_jit.state_context.loop_ramp_source">();
    auto passthrough = graph.node<"iv.test.graph_jit.state_context.aligned_pair_passthrough">();
    auto plain = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto probe = graph.node<"iv.test.graph_jit.state_context.latency_compensation_probe">();
    passthrough(latent, loop);
    loop(static_cast<iv::SamplePortRef>(passthrough).detach(8, iv::Sample{0.0f}));
    probe(plain, passthrough);
    graph.outputs();
}
IV_MODULE("iv.test.graph_jit.state_context.scc_compensation_module", scc_compensation_module, "Scc compensation module");

// Persisted Tick outputs of latency 1. Each sample is first written as its
// position and then revised, during the next position, to 1000 + its position.
// Each block adds a trigger at its last position and one in its latency window,
// the position before the block.
struct RevisingCaptureSource {
    static constexpr auto inputs() { return std::tuple{}; }
    static constexpr auto outputs()
    {
        return std::tuple{
            iv::output<iv::Sample>({"samples", "Samples"}, iv::OutputProduction::sequential, iv::OutputRealization::buffered),
            iv::event_output({"events", "Events"}, iv::EventTypeId::trigger, iv::OutputProduction::sequential, iv::OutputRealization::buffered, {.max_events_per_index = 0.25}),
        };
    }
    void constrain_ports(iv::ConstrainPortsContext<RevisingCaptureSource>& ctx) const
    {
        ctx.output<"samples">().latency() = 1;
        ctx.output<"events">().latency() = 1;
    }
    void tick_block(iv::TickBlockContext<RevisingCaptureSource> const& ctx) const
    {
        auto samples = ctx.output<"samples">();
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            auto const position = ctx.index.advanced_by({.whole = i}).whole().signed_value();
            samples.update(iv::Sample{static_cast<float>(999 + position)});
            samples.push(iv::Sample{static_cast<float>(position)});
        }
        if (ctx.block_size == 0) return;
        ctx.output<"events">().push(iv::TimedEvent{
            .time = ctx.index.whole().retreated_by(1),
            .value = iv::TriggerEvent{},
        });
        ctx.output<"events">().push(iv::TriggerEvent{}, ctx.block_size - 1);
    }
};
IV_NODE("iv.test.graph_jit.state_context.revising_capture_source", RevisingCaptureSource, "Revising capture source");
void revising_capture_module(iv::GraphBuilder& graph)
{
    (void)graph.node<"iv.test.graph_jit.state_context.revising_capture_source">();
    graph.outputs();
}
IV_MODULE("iv.test.graph_jit.state_context.revising_capture_module", revising_capture_module, "Revising capture module");
IV_NODE("iv.test.graph_jit.state_context.constant", iv::Constant, "Constant");
// A Constant read by a history reader. `raised` selects its value.
void constant_history_module(iv::GraphBuilder& graph, bool raised = false)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.constant">(
        iv::Sample{raised ? 0.75f : 0.25f});
    auto sink = graph.node<"iv.test.graph_jit.state_context.history_consumer">();
    sink(source);
    graph.outputs();
}
IV_MODULE("iv.test.graph_jit.state_context.constant_history_module", constant_history_module, "Constant history module");

// A pointwise node in a feedback loop: out = in * gain + id. Its gain reads a
// Sequential input without history.
struct FeedbackGain {
    std::uint64_t id = 0;
    std::uint64_t const* retained_pointer = nullptr;

    struct State {
        std::uint64_t ticks = 0;
        std::uint64_t configured_id = 0;
        std::uint64_t retained_pointer_identity_matches = 0;
        float last_input = 0.0f;
        float last_gain = 0.0f;
        float last_output = 0.0f;
    };

    constexpr FeedbackGain(
        std::uint64_t id_, std::uint64_t const* retained_pointer_)
        : id(id_)
        , retained_pointer(retained_pointer_)
    {}

    static constexpr auto inputs()
    {
        return std::array{
            iv::input<iv::Sample>({"in", "In"}, iv::InputAccess::sequential),
            iv::input<iv::Sample>({"gain", "Gain"}, iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>({"out", "Out"}, iv::OutputProduction::sequential)};
    }

    void tick(iv::TickSampleContext<FeedbackGain> const& ctx) const
    {
        auto& state = ctx.state();
        auto const in = static_cast<float>(ctx.input<"in">().get());
        auto const gain = static_cast<float>(ctx.input<"gain">().get());
        auto const out = in * gain + static_cast<float>(id);
        ctx.output<"out">().push(iv::Sample{out});
        ++state.ticks;
        state.configured_id = id;
        state.retained_pointer_identity_matches =
            retained_pointer == pointer_probe_values.data() + id;
        state.last_input = in;
        state.last_gain = gain;
        state.last_output = out;
    }
};
IV_NODE("iv.test.graph_jit.state_context.feedback_gain", FeedbackGain, "Feedback gain");

// head reads tail one sample later through detach(1); both read a Constant
// gain. The two nodes form one cyclic region. A non-zero initial value gives
// the detach its own copied timeline, whose write is a step operation.
void constant_gain_feedback_module(
    iv::GraphBuilder& graph, float gain = 0.5f, float initial = 0.0f)
{
    auto factor = graph.node<"iv.test.graph_jit.state_context.constant">(
        iv::Sample{gain});
    auto head = graph.node<"iv.test.graph_jit.state_context.feedback_gain">(
        std::uint64_t{1}, pointer_probe_values.data() + 1);
    auto tail = graph.node<"iv.test.graph_jit.state_context.feedback_gain">(
        std::uint64_t{2}, pointer_probe_values.data() + 2);
    head.connect_input(
        "in", static_cast<iv::SamplePortRef>(tail).detach(1, iv::Sample{initial}));
    head.connect_input("gain", static_cast<iv::SamplePortRef>(factor));
    tail.connect_input("in", static_cast<iv::SamplePortRef>(head));
    tail.connect_input("gain", static_cast<iv::SamplePortRef>(factor));
    graph.outputs();
}
IV_MODULE(
    "iv.test.graph_jit.state_context.constant_gain_feedback_module",
    constant_gain_feedback_module, "Constant gain feedback module");

// A pointwise node that, like SawOscillator, reads immutable inputs with
// history: out = in * gain[n - 1] + offset[n - 2] + the sum of gain's
// extended block (history plus the current value) + id.
struct ImmutableHistoryReader {
    std::uint64_t id = 0;

    struct State {
        std::uint64_t ticks = 0;
        float last_input = 0.0f;
        float last_output = 0.0f;
    };

    constexpr explicit ImmutableHistoryReader(std::uint64_t id_) : id(id_) {}

    static constexpr auto inputs()
    {
        return std::array{
            iv::input<iv::Sample>({"in", "In"}),
            iv::input<iv::Sample>({"gain", "Gain"}),
            iv::input<iv::Sample>({"offset", "Offset"}, {
                .default_value = iv::Sample{0.125f},
            }),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>({"out", "Out"})};
    }

    void constrain_ports(iv::ConstrainPortsContext<ImmutableHistoryReader>& ctx) const
    {
        ctx.input<"gain">().history() = 1;
        ctx.input<"offset">().history() = 2;
    }

    void tick(iv::TickSampleContext<ImmutableHistoryReader> const& ctx) const
    {
        auto& state = ctx.state();
        auto const in = static_cast<float>(ctx.input<"in">().get());
        auto extended = 0.0f;
        for (auto const value : ctx.input<"gain">().block_extended()) {
            extended += static_cast<float>(value);
        }
        auto const out = in * static_cast<float>(ctx.input<"gain">().get(1))
            + static_cast<float>(ctx.input<"offset">().get(2))
            + extended
            + static_cast<float>(id);
        ctx.output<"out">().push(iv::Sample{out});
        ++state.ticks;
        state.last_input = in;
        state.last_output = out;
    }
};
IV_NODE("iv.test.graph_jit.state_context.immutable_history_reader", ImmutableHistoryReader, "Immutable history reader");

// A ramp read by two ImmutableHistoryReaders of one Constant gain. Neither
// reader is in a cycle; offset stays disconnected.
void immutable_history_module(iv::GraphBuilder& graph, float gain = 0.5f)
{
    auto ramp = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto factor = graph.node<"iv.test.graph_jit.state_context.constant">(
        iv::Sample{gain});
    auto first = graph.node<"iv.test.graph_jit.state_context.immutable_history_reader">(
        std::uint64_t{1});
    auto second = graph.node<"iv.test.graph_jit.state_context.immutable_history_reader">(
        std::uint64_t{2});
    first.connect_input("in", static_cast<iv::SamplePortRef>(ramp));
    first.connect_input("gain", static_cast<iv::SamplePortRef>(factor));
    second.connect_input("in", static_cast<iv::SamplePortRef>(ramp));
    second.connect_input("gain", static_cast<iv::SamplePortRef>(factor));
    graph.outputs();
}
IV_MODULE(
    "iv.test.graph_jit.state_context.immutable_history_module",
    immutable_history_module, "Immutable history module");

// Two equal reader shapes with different immutable gain values. GraphJit can
// share their structure specialization while each invocation still reads its
// own gain ring.
void distinct_immutable_history_module(iv::GraphBuilder& graph)
{
    auto ramp = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto first_gain = graph.node<"iv.test.graph_jit.state_context.constant">(
        iv::Sample{0.5f});
    auto second_gain = graph.node<"iv.test.graph_jit.state_context.constant">(
        iv::Sample{0.75f});
    auto first = graph.node<"iv.test.graph_jit.state_context.immutable_history_reader">(
        std::uint64_t{1});
    auto second = graph.node<"iv.test.graph_jit.state_context.immutable_history_reader">(
        std::uint64_t{2});
    first.connect_input("in", static_cast<iv::SamplePortRef>(ramp));
    first.connect_input("gain", static_cast<iv::SamplePortRef>(first_gain));
    second.connect_input("in", static_cast<iv::SamplePortRef>(ramp));
    second.connect_input("gain", static_cast<iv::SamplePortRef>(second_gain));
    graph.outputs();
}
IV_MODULE(
    "iv.test.graph_jit.state_context.distinct_immutable_history_module",
    distinct_immutable_history_module, "Distinct immutable history module");
// A tiny Tock consumer forces background replay of the curve without
// materializing its entire duration. The point source still retains its one
// complete sparse event Region, including the two points outside this window.
struct AutomationWindow {
    static constexpr auto inputs() {
        return std::array{iv::input<iv::Sample>({"curve", "Curve"}, iv::InputAccess::coverage)};
    }
    static constexpr auto outputs() {
        return std::array{iv::output<iv::Sample>({"window", "Window"}, iv::OutputProduction::region,
            iv::OutputRealization::buffered)};
    }
    void propagate_forward_coverage(iv::PropagateForwardCoverageContext<AutomationWindow>& ctx) const {
        auto const window = iv::GlobalIndexRange{iv::GlobalIndex::from_bits(2), 2};
        ctx.output<"window">().publish(window);
        if (ctx.local_state_changed || !ctx.input<"curve">().changed().empty())
            ctx.output<"window">().change(iv::IndexCoverage{window});
    }
    void propagate_reverse_coverage(iv::PropagateReverseCoverageContext<AutomationWindow>& ctx) const {
        ctx.input<"curve">().require(ctx.output<"window">().required_positions());
    }
    void tock_coverage(iv::TockCoverageContext<AutomationWindow>& ctx) const {
        auto inputs = ctx.input<"curve">().regions();
        for (auto run : ctx.output<"window">().prepared_runs()) {
            for (std::size_t slot = 0; slot < run.size(); ++slot) {
                auto const index = run.position(slot);
                iv::Sample value{0};
                for (auto input : inputs) if (input.range().contains(index)) value.value += input.at(index).value;
                run.value(slot) = value;
            }
        }
    }
};
IV_NODE("iv.test.automation.window", AutomationWindow, "Window");
struct AutomationBoundSignal {
    iv::Sample value;
    explicit AutomationBoundSignal(iv::Sample value_) : value(value_) {}
    static constexpr auto outputs() {
        return std::array{iv::output<iv::Sample>({"bound", "Bound"}, iv::OutputProduction::coverage,
            iv::OutputRealization::computed, {.channel_layout = iv::mono_planar_channel_layout})};
    }
    void propagate_forward_coverage(iv::PropagateForwardCoverageContext<AutomationBoundSignal>& ctx) const {
        auto output = ctx.output<"bound">();
        auto const range = iv::GlobalIndexRange{iv::GlobalIndex::from_bits(0), 5};
        output.publish(std::array{range});
        if (ctx.local_state_changed) output.change(range);
    }
    void tock_coverage(iv::TockCoverageContext<AutomationBoundSignal>& ctx) const {
        for (auto run : ctx.output<"bound">().prepared_runs())
            for (std::size_t slot = 0; slot < run.size(); ++slot) run.value(slot) = value;
    }
};
IV_NODE("iv.test.automation.bound_signal", AutomationBoundSignal, "Bound signal");
void automation_window_module(iv::GraphBuilder& graph) {
    using namespace iv;
    // Ten hours of sparse coverage, but only three event records. None lies
    // inside the two-sample replay window. The list is deliberately unsorted:
    // the curve sorts it before PointSource, which rejects unsorted points.
    auto curve = graph.node<"builtin.automation.curve">(
        std::vector<iv::automation::Point>{{3, 36000, 0, iv::CurveInterpolation::bezier, 0},
            {1, 0, 0, iv::CurveInterpolation::bezier, 0}, {2, 4.0 / 88200, 1, iv::CurveInterpolation::bezier, 0}});
    auto min = graph.node<"iv.test.automation.bound_signal">(iv::Sample{.25f});
    auto max = graph.node<"iv.test.automation.bound_signal">(iv::Sample{.6f});
    curve("min"_P = min, "max"_P = max);
    auto window = graph.node<"iv.test.automation.window">();
    window(curve);
    auto live = graph.node<"iv.test.graph_jit.state_context.manual_control_slice_probe">();
    live(curve);
    graph.outputs();
}
IV_MODULE("iv.test.automation.window_module", automation_window_module, "Window module");

void automation_wrap_bound(iv::GraphBuilder& graph) {
    using namespace iv;
    using automation::Point;
    using automation::SampleIndex;
    auto bound = graph.node<"builtin.automation.curve">(std::vector<Point>{
        {.id = 1, .value = .5f, .interpolation = CurveInterpolation::hold, .sample_index = SampleIndex{"9223372036854775805"}},
        {.id = 2, .interpolation = CurveInterpolation::empty, .sample_index = SampleIndex{"-9223372036854775805"}},
    });
    auto curve = graph.node<"builtin.automation.curve">(std::vector<Point>{
        {.id = 1, .value = 1, .interpolation = CurveInterpolation::hold, .sample_index = SampleIndex{"9223372036854775803"}},
        {.id = 2, .interpolation = CurveInterpolation::empty, .sample_index = SampleIndex{"-9223372036854775803"}},
    });
    curve("max"_P = bound);
    graph.outputs();
}
IV_MODULE("iv.test.automation.wrap_bound", automation_wrap_bound, "Wrap bound");

// Replayable pointwise Tick nodes whose only input is Sequential. Both compute
// a value at every position; an input position without upstream coverage
// resolves to the input's neutral value. AddOne follows its input coverage.
struct ReplayAddOne {
    static constexpr iv::TickReplayMode replay_mode = iv::TickReplayMode::enabled;
    static constexpr auto inputs() { return std::array{iv::input<iv::Sample>({"in", "In"})}; }
    static constexpr auto outputs() { return std::array{iv::output<iv::Sample>({"out", "Out"})}; }
    void propagate_forward_coverage(iv::PropagateForwardCoverageContext<ReplayAddOne>& ctx) const {
        ctx.output<"out">().publish(ctx.input<"in">().coverage());
    }
    void tick_block(iv::TickBlockContext<ReplayAddOne> const& ctx) const {
        auto input = ctx.input<"in">();
        auto output = ctx.output<"out">().block();
        for (std::size_t frame = 0; frame < output.size(); ++frame)
            output[frame] = iv::Sample{static_cast<float>(input.get_value(frame)) + 1.0f};
    }
};
IV_NODE("iv.test.replay.add_one", ReplayAddOne, "Add one");
struct ReplayTenMinus {
    static constexpr iv::TickReplayMode replay_mode = iv::TickReplayMode::enabled;
    static constexpr auto inputs() { return std::array{iv::input<iv::Sample>({"in", "In"})}; }
    static constexpr auto outputs() { return std::array{iv::output<iv::Sample>({"out", "Out"})}; }
    void tick_block(iv::TickBlockContext<ReplayTenMinus> const& ctx) const {
        auto input = ctx.input<"in">();
        auto output = ctx.output<"out">().block();
        for (std::size_t frame = 0; frame < output.size(); ++frame)
            output[frame] = iv::Sample{10.0f - static_cast<float>(input.get_value(frame))};
    }
};
IV_NODE("iv.test.replay.ten_minus", ReplayTenMinus, "Ten minus");

struct ReplayLatentSample {
    static constexpr iv::TickReplayMode replay_mode = iv::TickReplayMode::enabled;
    static constexpr auto inputs() { return std::array{iv::input<iv::Sample>({"in", "In"})}; }
    static constexpr auto outputs() { return std::array{iv::output<iv::Sample>({"latent", "Latent"})}; }
    void constrain_ports(iv::ConstrainPortsContext<ReplayLatentSample>& ctx) const {
        ctx.output<"latent">().latency() = 2;
    }
    void tick_block(iv::TickBlockContext<ReplayLatentSample> const& ctx) const {
        auto input = ctx.input<"in">();
        auto output = ctx.output<"latent">();
        for (std::size_t frame = 0; frame < ctx.block_size; ++frame) {
            output.update(input.get_value(frame), 1);
            output.push(iv::Sample{-1});
        }
    }
};
IV_NODE("iv.test.replay.latent_sample", ReplayLatentSample, "Latent sample");

// A persisted Tock window that demands one configured range of its input,
// forcing background replay of the Tick node that feeds it.
struct ReplayWindow {
    std::int64_t begin = 0;
    std::uint64_t extent = 0;
    constexpr ReplayWindow(std::int64_t begin_, std::uint64_t extent_)
        : begin(begin_), extent(extent_) {}
    static constexpr auto inputs() {
        return std::array{iv::input<iv::Sample>({"in", "In"}, iv::InputAccess::coverage)};
    }
    static constexpr auto outputs() {
        return std::array{iv::output<iv::Sample>({"window", "Window"}, iv::OutputProduction::region,
            iv::OutputRealization::buffered)};
    }
    [[nodiscard]] iv::GlobalIndexRange window() const {
        return {iv::GlobalIndex::from_signed(begin), extent};
    }
    void propagate_forward_coverage(iv::PropagateForwardCoverageContext<ReplayWindow>& ctx) const {
        ctx.output<"window">().publish(window());
        if (ctx.local_state_changed || !ctx.input<"in">().changed().empty())
            ctx.output<"window">().change(iv::IndexCoverage{window()});
    }
    void propagate_reverse_coverage(iv::PropagateReverseCoverageContext<ReplayWindow>& ctx) const {
        ctx.input<"in">().require(ctx.output<"window">().required_positions());
    }
    void tock_coverage(iv::TockCoverageContext<ReplayWindow>& ctx) const {
        auto inputs = ctx.input<"in">().regions();
        for (auto run : ctx.output<"window">().prepared_runs()) {
            for (std::size_t slot = 0; slot < run.size(); ++slot) {
                auto const index = run.position(slot);
                iv::Sample value{0};
                for (auto input : inputs) if (input.range().contains(index)) value.value += input.at(index).value;
                run.value(slot) = value;
            }
        }
    }
};
IV_NODE("iv.test.replay.window", ReplayWindow, "Window");

void replay_latent_sample_module(iv::GraphBuilder& graph) {
    auto source = graph.node<"iv.test.graph_jit.state_context.single_region_sample_source">();
    auto latent = graph.node<"iv.test.replay.latent_sample">();
    auto window = graph.node<"iv.test.replay.window">(std::int64_t{18}, std::uint64_t{4});
    latent(source);
    window(latent);
    graph.outputs();
}
IV_MODULE("iv.test.replay.latent_sample_module", replay_latent_sample_module, "Latent sample replay module");

void coverage_manual_window_module(iv::GraphBuilder& graph) {
    auto source = graph.node<"iv.test.graph_jit.state_context.single_region_sample_source">();
    auto module = graph.subgraph([&](iv::SubgraphBuilder& boundary) {
        auto coverage = boundary.input<iv::Sample>({"Coverage"}, iv::InputAccess::coverage);
        auto window = graph.node<"iv.test.replay.window">(std::int64_t{18}, std::uint64_t{8});
        window(coverage);
        boundary.outputs(window);
    }, "coverage-manual-window");
    module(source);
    graph.outputs();
}
IV_MODULE("iv.test.replay.coverage_manual_window", coverage_manual_window_module, "Coverage manual window");

struct StereoBoundaryCoverageSource {
    static constexpr auto outputs() {
        return std::array{iv::output<iv::Sample>({"stereo", "Stereo"}, iv::OutputProduction::region,
            iv::OutputRealization::computed, {.channel_layout = {.channel_type = iv::ChannelTypeId::stereo,
                .sample_layout = iv::SampleStreamLayout::planar}})};
    }
    void propagate_forward_coverage(iv::PropagateForwardCoverageContext<StereoBoundaryCoverageSource>& ctx) const {
        auto output = ctx.output<"stereo">();
        output.publish(SingleRegionSampleSource::region());
        output.change(SingleRegionSampleSource::region());
    }
    void tock_coverage(iv::TockCoverageContext<StereoBoundaryCoverageSource>& ctx) const {
        for (auto run : ctx.output<"stereo">().prepared_runs()) {
            for (std::size_t slot = 0; slot < run.size(); ++slot) {
                auto const sample = static_cast<float>(run.index(slot));
                run.value(slot, 0) = iv::Sample{4 + sample};
                run.value(slot, 1) = iv::Sample{104 + sample};
            }
        }
    }
};
IV_NODE("iv.test.replay.stereo_boundary_source", StereoBoundaryCoverageSource, "Stereo boundary source");

void stereo_coverage_boundary_module(iv::GraphBuilder& graph) {
    auto source = graph.node<"iv.test.replay.stereo_boundary_source">();
    auto module = graph.subgraph([&](iv::SubgraphBuilder& boundary) {
        auto coverage = boundary.input<iv::stereo>({"Coverage"}, iv::InputAccess::coverage);
        auto window = graph.node<"iv.test.replay.window">(std::int64_t{18}, std::uint64_t{8});
        window(coverage);
        boundary.outputs(window);
    }, "stereo-coverage-window");
    module(source);
    graph.outputs();
}
IV_MODULE("iv.test.replay.stereo_coverage_boundary", stereo_coverage_boundary_module, "Stereo coverage boundary");

void unused_coverage_input_module(iv::GraphBuilder& graph) {
    auto source = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto module = graph.subgraph([](iv::SubgraphBuilder& boundary) {
        [[maybe_unused]] auto coverage = boundary.input<iv::Sample>({"Coverage"}, iv::InputAccess::coverage);
        boundary.outputs();
    }, "unused-coverage-input");
    module(source);
    graph.outputs();
}
IV_MODULE("iv.test.replay.unused_coverage_input", unused_coverage_input_module, "Unused coverage input");

void replay_sequential_resolution_module(iv::GraphBuilder& graph) {
    // The source covers only [20, 24), with values 4, 5, 6 and 7.
    auto source = graph.node<"iv.test.graph_jit.state_context.single_region_sample_source">();
    auto add = graph.node<"iv.test.replay.add_one">();
    auto flip = graph.node<"iv.test.replay.ten_minus">();
    add(source);
    flip(source);
    // Input-union replay preserves source gaps instead of publishing fallback values.
    auto added = graph.node<"iv.test.replay.window">(std::int64_t{18}, std::uint64_t{8});
    added(add);
    // full: [18, 26) straddles the source Region; [100, 104) lies outside it.
    auto spanning = graph.node<"iv.test.replay.window">(std::int64_t{18}, std::uint64_t{8});
    spanning(flip);
    auto uncovered = graph.node<"iv.test.replay.window">(std::int64_t{100}, std::uint64_t{4});
    uncovered(flip);
    // A full-mode replayable generator without inputs writes 11 + position.
    auto generator = graph.node<"iv.test.graph_jit.state_context.native_batch_replay">(
        std::uint64_t{11});
    auto generated = graph.node<"iv.test.replay.window">(std::int64_t{40}, std::uint64_t{3});
    generated(generator);
    graph.outputs();
}
IV_MODULE("iv.test.replay.sequential_resolution_module", replay_sequential_resolution_module, "Sequential resolution module");

template<bool ModuleChild>
struct ReplayManualFallback {
    static constexpr iv::TickReplayMode replay_mode = iv::TickReplayMode::enabled;
    // A nonzero neutral value must remain the combiner's identity, as the
    // Sequential fan-in contract requires: combine(neutral, x) == x.
    static constexpr iv::Sample shifted_add(iv::Sample left, iv::Sample right) noexcept {
        return iv::Sample{left.value + right.value - 0.125f};
    }
    static constexpr auto inputs() {
        return std::array{iv::input<iv::Sample>({"gain", "Gain"}, {
            .default_value = 0.25f, .neutral_value = 0.125f,
            .combine_values = &shifted_add})};
    }
    static constexpr auto outputs() {
        auto const names = ModuleChild
            ? iv::PortNames{"manual_union", "Manual union"}
            : iv::PortNames{"manual_full", "Manual full"};
        return std::array{iv::output<iv::Sample>(names)};
    }
    void propagate_forward_coverage(iv::PropagateForwardCoverageContext<ReplayManualFallback>& ctx) const
        requires (ModuleChild)
    {
        ctx.template output<"manual_union">().publish(ctx.template input<"gain">().coverage());
    }
    void tick_block(iv::TickBlockContext<ReplayManualFallback> const& ctx) const {
        auto input = ctx.template input<"gain">();
        auto output = [&] {
            if constexpr (!ModuleChild) return ctx.template output<"manual_full">().block();
            else return ctx.template output<"manual_union">().block();
        }();
        for (std::size_t frame = 0; frame < output.size(); ++frame)
            output[frame] = iv::Sample{static_cast<float>(input.get_value(frame)) + 1.0f};
    }
};
using ReplayManualFull = ReplayManualFallback<false>;
using ReplayManualUnion = ReplayManualFallback<true>;
IV_NODE("iv.test.replay.manual_full", ReplayManualFull, "Manual full");
IV_NODE("iv.test.replay.manual_union", ReplayManualUnion, "Manual union");
struct ReplayLiveProbe {
    struct State { std::uint64_t calls = 0; std::array<float, 8> values{}; };
    static constexpr auto inputs() { return std::array{iv::input<iv::Sample>({"value", "Value"})}; }
    static constexpr auto outputs() { return std::tuple{}; }
    void tick_block(iv::TickBlockContext<ReplayLiveProbe> const& ctx) const {
        auto& state = ctx.state();
        ++state.calls;
        auto input = ctx.input<"value">();
        for (std::size_t frame = 0; frame < std::min(state.values.size(), ctx.block_size); ++frame)
            state.values[frame] = static_cast<float>(input.get_value(frame));
    }
};
IV_NODE("iv.test.replay.live_probe", ReplayLiveProbe, "Live probe");
void live_union_fallback_module(iv::GraphBuilder& graph) {
    auto source = graph.node<"iv.test.graph_jit.state_context.single_region_sample_source">();
    auto sparse = graph.node<"iv.test.replay.add_one">();
    auto resolved = graph.node<"iv.test.replay.manual_full">();
    auto probe = graph.node<"iv.test.replay.live_probe">();
    sparse(source);
    resolved(sparse);
    probe(resolved);
    graph.outputs();
}
IV_MODULE("iv.test.replay.live_union_fallback", live_union_fallback_module, "Live union fallback");

struct FallbackWindowSource {
    std::int64_t begin;
    std::uint64_t length;
    iv::Sample value;

    constexpr FallbackWindowSource(std::int64_t begin_, std::uint64_t length_, iv::Sample value_)
        : begin(begin_), length(length_), value(value_) {}

    static constexpr auto outputs() {
        return std::array{iv::output<iv::Sample>({"out", "Out"}, iv::OutputProduction::region)};
    }

    void propagate_forward_coverage(iv::PropagateForwardCoverageContext<FallbackWindowSource>& ctx) const {
        auto out = ctx.output<"out">();
        auto const window = iv::GlobalIndexRange{iv::GlobalIndex::from_signed(begin), length};
        out.publish(window);
        out.change(window);
    }

    void tock_coverage(iv::TockCoverageContext<FallbackWindowSource>& ctx) const {
        for (auto run : ctx.output<"out">().prepared_runs())
            for (std::size_t slot = 0; slot < run.size(); ++slot)
                run.value(slot) = value;
    }
};
IV_NODE("iv.test.replay.fallback_window_source", FallbackWindowSource, "Fallback source");

void sample_fallback_operator_module(iv::GraphBuilder& graph) {
    auto primary = graph.node<"iv.test.replay.fallback_window_source">(
        std::int64_t{20}, std::uint64_t{3}, iv::Sample{0.0f});
    auto secondary = graph.node<"iv.test.replay.fallback_window_source">(
        std::int64_t{21}, std::uint64_t{4}, iv::Sample{2.0f});
    auto constant = graph.node<"iv.test.graph_jit.state_context.constant">(iv::Sample{0.5f});
    auto const selected = primary | (primary + secondary) | constant;
    auto probe = graph.node<"iv.test.replay.live_probe">();
    auto window = graph.node<"iv.test.replay.window">(std::int64_t{18}, std::uint64_t{8});
    probe(iv::named<"value"> = selected);
    window(selected);
    graph.outputs();
}
IV_MODULE("iv.test.replay.sample_fallback_operator", sample_fallback_operator_module, "Sample fallback operator");

struct FiniteTail {
    struct State { float previous = 0; float older = 0; };
    static constexpr auto inputs() { return std::array{iv::input<iv::Sample>({"signal", "Signal"})}; }
    static constexpr auto outputs() { return std::array{iv::output<iv::Sample>({"filtered", "Filtered"})}; }

    void propagate_forward_coverage(iv::PropagateForwardCoverageContext<FiniteTail>& ctx) const
    {
        ctx.output<"filtered">().publish(ctx.input<"signal">().coverage().extend_right(2));
    }

    void tick_block(iv::TickBlockContext<FiniteTail> const& ctx) const
    {
        auto& state = ctx.state();
        auto const input = ctx.input<"signal">();
        auto const output = ctx.output<"filtered">().block();
        for (std::size_t frame = 0; frame < output.size(); ++frame) {
            auto const current = input.get_value(frame).value;
            output[frame] = iv::Sample{current + state.previous + state.older};
            state.older = state.previous;
            state.previous = current;
        }
    }
};
IV_NODE("iv.test.tick.finite_tail", FiniteTail, "Finite tail");

void finite_tail_fallback_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.single_region_sample_source">();
    auto sparse = graph.node<"iv.test.replay.add_one">();
    auto filtered = graph.node<"iv.test.tick.finite_tail">();
    auto fallback = graph.node<"iv.test.graph_jit.state_context.constant">(iv::Sample{0.5f});
    auto probe = graph.node<"iv.test.replay.live_probe">();
    sparse(source);
    filtered(sparse);
    probe(iv::named<"value"> = (filtered | fallback));
    graph.outputs();
}
IV_MODULE("iv.test.tick.finite_tail_fallback", finite_tail_fallback_module, "Finite tail fallback");

struct ReplayEventCoverageValue {
    static constexpr auto replay_mode = iv::TickReplayMode::enabled;
    static constexpr auto inputs() {
        return std::array{iv::event_input({"events", "Events"}, iv::EventTypeId::trigger, iv::InputAccess::sequential)};
    }
    static constexpr auto outputs() { return std::array{iv::output<iv::Sample>({"out", "Out"})}; }
    void propagate_forward_coverage(iv::PropagateForwardCoverageContext<ReplayEventCoverageValue>& ctx) const {
        ctx.output<"out">().publish(ctx.input<"events">().coverage());
    }
    void tick_block(iv::TickBlockContext<ReplayEventCoverageValue> const& ctx) const {
        auto output = ctx.output<"out">().block();
        for (std::size_t frame = 0; frame < output.size(); ++frame)
            output[frame] = iv::Sample{3.f};
    }
};
IV_NODE("iv.test.replay.event_coverage_value", ReplayEventCoverageValue, "Event coverage value");

void live_event_union_fallback_module(iv::GraphBuilder& graph) {
    auto source = graph.node<"iv.test.graph_jit.state_context.single_region_event_source">();
    auto sparse = graph.node<"iv.test.replay.event_coverage_value">();
    auto resolved = graph.node<"iv.test.replay.manual_full">();
    auto probe = graph.node<"iv.test.replay.live_probe">();
    sparse.connect_event_input(0, source.event_port());
    resolved(sparse);
    probe(resolved);
    graph.outputs();
}
IV_MODULE("iv.test.replay.live_event_union_fallback", live_event_union_fallback_module, "Live event union fallback");

struct DelayedSparseSampleProbe {
    struct State { std::uint64_t calls = 0; std::array<float, 2> values{}; };
    static constexpr auto inputs() {
        return std::array{iv::input<iv::Sample>({"value", "Value"}, {
            .default_value = 0.25f}),
            iv::input<iv::Sample>({"alignment", "Alignment"})};
    }
    void tick_block(iv::TickBlockContext<DelayedSparseSampleProbe> const& ctx) const {
        auto input = ctx.input<"value">();
        auto& state = ctx.state();
        ++state.calls;
        for (std::size_t frame = 0; frame < std::min(state.values.size(), ctx.block_size); ++frame)
            state.values[frame] = static_cast<float>(input.get_value(frame));
    }
};
IV_NODE("iv.test.replay.delayed_sparse_probe", DelayedSparseSampleProbe, "Delayed sparse probe");

void delayed_sparse_coverage_module(iv::GraphBuilder& graph) {
    auto source = graph.node<"iv.test.graph_jit.state_context.single_region_sample_source">();
    auto sparse = graph.node<"iv.test.replay.add_one">();
    auto pass = graph.node<"iv.test.replay.add_one">();
    auto alignment = graph.node<"iv.test.graph_jit.state_context.latency_argument_ramp_source">(std::size_t{2});
    auto probe = graph.node<"iv.test.replay.delayed_sparse_probe">();
    sparse(source);
    pass(sparse);
    probe(pass, alignment);
    graph.outputs();
}
IV_MODULE("iv.test.replay.delayed_sparse_coverage", delayed_sparse_coverage_module, "Delayed sparse coverage");

void sparse_input_with_feedback_module(iv::GraphBuilder& graph) {
    auto source = graph.node<"iv.test.graph_jit.state_context.single_region_sample_source">();
    auto sparse = graph.node<"iv.test.replay.add_one">();
    auto counter = graph.node<"iv.test.graph_jit.state_context.neutral_feedback_counter">();
    sparse(source);
    counter(sparse);
    counter(static_cast<iv::SamplePortRef>(counter).detach(1, iv::Sample{100.0f}));
    graph.outputs();
}
IV_MODULE("iv.test.replay.sparse_input_with_feedback", sparse_input_with_feedback_module, "Sparse input with feedback");

void replay_module_manual_resolution(iv::GraphBuilder& graph) {
    auto source = graph.node<"iv.test.graph_jit.state_context.single_region_sample_source">();
    auto direct = graph.node<"iv.test.replay.manual_full">();
    direct(source);
    auto module = graph.subgraph([&](iv::SubgraphBuilder& boundary) {
        auto boundary_gain = boundary.input<iv::Sample>({"Boundary gain"}, iv::InputAccess::sequential, {
            .default_value = 0.5f});
        auto child = graph.node<"iv.test.replay.manual_union">();
        child(boundary_gain);
        boundary.outputs(child);
    }, "manual-resolution");
    module(source);
    auto direct_window = graph.node<"iv.test.replay.window">(std::int64_t{18}, std::uint64_t{8});
    auto module_window = graph.node<"iv.test.replay.window">(std::int64_t{18}, std::uint64_t{8});
    direct_window(direct);
    module_window(module);
    auto live_direct = graph.node<"iv.test.replay.live_probe">();
    auto live_module = graph.node<"iv.test.replay.live_probe">();
    live_direct(direct);
    live_module(module);
    graph.outputs();
}
IV_MODULE("iv.test.replay.module_manual_resolution", replay_module_manual_resolution, "Module manual resolution");
