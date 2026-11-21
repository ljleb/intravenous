struct ConvertedSampleFeedback {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t scc_feedback_latency = 0;
        std::array<std::uint64_t, 24> indices{};
        std::array<std::uint64_t, 24> block_sizes{};
        std::array<float, 24> first_left{};
        std::array<float, 24> first_right{};
        std::array<float, 24> last_left{};
        std::array<float, 24> last_right{};
        std::uint32_t marker = 0;
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
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::sequential)};
    }

    void tick_block(iv::TickBlockContext<ConvertedSampleFeedback> const& ctx) const
    {
        auto& state = ctx.state();
        auto const index = static_cast<std::size_t>(state.calls);
        if (index < state.indices.size()) {
            state.indices[index] = ctx.index.whole().bits();
            state.block_sizes[index] = ctx.block_size;
            if (ctx.block_size != 0) {
                state.first_left[index] =
                    static_cast<float>(ctx.template input<0>().get_value(0, 0));
                state.first_right[index] =
                    static_cast<float>(ctx.template input<0>().get_value(0, 1));
                state.last_left[index] = static_cast<float>(
                    ctx.template input<0>().get_value(ctx.block_size - 1, 0));
                state.last_right[index] = static_cast<float>(
                    ctx.template input<0>().get_value(ctx.block_size - 1, 1));
            }
        }
        ++state.calls;
        state.scc_feedback_latency = ctx.scc_feedback_latency;
        state.marker = 0xc04e7ed1u;
        auto output = ctx.template output<0>();
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            auto const left = ctx.template input<0>().get_value(i, 0);
            auto const right = ctx.template input<0>().get_value(i, 1);
            output.push((left + right) * 0.5f + 1.0f);
        }
    }
};

struct MonoInterleavedConsumerProbe {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t last_index = 0;
        std::uint64_t last_block_size = 0;
        float first = 0.0f;
        float last = 0.0f;
        float sum = 0.0f;
    };

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>("in", iv::InputAccess::sequential,
            {.channel_layout = {
                .channel_type = iv::ChannelTypeId::mono,
                .sample_layout = iv::SampleStreamLayout::interleaved,
            }})};
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(iv::TickBlockContext<MonoInterleavedConsumerProbe> const& ctx) const
    {
        auto& state = ctx.state();
        auto const block = ctx.template input<0>().block();
        ++state.calls;
        state.last_index = ctx.index.whole().bits();
        state.last_block_size = ctx.block_size;
        state.first = block.empty() ? 0.0f : static_cast<float>(block[0]);
        state.last = block.empty()
            ? 0.0f
            : static_cast<float>(block[block.size() - 1]);
        state.sum = 0.0f;
        for (auto const sample : block) state.sum += sample;
    }
};

struct StereoRampSource {
    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::sequential,
            {.channel_layout = {
                .channel_type = iv::ChannelTypeId::stereo,
                .sample_layout = iv::SampleStreamLayout::interleaved,
            }})};
    }

    void tick_block(iv::TickBlockContext<StereoRampSource> const& ctx) const
    {
        auto output = ctx.template output<0>();
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            auto const position = ctx.index.advanced_by({.whole = i}).whole();
            std::array<iv::Sample, 2> frame{
                iv::Sample{static_cast<float>(position.signed_value())},
                iv::Sample{
                    static_cast<float>(position.signed_value()) + 1000.0f},
            };
            output.value(i, 0) = frame[0];
            output.value(i, 1) = frame[1];
        }
    }
};

struct StereoPlanarConsumerProbe {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t last_index = 0;
        std::uint64_t last_block_size = 0;
        float first_left = 0.0f;
        float first_right = 0.0f;
        float last_left = 0.0f;
        float last_right = 0.0f;
        float sum_left = 0.0f;
        float sum_right = 0.0f;
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

    void tick_block(iv::TickBlockContext<StereoPlanarConsumerProbe> const& ctx) const
    {
        auto& state = ctx.state();
        ++state.calls;
        state.last_index = ctx.index.whole().bits();
        state.last_block_size = ctx.block_size;
        state.sum_left = 0.0f;
        state.sum_right = 0.0f;
        if (ctx.block_size != 0) {
            state.first_left = static_cast<float>(ctx.template input<0>().get_value(0, 0));
            state.first_right = static_cast<float>(ctx.template input<0>().get_value(0, 1));
            state.last_left = static_cast<float>(
                ctx.template input<0>().get_value(ctx.block_size - 1, 0));
            state.last_right = static_cast<float>(
                ctx.template input<0>().get_value(ctx.block_size - 1, 1));
        }
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            state.sum_left += static_cast<float>(ctx.template input<0>().get_value(i, 0));
            state.sum_right += static_cast<float>(ctx.template input<0>().get_value(i, 1));
        }
    }
};

struct StereoSampleConsumerProbe {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t last_index = 0;
        std::uint64_t last_block_size = 0;
        float first_left = 0.0f;
        float first_right = 0.0f;
        float last_left = 0.0f;
        float last_right = 0.0f;
        float sum_left = 0.0f;
        float sum_right = 0.0f;
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

    void tick_block(iv::TickBlockContext<StereoSampleConsumerProbe> const& ctx) const
    {
        auto& state = ctx.state();
        ++state.calls;
        state.last_index = ctx.index.whole().bits();
        state.last_block_size = ctx.block_size;
        state.sum_left = 0.0f;
        state.sum_right = 0.0f;
        if (ctx.block_size != 0) {
            state.first_left = static_cast<float>(ctx.template input<0>().get_value(0, 0));
            state.first_right = static_cast<float>(ctx.template input<0>().get_value(0, 1));
            state.last_left = static_cast<float>(
                ctx.template input<0>().get_value(ctx.block_size - 1, 0));
            state.last_right = static_cast<float>(
                ctx.template input<0>().get_value(ctx.block_size - 1, 1));
        }
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            state.sum_left += static_cast<float>(ctx.template input<0>().get_value(i, 0));
            state.sum_right += static_cast<float>(ctx.template input<0>().get_value(i, 1));
        }
    }
};

struct HistoryRampSource {
    struct State {
        std::uint64_t calls = 0;
        float previous_output = 0.0f;
        std::uint32_t marker = 0;
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
        iv::ConstrainPortsContext<HistoryRampSource>& ctx) const
    {
        ctx.output<"out">().history() = 3;
        ctx.output<"out">().latency() = 2;
    }

    void tick_block(iv::TickBlockContext<HistoryRampSource> const& ctx) const
    {
        auto& state = ctx.state();
        auto output = ctx.template output<0>();
        ++state.calls;
        state.previous_output = static_cast<float>(output.get());
        state.marker = 0x91a2b3c4u;
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            output.push(iv::Sample{
                static_cast<float>(ctx.index.advanced_by({.whole = i}).whole().signed_value())});
        }
    }
};

struct HistoryConsumerProbe {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t last_index = 0;
        float current = 0.0f;
        float history_1 = 0.0f;
        float history_5 = 0.0f;
        std::uint32_t marker = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>("in", iv::InputAccess::sequential)};
    }

    void constrain_ports(
        iv::ConstrainPortsContext<HistoryConsumerProbe>& ctx) const
    {
        ctx.input<"in">().history() = 5;
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(iv::TickBlockContext<HistoryConsumerProbe> const& ctx) const
    {
        auto& state = ctx.state();
        ++state.calls;
        state.last_index = ctx.index.whole().bits();
        state.current = static_cast<float>(ctx.template input<0>().get(0));
        state.history_1 = static_cast<float>(ctx.template input<0>().get(1));
        state.history_5 = static_cast<float>(ctx.template input<0>().get(5));
        state.marker = 0x5e6f7788u;
    }
};

struct StereoHistoryConsumerProbe {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t last_index = 0;
        float current_left = 0.0f;
        float current_right = 0.0f;
        float history_5_left = 0.0f;
        float history_5_right = 0.0f;
        std::uint64_t marker = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>("in", iv::InputAccess::sequential,
            {.channel_layout = {
                .channel_type = iv::ChannelTypeId::stereo,
                .sample_layout = iv::SampleStreamLayout::interleaved,
            }})};
    }

    void constrain_ports(
        iv::ConstrainPortsContext<StereoHistoryConsumerProbe>& ctx) const
    {
        ctx.input<"in">().history() = 5;
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(iv::TickBlockContext<StereoHistoryConsumerProbe> const& ctx) const
    {
        auto& state = ctx.state();
        ++state.calls;
        state.last_index = ctx.index.whole().bits();
        state.current_left = static_cast<float>(ctx.template input<0>().get(0, 0));
        state.current_right = static_cast<float>(ctx.template input<0>().get(0, 1));
        state.history_5_left = static_cast<float>(ctx.template input<0>().get(5, 0));
        state.history_5_right = static_cast<float>(ctx.template input<0>().get(5, 1));
        state.marker = 0xa1b2c3d4e5f60718ull;
    }
};

struct LargeHistoryRampSource {
    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::sequential)};
    }

    void tick_block(iv::TickBlockContext<LargeHistoryRampSource> const& ctx) const
    {
        auto output = ctx.template output<0>();
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            output.push(iv::Sample{
                static_cast<float>(ctx.index.advanced_by({.whole = i}).whole().signed_value())});
        }
    }
};

struct LargeHistoryConsumerProbe {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t last_index = 0;
        float current = 0.0f;
        float history_1 = 0.0f;
        float history_5000 = 0.0f;
    };

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>("in", iv::InputAccess::sequential)};
    }

    void constrain_ports(
        iv::ConstrainPortsContext<LargeHistoryConsumerProbe>& ctx) const
    {
        ctx.input<"in">().history() = 5000;
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(iv::TickBlockContext<LargeHistoryConsumerProbe> const& ctx) const
    {
        auto& state = ctx.state();
        ++state.calls;
        state.last_index = ctx.index.whole().bits();
        state.current = static_cast<float>(ctx.template input<0>().get(0));
        state.history_1 = static_cast<float>(ctx.template input<0>().get(1));
        state.history_5000 = static_cast<float>(ctx.template input<0>().get(5000));
    }
};

struct FiveSampleDelay {
    struct State {
        std::array<iv::Sample, 8> memory{};
    };

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>("in", iv::InputAccess::sequential)};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::sequential)};
    }

    void constrain_ports(iv::ConstrainPortsContext<FiveSampleDelay>& ctx) const
    {
        ctx.output<"out">().latency() = 2;
    }

    constexpr std::size_t internal_latency() const { return 5; }

    void tick_block(iv::TickBlockContext<FiveSampleDelay> const& ctx) const
    {
        auto const input = ctx.template input<0>().block();
        auto output = ctx.template output<0>();
        auto& memory = ctx.state().memory;
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            auto const index = static_cast<std::size_t>(
                ctx.index.advanced_by({.whole = i}).whole().bits());
            auto const delayed = memory[index & 7u];
            memory[(index + 5u) & 7u] = input[i];
            output.push(delayed);
        }
    }
};

// Adds one to every value its input reads. The input's neutral value is 100.
struct NeutralFeedbackCounter {
    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>(
            "in", iv::InputAccess::sequential, {.neutral_value = 100.0f})};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::sequential)};
    }

    void tick_block(iv::TickBlockContext<NeutralFeedbackCounter> const& ctx) const
    {
        auto const input = ctx.input<"in">();
        auto output = ctx.output<"out">();
        for (std::size_t offset = 0; offset < ctx.block_size; ++offset) {
            output.push(iv::Sample{
                static_cast<float>(input.get_value(offset)) + 1.0f});
        }
    }
};

// Records the right channel of its stereo input, which keeps 3 values of
// history, filed by index. Its output adds one to the left channel.
struct RoutedHistoryProbe {
    struct State {
        std::uint64_t calls = 0;
        std::array<float, 64> right{};
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
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::sequential)};
    }

    void constrain_ports(iv::ConstrainPortsContext<RoutedHistoryProbe>& ctx) const
    {
        ctx.input<"in">().history() = 3;
    }

    void tick_block(iv::TickBlockContext<RoutedHistoryProbe> const& ctx) const
    {
        auto& state = ctx.state();
        auto const input = ctx.input<"in">();
        auto output = ctx.output<"out">();
        ++state.calls;
        for (std::size_t offset = 0; offset < ctx.block_size; ++offset) {
            auto const index = static_cast<std::size_t>(
                ctx.index.advanced_by({.whole = offset}).whole().bits());
            state.right[index % state.right.size()] =
                static_cast<float>(input.get_value(offset, 1));
            output.push(iv::Sample{
                static_cast<float>(input.get_value(offset, 0)) + 1.0f});
        }
    }
};

// The counter reads its own output back through a detach() of `latency`,
// whose storage starts at the counter input's neutral value.
void neutral_feedback_counter_module(
    iv::GraphBuilder& graph,
    std::size_t latency = 5)
{
    auto counter =
        graph.node<"iv.test.graph_jit.state_context.neutral_feedback_counter">();
    auto observer =
        graph.node<"iv.test.graph_jit.state_context.sample_sequence_probe">();
    observer(counter);
    counter(static_cast<iv::SamplePortRef>(counter).detach(latency));
    graph.outputs();
}

// The probe's left channel reads the probe's own output back through detach().
// Its right channel reads the ramp, or the offset ramp when `rewired`.
void routed_rewire_module(iv::GraphBuilder& graph, bool rewired = false)
{
    auto ramp = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto offset_ramp =
        graph.node<"iv.test.graph_jit.state_context.offset_sample_ramp_source">();
    auto probe =
        graph.node<"iv.test.graph_jit.state_context.routed_history_probe">();
    auto const right = rewired
        ? static_cast<iv::SamplePortRef>(offset_ramp)
        : static_cast<iv::SamplePortRef>(ramp);
    probe(graph.tile<iv::stereo>(
        static_cast<iv::SamplePortRef>(probe).detach(4), right));
    graph.outputs();
}

// Records the left channel of its interleaved stereo input, filed by index.
struct InterleavedLeftProbe {
    struct State {
        std::uint64_t calls = 0;
        std::array<float, 64> values{};
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

    void tick_block(iv::TickBlockContext<InterleavedLeftProbe> const& ctx) const
    {
        auto& state = ctx.state();
        auto const input = ctx.input<"in">();
        ++state.calls;
        for (std::size_t offset = 0; offset < ctx.block_size; ++offset) {
            auto const index = static_cast<std::size_t>(
                ctx.index.advanced_by({.whole = offset}).whole().bits());
            state.values[index % state.values.size()] =
                static_cast<float>(input.get_value(offset, 0));
        }
    }
};

// Records its interleaved mono input, filed by index. Its slower input only
// sets that input's compensation.
struct ConvertedSequenceProbe {
    struct State {
        std::uint64_t calls = 0;
        std::array<float, 64> values{};
    };

    static constexpr auto inputs()
    {
        return std::array{
            iv::input<iv::Sample>("in", iv::InputAccess::sequential,
                {.channel_layout = {
                    .channel_type = iv::ChannelTypeId::mono,
                    .sample_layout = iv::SampleStreamLayout::interleaved,
                }}),
            iv::input<iv::Sample>("slower", iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(iv::TickBlockContext<ConvertedSequenceProbe> const& ctx) const
    {
        auto& state = ctx.state();
        auto const input = ctx.input<"in">();
        ++state.calls;
        for (std::size_t offset = 0; offset < ctx.block_size; ++offset) {
            auto const index = static_cast<std::size_t>(
                ctx.index.advanced_by({.whole = offset}).whole().bits());
            state.values[index % state.values.size()] =
                static_cast<float>(input.get_value(offset));
        }
    }
};

// sample_sequence_probe whose input's neutral value is 100.
struct NeutralSequenceProbe {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t read_latency = 0;
        std::int64_t first_index = 0;
        std::uint64_t recorded = 0;
        std::array<float, 64> values{};
    };

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>(
            "in", iv::InputAccess::sequential, {.neutral_value = 100.0f})};
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(iv::TickBlockContext<NeutralSequenceProbe> const& ctx) const
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

// compensated_sequence_probe whose ramp input's neutral value is 100.
struct NeutralCompensatedSequenceProbe {
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
            iv::input<iv::Sample>(
                "ramp", iv::InputAccess::sequential, {.neutral_value = 100.0f}),
            iv::input<iv::Sample>("slower", iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(
        iv::TickBlockContext<NeutralCompensatedSequenceProbe> const& ctx) const
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

// The probe's interleaved stereo input tiles the ramp on its left channel and
// the delay node on its right, which compensates the left channel by 7. Each
// channel aliases its source's storage. `rewired` reads the offset ramp on the
// left channel instead.
void tiled_rewire_module(iv::GraphBuilder& graph, bool rewired = false)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto offset_source =
        graph.node<"iv.test.graph_jit.state_context.offset_sample_ramp_source">();
    auto delayed = graph.node<"iv.test.graph_jit.state_context.five_sample_delay">();
    auto probe =
        graph.node<"iv.test.graph_jit.state_context.interleaved_left_probe">();
    delayed(source);
    auto const left = rewired
        ? static_cast<iv::SamplePortRef>(offset_source)
        : static_cast<iv::SamplePortRef>(source);
    probe(graph.tile<iv::stereo>(left, delayed));
    graph.outputs();
}

// The probe's interleaved mono input reads the ramp through a layout
// conversion that aliases the ramp's storage, compensated by 7 for its slower
// input, which reads the delay node. `rewired` reads the offset ramp instead.
void interleaved_mono_rewire_module(iv::GraphBuilder& graph, bool rewired = false)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto offset_source =
        graph.node<"iv.test.graph_jit.state_context.offset_sample_ramp_source">();
    auto delayed = graph.node<"iv.test.graph_jit.state_context.five_sample_delay">();
    auto probe =
        graph.node<"iv.test.graph_jit.state_context.converted_sequence_probe">();
    delayed(source);
    auto const in = rewired
        ? static_cast<iv::SamplePortRef>(offset_source)
        : static_cast<iv::SamplePortRef>(source);
    probe(in, delayed);
    graph.outputs();
}

// The probe, whose input's neutral value is 100, reads a ramp whose latency is
// the module's argument.
void neutral_latency_module(iv::GraphBuilder& graph, std::size_t latency = 0)
{
    auto source = graph.node<
        "iv.test.graph_jit.state_context.latency_argument_ramp_source">(latency);
    auto probe =
        graph.node<"iv.test.graph_jit.state_context.neutral_sequence_probe">();
    probe(source);
    graph.outputs();
}

// The probe's ramp input, whose neutral value is 100, reads the ramp
// compensated by 7 for its slower input, which reads the delay node.
void neutral_compensated_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto delayed = graph.node<"iv.test.graph_jit.state_context.five_sample_delay">();
    auto probe = graph.node<
        "iv.test.graph_jit.state_context.neutral_compensated_sequence_probe">();
    delayed(source);
    probe(source, delayed);
    graph.outputs();
}

// The probe's slower input reads the delay node. Its ramp input reads the ramp
// only when `connected`, compensated by 7. A history reader keeps the ramp's
// recent values.
void connected_input_module(iv::GraphBuilder& graph, bool connected = false)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto delayed = graph.node<"iv.test.graph_jit.state_context.five_sample_delay">();
    auto history_reader =
        graph.node<"iv.test.graph_jit.state_context.large_history_consumer">();
    auto probe = graph.node<
        "iv.test.graph_jit.state_context.compensated_sequence_probe">();
    delayed(source);
    history_reader(source);
    if (connected) probe.connect_input(0, static_cast<iv::SamplePortRef>(source));
    probe.connect_input(1, static_cast<iv::SamplePortRef>(delayed));
    graph.outputs();
}

// The probe's interleaved stereo input tiles the ramp on its left channel. Its
// right channel reads a second ramp, or the delay node when `delayed_right`,
// which compensates the left channel by 7. Each channel aliases its source's
// storage. A history reader keeps the ramp's recent values.
void tiled_compensation_module(
    iv::GraphBuilder& graph,
    bool delayed_right = false)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto other_source =
        graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto delayed = graph.node<"iv.test.graph_jit.state_context.five_sample_delay">();
    auto history_reader =
        graph.node<"iv.test.graph_jit.state_context.large_history_consumer">();
    auto probe =
        graph.node<"iv.test.graph_jit.state_context.interleaved_left_probe">();
    delayed(source);
    history_reader(source);
    auto const right = delayed_right
        ? static_cast<iv::SamplePortRef>(delayed)
        : static_cast<iv::SamplePortRef>(other_source);
    probe(graph.tile<iv::stereo>(source, right));
    graph.outputs();
}

// The converted node reads the temporal node's output back through detach(6),
// optionally after a node that nothing reads. Adding that node renumbers every
// later node's bundle.
void detach_feedback_module(iv::GraphBuilder& graph, bool leading = false)
{
    if (leading) {
        auto leading_source =
            graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
        (void)leading_source;
    }
    auto temporal =
        graph.node<"iv.test.graph_jit.state_context.temporal_sample_feedback">();
    auto converted =
        graph.node<"iv.test.graph_jit.state_context.converted_sample_feedback">();
    temporal(converted);
    converted(static_cast<iv::SamplePortRef>(temporal).detach(
        6, iv::Sample{-0.25f}));
    graph.outputs();
}

// The probe's ramp input, whose neutral value is 100, reads the ramp
// compensated by 7 for its slower input, which reads a ramp of latency 7. A
// history reader, whose neutral value is 0, reads the same ramp.
void shared_neutral_compensated_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto slower = graph.node<
        "iv.test.graph_jit.state_context.latency_argument_ramp_source">(
        std::size_t{7});
    auto history_reader =
        graph.node<"iv.test.graph_jit.state_context.large_history_consumer">();
    auto probe = graph.node<
        "iv.test.graph_jit.state_context.neutral_compensated_sequence_probe">();
    history_reader(source);
    probe(source, slower);
    graph.outputs();
}

// The probe's interleaved mono input downmixes a stereo tile of a ramp of
// latency 3 and the offset ramp. Its slower input reads a ramp of latency
// `slower_latency`, which compensates the first source by
// `slower_latency - 3` and the second by `slower_latency`.
void unequal_latency_composition_module(
    iv::GraphBuilder& graph,
    std::size_t slower_latency = 7)
{
    auto latent = graph.node<
        "iv.test.graph_jit.state_context.latency_argument_ramp_source">(
        std::size_t{3});
    auto offset =
        graph.node<"iv.test.graph_jit.state_context.offset_sample_ramp_source">();
    auto slower = graph.node<
        "iv.test.graph_jit.state_context.latency_argument_ramp_source">(
        slower_latency);
    auto probe =
        graph.node<"iv.test.graph_jit.state_context.converted_sequence_probe">();
    probe(graph.tile<iv::stereo>(latent, offset), slower);
    graph.outputs();
}

// The probe's stereo input reads a ramp on each channel. When `feedback`, its
// left channel reads the probe's own output back through detach(4) instead.
// An observer reads the probe's output.
void added_feedback_channel_module(iv::GraphBuilder& graph, bool feedback = false)
{
    auto left = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto right = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto probe =
        graph.node<"iv.test.graph_jit.state_context.routed_history_probe">();
    auto observer =
        graph.node<"iv.test.graph_jit.state_context.sample_sequence_probe">();
    observer(probe);
    auto const left_input = feedback
        ? static_cast<iv::SamplePortRef>(probe).detach(4)
        : static_cast<iv::SamplePortRef>(left);
    probe(graph.tile<iv::stereo>(left_input, right));
    graph.outputs();
}

// The probe's mono input downmixes a stereo tile of the revising source, whose
// latency is 1, and the ramp, or the offset ramp when `rewired`, compensated
// for its slower input, which reads the delay node.
void revising_tile_rewire_module(iv::GraphBuilder& graph, bool rewired = false)
{
    auto latent =
        graph.node<"iv.test.graph_jit.state_context.revising_sample_source">();
    auto ramp = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto offset =
        graph.node<"iv.test.graph_jit.state_context.offset_sample_ramp_source">();
    auto delayed = graph.node<"iv.test.graph_jit.state_context.five_sample_delay">();
    auto probe =
        graph.node<"iv.test.graph_jit.state_context.converted_sequence_probe">();
    delayed(ramp);
    auto const right = rewired
        ? static_cast<iv::SamplePortRef>(offset)
        : static_cast<iv::SamplePortRef>(ramp);
    probe(graph.tile<iv::stereo>(latent, right), delayed);
    graph.outputs();
}

// The two event feedback nodes in a loop: the first reads the second's
// triggers, and the second reads the first's back through detach(`latency`).
void event_feedback_module(iv::GraphBuilder& graph, std::size_t latency = 10)
{
    auto first = graph.node<"iv.test.graph_jit.state_context.event_feedback_a">();
    auto second = graph.node<"iv.test.graph_jit.state_context.event_feedback_b">();
    first.connect_event_input(0, second.event_port());
    second.connect_event_input(0, first.event_port().detach(latency));
    graph.outputs();
}

IV_MODULE(
    "iv.test.graph_jit.state_context.revising_tile_rewire_module",
    revising_tile_rewire_module, "Revising tile rewire module");
IV_MODULE(
    "iv.test.graph_jit.state_context.event_feedback_module",
    event_feedback_module, "Event feedback module");
IV_MODULE(
    "iv.test.graph_jit.state_context.shared_neutral_compensated_module",
    shared_neutral_compensated_module, "Shared neutral compensated module");
IV_MODULE(
    "iv.test.graph_jit.state_context.unequal_latency_composition_module",
    unequal_latency_composition_module, "Unequal latency composition module");
IV_MODULE(
    "iv.test.graph_jit.state_context.added_feedback_channel_module",
    added_feedback_channel_module, "Added feedback channel module");
