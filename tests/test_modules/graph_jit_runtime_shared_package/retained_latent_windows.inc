struct LargeLatentWindowSource {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t checked = 0;
        std::uint64_t mismatches = 0;
    };

    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::sequential)};
    }

    void constrain_ports(
        iv::ConstrainPortsContext<LargeLatentWindowSource>& ctx) const
    {
        ctx.output<"out">().history() = 4000;
        ctx.output<"out">().latency() = 1000;
    }

    void tick_block(iv::TickBlockContext<LargeLatentWindowSource> const& ctx) const
    {
        author_latent_window(
            ctx.state(),
            ctx.output<"out">(),
            ctx.index.whole().signed_value(),
            ctx.block_size);
    }
};

// Reads a latent source through its latency-compensated input. Every value it
// observes, in its history and in its current block, must already be final.
struct LatentWindowConsumer {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t checked = 0;
        std::uint64_t mismatches = 0;
        std::int64_t first_position = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>("in", iv::InputAccess::sequential)};
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void constrain_ports(
        iv::ConstrainPortsContext<LatentWindowConsumer>& ctx) const
    {
        ctx.input<"in">().history() = 4;
    }

    void tick_block(iv::TickBlockContext<LatentWindowConsumer> const& ctx) const
    {
        auto& state = ctx.state();
        auto input = ctx.input<"in">();
        auto const first = ctx.index.whole().signed_value()
            - static_cast<std::int64_t>(input.latency());
        ++state.calls;
        state.checked = 0;
        state.mismatches = 0;
        state.first_position = first;
        auto const check = [&](std::int64_t position, float observed) {
            if (position < 0) return;
            ++state.checked;
            if (observed != static_cast<float>(position)) ++state.mismatches;
        };
        for (std::size_t history = 1; history <= input.history(); ++history) {
            check(
                first - static_cast<std::int64_t>(history),
                static_cast<float>(input.get(history)));
        }
        for (std::size_t offset = 0; offset < ctx.block_size; ++offset) {
            check(
                first + static_cast<std::int64_t>(offset),
                static_cast<float>(input.get_value(offset)));
        }
    }
};

void latent_window_compact_carry_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.latent_window_source">();
    (void)source;
    graph.outputs();
}

void latent_window_ring_module(iv::GraphBuilder& graph)
{
    auto source =
        graph.node<"iv.test.graph_jit.state_context.large_latent_window_source">();
    (void)source;
    graph.outputs();
}

void latent_window_consumer_compact_carry_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.latent_window_source">();
    auto consumer =
        graph.node<"iv.test.graph_jit.state_context.latent_window_consumer">();
    consumer(source);
    graph.outputs();
}

void latent_window_consumer_ring_module(iv::GraphBuilder& graph)
{
    auto source =
        graph.node<"iv.test.graph_jit.state_context.large_latent_window_source">();
    auto consumer =
        graph.node<"iv.test.graph_jit.state_context.latent_window_consumer">();
    consumer(source);
    graph.outputs();
}

// Checks every value a Sequential input reads from sample_ramp_source, whose
// value at each position is that position. The input's first current value is
// at position index - latency(); its history precedes that position. checked
// and mismatches count this block's reads only.
template<class State, class Input>
void check_ramp_reads(
    State& state,
    Input const& input,
    std::int64_t index,
    std::size_t block_size)
{
    auto const first = index - static_cast<std::int64_t>(input.latency());
    ++state.calls;
    state.first_position = first;
    state.read_latency = input.latency();
    state.checked = 0;
    state.mismatches = 0;
    auto const check = [&](std::int64_t position, float observed) {
        if (position < 0) return;
        ++state.checked;
        if (observed != static_cast<float>(position)) ++state.mismatches;
    };
    for (std::size_t history = 1; history <= input.history(); ++history) {
        check(
            first - static_cast<std::int64_t>(history),
            static_cast<float>(input.get(history)));
    }
    for (std::size_t offset = 0; offset < block_size; ++offset) {
        check(
            first + static_cast<std::int64_t>(offset),
            static_cast<float>(input.get_value(offset)));
    }
}

// Reads a ramp beside a slower input, so the compiler delays the ramp input to
// align the two. The slower input only sets that delay; the probe checks the
// ramp input alone.
struct CompensatedRampProbe {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t checked = 0;
        std::uint64_t mismatches = 0;
        std::int64_t first_position = 0;
        std::uint64_t read_latency = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{
            iv::input<iv::Sample>("ramp", iv::InputAccess::sequential),
            iv::input<iv::Sample>("slower", iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(iv::TickBlockContext<CompensatedRampProbe> const& ctx) const
    {
        check_ramp_reads(
            ctx.state(),
            ctx.input<"ramp">(),
            ctx.index.whole().signed_value(),
            ctx.block_size);
    }
};

// CompensatedRampProbe whose ramp input also keeps history.
struct CompensatedRampHistoryProbe {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t checked = 0;
        std::uint64_t mismatches = 0;
        std::int64_t first_position = 0;
        std::uint64_t read_latency = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{
            iv::input<iv::Sample>("ramp", iv::InputAccess::sequential),
            iv::input<iv::Sample>("slower", iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void constrain_ports(
        iv::ConstrainPortsContext<CompensatedRampHistoryProbe>& ctx) const
    {
        ctx.input<"ramp">().history() = 4;
    }

    void tick_block(
        iv::TickBlockContext<CompensatedRampHistoryProbe> const& ctx) const
    {
        check_ramp_reads(
            ctx.state(),
            ctx.input<"ramp">(),
            ctx.index.whole().signed_value(),
            ctx.block_size);
    }
};

// The probe's ramp input reads the source, compensated for the delay node's
// latency. The second reader's fast input reads the delay node; a successor
// moves it to the source.
void compensated_ramp_second_reader_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto delayed = graph.node<"iv.test.graph_jit.state_context.five_sample_delay">();
    auto twice_delayed =
        graph.node<"iv.test.graph_jit.state_context.five_sample_delay">();
    auto probe =
        graph.node<"iv.test.graph_jit.state_context.compensated_ramp_probe">();
    auto second_reader =
        graph.node<"iv.test.graph_jit.state_context.latency_compensation_probe">();
    delayed(source);
    twice_delayed(delayed);
    probe(source, delayed);
    second_reader(delayed, twice_delayed);
    graph.outputs();
}

// The probe's ramp input reads the source, compensated for the delay node's
// latency. The history reader reads the delay node; a successor moves it to
// the source.
void compensated_ramp_history_reader_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto delayed = graph.node<"iv.test.graph_jit.state_context.five_sample_delay">();
    auto probe =
        graph.node<"iv.test.graph_jit.state_context.compensated_ramp_probe">();
    auto history_reader =
        graph.node<"iv.test.graph_jit.state_context.large_history_consumer">();
    delayed(source);
    probe(source, delayed);
    history_reader(delayed);
    graph.outputs();
}

// Continues its own State in the next graph generation through move(), which
// counts the moves.
struct MovingStateProbe {
    struct State {
        std::uint64_t ticks = 0;
        std::uint64_t moves = 0;
        std::uint64_t marker = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>("in", iv::InputAccess::sequential)};
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void move(iv::MoveContext<MovingStateProbe> const& ctx) const
    {
        auto const& previous = ctx.previous_state();
        auto& state = ctx.state();
        state.ticks = previous.ticks;
        state.moves = previous.moves + 1;
        state.marker = previous.marker;
    }

    void tick_block(iv::TickBlockContext<MovingStateProbe> const& ctx) const
    {
        ++ctx.state().ticks;
    }
};

void moving_state_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto first = graph.node<"iv.test.graph_jit.state_context.moving_state_probe">();
    auto second = graph.node<"iv.test.graph_jit.state_context.moving_state_probe">();
    first(source);
    second(source);
    graph.outputs();
}

// sample_ramp_source shifted by 100000, so that values from the two sources of
// a rewired input can be told apart.
struct OffsetSampleRampSource {
    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::sequential)};
    }

    void tick_block(iv::TickBlockContext<OffsetSampleRampSource> const& ctx) const
    {
        auto output = ctx.output<"out">();
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            output.push(iv::Sample{static_cast<float>(
                ctx.index.advanced_by({.whole = i}).whole().signed_value()
                + 100000)});
        }
    }
};

// Records every value its ramp input reads in each block of at most 64 values.
// The slower input only sets the ramp input's compensation.
struct CompensatedSequenceProbe {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t read_latency = 0;
        std::int64_t first_index = 0;
        std::uint64_t recorded = 0;
        std::array<float, 64> values{};
    };

    static constexpr auto inputs()
    {
        return std::array{
            iv::input<iv::Sample>("ramp", iv::InputAccess::sequential),
            iv::input<iv::Sample>("slower", iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(iv::TickBlockContext<CompensatedSequenceProbe> const& ctx) const
    {
        auto& state = ctx.state();
        auto ramp = ctx.input<"ramp">();
        ++state.calls;
        state.read_latency = ramp.latency();
        state.first_index = ctx.index.whole().signed_value();
        state.recorded = std::min<std::size_t>(ctx.block_size, state.values.size());
        for (std::size_t offset = 0; offset < state.recorded; ++offset) {
            state.values[offset] = static_cast<float>(ramp.get_value(offset));
        }
    }
};

// The probe's ramp input reads the source, compensated for the delay node's
// latency. twice_delayed gives a successor a slower path to move the probe's
// slower input to.
void compensated_sequence_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto delayed = graph.node<"iv.test.graph_jit.state_context.five_sample_delay">();
    auto twice_delayed =
        graph.node<"iv.test.graph_jit.state_context.five_sample_delay">();
    auto probe = graph.node<
        "iv.test.graph_jit.state_context.compensated_sequence_probe">();
    delayed(source);
    twice_delayed(delayed);
    probe(source, delayed);
    graph.outputs();
}

// The probe's ramp input reads the first source; a successor rewires it to the
// offset source, which has the same latency.
void compensated_sequence_rewire_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto offset_source =
        graph.node<"iv.test.graph_jit.state_context.offset_sample_ramp_source">();
    auto delayed = graph.node<"iv.test.graph_jit.state_context.five_sample_delay">();
    auto offset_delayed =
        graph.node<"iv.test.graph_jit.state_context.five_sample_delay">();
    auto probe = graph.node<
        "iv.test.graph_jit.state_context.compensated_sequence_probe">();
    delayed(source);
    offset_delayed(offset_source);
    probe(source, delayed);
    graph.outputs();
}

// Records every value its input reads in each block of at most 64 values. Its
// State matches CompensatedSequenceProbe's.
struct SampleSequenceProbe {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t read_latency = 0;
        std::int64_t first_index = 0;
        std::uint64_t recorded = 0;
        std::array<float, 64> values{};
    };

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>("in", iv::InputAccess::sequential)};
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(iv::TickBlockContext<SampleSequenceProbe> const& ctx) const
    {
        auto& state = ctx.state();
        auto input = ctx.input<"in">();
        ++state.calls;
        state.read_latency = input.latency();
        state.first_index = ctx.index.whole().signed_value();
        state.recorded = std::min<std::size_t>(ctx.block_size, state.values.size());
        for (std::size_t offset = 0; offset < state.recorded; ++offset) {
            state.values[offset] = static_cast<float>(input.get_value(offset));
        }
    }
};

// Counts the values at which a stereo input's two channels differ.
template<class State, class Input>
void count_channel_mismatches(
    State& state,
    Input const& input,
    std::size_t block_size)
{
    ++state.calls;
    for (std::size_t offset = 0; offset < block_size; ++offset) {
        if (static_cast<float>(input.get_value(offset, 0))
            != static_cast<float>(input.get_value(offset, 1))) {
            ++state.mismatches;
        }
    }
}

// Compares the two channels of a planar stereo input.
struct PlanarStereoAlignmentProbe {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t mismatches = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>("in", iv::InputAccess::sequential,
            {.channel_layout = {
                .channel_type = iv::ChannelTypeId::stereo,
                .sample_layout = iv::SampleStreamLayout::planar,
            }})};
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(iv::TickBlockContext<PlanarStereoAlignmentProbe> const& ctx) const
    {
        count_channel_mismatches(ctx.state(), ctx.input<"in">(), ctx.block_size);
    }
};

// Compares the two channels of an interleaved stereo input.
struct InterleavedStereoAlignmentProbe {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t mismatches = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>("in", iv::InputAccess::sequential,
            {.channel_layout = {
                .channel_type = iv::ChannelTypeId::stereo,
                .sample_layout = iv::SampleStreamLayout::interleaved,
            }})};
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(
        iv::TickBlockContext<InterleavedStereoAlignmentProbe> const& ctx) const
    {
        count_channel_mismatches(ctx.state(), ctx.input<"in">(), ctx.block_size);
    }
};

// Outputs its input plus one. Fed back through a detach of latency L with
// initial value 0, it outputs floor(position / L) + 1.
struct FeedbackCounter {
    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>("in", iv::InputAccess::sequential)};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::sequential)};
    }

    void tick_block(iv::TickBlockContext<FeedbackCounter> const& ctx) const
    {
        auto const input = ctx.input<"in">();
        auto output = ctx.output<"out">();
        for (std::size_t offset = 0; offset < ctx.block_size; ++offset) {
            output.push(iv::Sample{
                static_cast<float>(input.get_value(offset)) + 1.0f});
        }
    }
};

// sample_ramp_source whose output latency is a construction argument. It
// authors every value final, so latency only delays its readers.
struct LatencyArgumentRampSource {
    std::size_t latency = 0;

    explicit LatencyArgumentRampSource(std::size_t latency_ = 0)
        : latency(latency_)
    {}

    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::sequential)};
    }

    void constrain_ports(
        iv::ConstrainPortsContext<LatencyArgumentRampSource>& ctx) const
    {
        ctx.output<"out">().latency() = latency;
    }

    void tick_block(
        iv::TickBlockContext<LatencyArgumentRampSource> const& ctx) const
    {
        auto output = ctx.output<"out">();
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            output.push(iv::Sample{static_cast<float>(
                ctx.index.advanced_by({.whole = i}).whole().signed_value())});
        }
    }
};

// The probe's stereo input composes the ramp (left) with the delay node's
// output (right), so the compiler delays the left channel to align them. The
// history reader reads the delay node; a successor moves it to the ramp.
void compensated_planar_composition_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto delayed = graph.node<"iv.test.graph_jit.state_context.five_sample_delay">();
    auto probe = graph.node<
        "iv.test.graph_jit.state_context.planar_stereo_alignment_probe">();
    auto history_reader =
        graph.node<"iv.test.graph_jit.state_context.large_history_consumer">();
    delayed(source);
    probe(graph.tile<iv::stereo>(source, delayed));
    history_reader(delayed);
    graph.outputs();
}

// compensated_planar_composition_module with an interleaved stereo probe.
void compensated_interleaved_composition_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto delayed = graph.node<"iv.test.graph_jit.state_context.five_sample_delay">();
    auto probe = graph.node<
        "iv.test.graph_jit.state_context.interleaved_stereo_alignment_probe">();
    auto history_reader =
        graph.node<"iv.test.graph_jit.state_context.large_history_consumer">();
    delayed(source);
    probe(graph.tile<iv::stereo>(source, delayed));
    history_reader(delayed);
    graph.outputs();
}

// The counter's input reads its own output through a detach of latency 5. The
// history reader reads the ramp; a successor moves it to the counter's output.
void feedback_counter_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto counter = graph.node<"iv.test.graph_jit.state_context.feedback_counter">();
    auto observer =
        graph.node<"iv.test.graph_jit.state_context.sample_sequence_probe">();
    auto history_reader =
        graph.node<"iv.test.graph_jit.state_context.large_history_consumer">();
    observer(counter);
    history_reader(source);
    counter(static_cast<iv::SamplePortRef>(counter).detach(5, iv::Sample{0.0f}));
    graph.outputs();
}

// The probe's ramp input reads a latent source 1000 values late, compensated for
// the large latent source on its slower input. The second reader's fast input
// reads the large latent source; a successor moves it to the probe's ramp
// source, at a compensation larger than the probe's.
void long_compensation_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.latent_window_source">();
    auto slow_source =
        graph.node<"iv.test.graph_jit.state_context.large_latent_window_source">();
    auto slow_delayed =
        graph.node<"iv.test.graph_jit.state_context.five_sample_delay">();
    auto probe = graph.node<
        "iv.test.graph_jit.state_context.compensated_ramp_history_probe">();
    auto second_reader =
        graph.node<"iv.test.graph_jit.state_context.latency_compensation_probe">();
    slow_delayed(slow_source);
    probe(source, slow_source);
    second_reader(slow_source, slow_delayed);
    graph.outputs();
}

// compensated_ramp_history_reader_module's probe, optionally after a node that
// nothing reads. Adding that node renumbers every later node's bundle.
void leading_node_module(iv::GraphBuilder& graph, bool leading = false)
{
    if (leading) {
        auto leading_source =
            graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
        (void)leading_source;
    }
    auto source = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto delayed = graph.node<"iv.test.graph_jit.state_context.five_sample_delay">();
    auto probe =
        graph.node<"iv.test.graph_jit.state_context.compensated_ramp_probe">();
    delayed(source);
    probe(source, delayed);
    graph.outputs();
}

// The probe's ramp input reads a source whose latency is the module's argument.
// Its slower input reads that source through the delay node, so the ramp
// input's compensation stays 7 whatever the source's latency.
void source_latency_module(iv::GraphBuilder& graph, std::size_t latency = 0)
{
    auto source = graph.node<
        "iv.test.graph_jit.state_context.latency_argument_ramp_source">(latency);
    auto delayed = graph.node<"iv.test.graph_jit.state_context.five_sample_delay">();
    auto probe = graph.node<
        "iv.test.graph_jit.state_context.compensated_sequence_probe">();
    delayed(source);
    probe(source, delayed);
    graph.outputs();
}

// The converted node reads the temporal node's output back through a different
// detach() latency on each channel of its stereo input.
void routed_feedback_module(iv::GraphBuilder& graph)
{
    auto temporal =
        graph.node<"iv.test.graph_jit.state_context.temporal_sample_feedback">();
    auto converted =
        graph.node<"iv.test.graph_jit.state_context.converted_sample_feedback">();
    temporal(converted);
    auto const out = static_cast<iv::SamplePortRef>(temporal);
    converted(graph.tile<iv::stereo>(
        out.detach(6, iv::Sample{-0.25f}), out.detach(7, iv::Sample{-0.25f})));
    graph.outputs();
}

// compensated_ramp_history_probe_module with a latent source that keeps history
// of its own.
void compensated_latent_history_probe_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.latent_window_source">();
    auto delayed = graph.node<"iv.test.graph_jit.state_context.five_sample_delay">();
    auto probe = graph.node<
        "iv.test.graph_jit.state_context.compensated_ramp_history_probe">();
    auto history_reader =
        graph.node<"iv.test.graph_jit.state_context.large_history_consumer">();
    delayed(source);
    probe(source, delayed);
    history_reader(delayed);
    graph.outputs();
}

// compensated_ramp_history_reader_module with a probe whose fast input is
// converted from mono to interleaved mono.
void compensated_converted_history_reader_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto delayed = graph.node<"iv.test.graph_jit.state_context.five_sample_delay">();
    auto probe = graph.node<
        "iv.test.graph_jit.state_context.interleaved_latency_compensation_probe">();
    auto history_reader =
        graph.node<"iv.test.graph_jit.state_context.large_history_consumer">();
    delayed(source);
    probe(source, delayed);
    history_reader(delayed);
    graph.outputs();
}

// compensated_ramp_history_reader_module with a probe whose ramp input keeps
// history.
void compensated_ramp_history_probe_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto delayed = graph.node<"iv.test.graph_jit.state_context.five_sample_delay">();
    auto probe = graph.node<
        "iv.test.graph_jit.state_context.compensated_ramp_history_probe">();
    auto history_reader =
        graph.node<"iv.test.graph_jit.state_context.large_history_consumer">();
    delayed(source);
    probe(source, delayed);
    history_reader(delayed);
    graph.outputs();
}
} // namespace

IV_NODE("iv.test.graph_jit.state_context.playback_sample_source", PlaybackSampleSource, "Playback sample source");
IV_NODE("iv.test.graph_jit.state_context.playback_trigger_source", PlaybackTriggerSource, "Playback trigger source");
IV_NODE("iv.test.graph_jit.state_context.playback_history_consumer", PlaybackHistoryConsumer, "Playback history consumer");
IV_NODE("iv.test.graph_jit.state_context.playback_history_peer", PlaybackHistoryPeer, "Playback history peer");
IV_NODE("iv.test.graph_jit.state_context.playback_stereo_history_consumer", PlaybackStereoHistoryConsumer, "Playback stereo history consumer");
IV_NODE("iv.test.graph_jit.state_context.playback_event_history_consumer", PlaybackEventHistoryConsumer, "Playback event history consumer");
IV_NODE("iv.test.graph_jit.state_context.playback_event_history_peer", PlaybackEventHistoryPeer, "Playback event history peer");
IV_NODE(
    "iv.test.graph_jit.state_context.module_boundary_coverage_observer",
    ModuleBoundaryCoverageObserver, "Module boundary coverage observer");
IV_MODULE(
    "iv.test.graph_jit.state_context.multi_region_boundary_child_module",
    multi_region_boundary_child_module, "Multi region boundary child module");
IV_NODE(
    "iv.test.graph_jit.state_context.persisted_stereo_recording_source",
    PersistedStereoRecordingSource, "Persisted stereo recording source");
IV_NODE(
    "iv.test.graph_jit.state_context.persisted_recording_coverage_consumer",
    PersistedRecordingCoverageConsumer, "Persisted recording coverage consumer");
IV_MODULE(
    "iv.test.graph_jit.state_context.persisted_recording_coverage_module",
    persisted_recording_coverage_module, "Persisted recording coverage module");
IV_MODULE("iv.test.graph_jit.state_context.playback_sample_history_module", playback_sample_history_module, "Playback sample history module");
IV_MODULE("iv.test.graph_jit.state_context.playback_sample_history_replacement_module", playback_sample_history_replacement_module, "Playback sample history replacement module");
IV_MODULE("iv.test.graph_jit.state_context.playback_sample_window_module", playback_sample_window_module, "Playback sample window module");
IV_MODULE("iv.test.graph_jit.state_context.playback_mixed_sample_module", playback_mixed_sample_module, "Playback mixed sample module");
IV_MODULE("iv.test.graph_jit.state_context.playback_event_history_module", playback_event_history_module, "Playback event history module");
IV_MODULE("iv.test.graph_jit.state_context.playback_event_history_replacement_module", playback_event_history_replacement_module, "Playback event history replacement module");
IV_MODULE("iv.test.graph_jit.state_context.playback_mixed_event_module", playback_mixed_event_module, "Playback mixed event module");
IV_MODULE("iv.test.graph_jit.state_context.playback_sample_rewire_module", playback_sample_rewire_module, "Playback sample rewire module");
IV_MODULE("iv.test.graph_jit.state_context.playback_event_rewire_module", playback_event_rewire_module, "Playback event rewire module");
IV_NODE("iv.test.graph_jit.state_context.latent_window_source", LatentWindowSource, "Latent window source");
IV_NODE("iv.test.graph_jit.state_context.large_latent_window_source", LargeLatentWindowSource, "Large latent window source");
IV_NODE("iv.test.graph_jit.state_context.latent_window_consumer", LatentWindowConsumer, "Latent window consumer");
IV_MODULE("iv.test.graph_jit.state_context.latent_window_compact_carry_module", latent_window_compact_carry_module, "Latent window compact carry module");
IV_MODULE("iv.test.graph_jit.state_context.latent_window_ring_module", latent_window_ring_module, "Latent window ring module");
IV_MODULE("iv.test.graph_jit.state_context.latent_window_consumer_compact_carry_module", latent_window_consumer_compact_carry_module, "Latent window consumer compact carry module");
IV_MODULE("iv.test.graph_jit.state_context.latent_window_consumer_ring_module", latent_window_consumer_ring_module, "Latent window consumer ring module");
IV_NODE("iv.test.graph_jit.state_context.compensated_ramp_probe", CompensatedRampProbe, "Compensated ramp probe");
IV_NODE("iv.test.graph_jit.state_context.compensated_ramp_history_probe", CompensatedRampHistoryProbe, "Compensated ramp history probe");
IV_MODULE("iv.test.graph_jit.state_context.compensated_ramp_second_reader_module", compensated_ramp_second_reader_module, "Compensated ramp second reader module");
IV_MODULE("iv.test.graph_jit.state_context.compensated_ramp_history_reader_module", compensated_ramp_history_reader_module, "Compensated ramp history reader module");
IV_MODULE("iv.test.graph_jit.state_context.compensated_ramp_history_probe_module", compensated_ramp_history_probe_module, "Compensated ramp history probe module");
IV_NODE("iv.test.graph_jit.state_context.moving_state_probe", MovingStateProbe, "Moving state probe");
IV_MODULE("iv.test.graph_jit.state_context.moving_state_module", moving_state_module, "Moving state module");
IV_NODE("iv.test.graph_jit.state_context.offset_sample_ramp_source", OffsetSampleRampSource, "Offset sample ramp source");
IV_NODE("iv.test.graph_jit.state_context.compensated_sequence_probe", CompensatedSequenceProbe, "Compensated sequence probe");
IV_MODULE("iv.test.graph_jit.state_context.compensated_sequence_module", compensated_sequence_module, "Compensated sequence module");
IV_MODULE("iv.test.graph_jit.state_context.compensated_sequence_rewire_module", compensated_sequence_rewire_module, "Compensated sequence rewire module");
IV_MODULE("iv.test.graph_jit.state_context.compensated_latent_history_probe_module", compensated_latent_history_probe_module, "Compensated latent history probe module");
IV_MODULE("iv.test.graph_jit.state_context.compensated_converted_history_reader_module", compensated_converted_history_reader_module, "Compensated converted history reader module");
IV_NODE("iv.test.graph_jit.state_context.sample_sequence_probe", SampleSequenceProbe, "Sample sequence probe");
IV_NODE("iv.test.graph_jit.state_context.planar_stereo_alignment_probe", PlanarStereoAlignmentProbe, "Planar stereo alignment probe");
IV_NODE("iv.test.graph_jit.state_context.interleaved_stereo_alignment_probe", InterleavedStereoAlignmentProbe, "Interleaved stereo alignment probe");
IV_NODE("iv.test.graph_jit.state_context.feedback_counter", FeedbackCounter, "Feedback counter");
IV_NODE("iv.test.graph_jit.state_context.latency_argument_ramp_source", LatencyArgumentRampSource, "Latency argument ramp source");
IV_MODULE("iv.test.graph_jit.state_context.compensated_planar_composition_module", compensated_planar_composition_module, "Compensated planar composition module");
IV_MODULE("iv.test.graph_jit.state_context.compensated_interleaved_composition_module", compensated_interleaved_composition_module, "Compensated interleaved composition module");
IV_MODULE("iv.test.graph_jit.state_context.feedback_counter_module", feedback_counter_module, "Feedback counter module");
IV_MODULE("iv.test.graph_jit.state_context.long_compensation_module", long_compensation_module, "Long compensation module");
IV_MODULE("iv.test.graph_jit.state_context.leading_node_module", leading_node_module, "Leading node module");
IV_MODULE("iv.test.graph_jit.state_context.source_latency_module", source_latency_module, "Source latency module");
IV_MODULE("iv.test.graph_jit.state_context.routed_feedback_module", routed_feedback_module, "Routed feedback module");
IV_NODE("iv.test.graph_jit.state_context.neutral_feedback_counter", NeutralFeedbackCounter, "Neutral feedback counter");
IV_NODE("iv.test.graph_jit.state_context.routed_history_probe", RoutedHistoryProbe, "Routed history probe");
IV_MODULE("iv.test.graph_jit.state_context.neutral_feedback_counter_module", neutral_feedback_counter_module, "Neutral feedback counter module");
IV_MODULE("iv.test.graph_jit.state_context.routed_rewire_module", routed_rewire_module, "Routed rewire module");
IV_NODE("iv.test.graph_jit.state_context.interleaved_left_probe", InterleavedLeftProbe, "Interleaved left probe");
IV_NODE("iv.test.graph_jit.state_context.converted_sequence_probe", ConvertedSequenceProbe, "Converted sequence probe");
IV_NODE("iv.test.graph_jit.state_context.neutral_sequence_probe", NeutralSequenceProbe, "Neutral sequence probe");
IV_NODE("iv.test.graph_jit.state_context.neutral_compensated_sequence_probe", NeutralCompensatedSequenceProbe, "Neutral compensated sequence probe");
IV_MODULE("iv.test.graph_jit.state_context.tiled_rewire_module", tiled_rewire_module, "Tiled rewire module");
IV_MODULE("iv.test.graph_jit.state_context.interleaved_mono_rewire_module", interleaved_mono_rewire_module, "Interleaved mono rewire module");
IV_MODULE("iv.test.graph_jit.state_context.neutral_latency_module", neutral_latency_module, "Neutral latency module");
IV_MODULE("iv.test.graph_jit.state_context.neutral_compensated_module", neutral_compensated_module, "Neutral compensated module");
IV_MODULE("iv.test.graph_jit.state_context.connected_input_module", connected_input_module, "Connected input module");
IV_MODULE("iv.test.graph_jit.state_context.tiled_compensation_module", tiled_compensation_module, "Tiled compensation module");
IV_MODULE("iv.test.graph_jit.state_context.detach_feedback_module", detach_feedback_module, "Detach feedback module");
