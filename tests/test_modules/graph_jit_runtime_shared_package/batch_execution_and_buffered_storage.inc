#include <intravenous/dsl.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace {

struct NativeBatchTickProbe {
    std::uint64_t id = 0;

    struct State {
        std::uint64_t tick_batch_calls = 0;
        std::uint64_t skip_batch_calls = 0;
        std::uint64_t last_lane_count = 0;
        std::uint64_t configured_id = 0;
        std::uint64_t last_index = 0;
        std::uint64_t last_block_size = 0;
    };

    constexpr explicit NativeBatchTickProbe(std::uint64_t id_) : id(id_) {}

    static constexpr auto inputs() { return std::tuple{}; }
    static constexpr auto outputs() { return std::tuple{}; }
    bool can_skip_block() const { return true; }

    static void tick_block_batch(
        iv::TickBlockBatchContext<NativeBatchTickProbe> const& context)
    {
        for (auto lane : context) {
            auto& state = lane.state();
            ++state.tick_batch_calls;
            state.last_lane_count = context.size();
            state.configured_id = lane.node().id;
            state.last_index = lane.context().index.whole().bits();
            state.last_block_size = lane.context().block_size;
        }
    }

    static void skip_block_batch(
        iv::SkipBlockBatchContext<NativeBatchTickProbe> const& context)
    {
        for (auto lane : context) {
            auto& state = lane.state();
            ++state.skip_batch_calls;
            state.last_lane_count = context.size();
            state.configured_id = lane.node().id;
            state.last_index = lane.context().index.whole().bits();
            state.last_block_size = lane.context().block_size;
        }
    }
};

struct NativeBatchAliasingProbe {
    std::uint64_t id = 0;

    struct State {
        std::uint64_t tick_batch_calls = 0;
        std::uint64_t last_lane_count = 0;
        std::uint64_t configured_id = 0;
    };

    constexpr explicit NativeBatchAliasingProbe(std::uint64_t id_) : id(id_) {}

    static constexpr auto inputs() { return std::tuple{}; }
    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>(
            {"out", "Out"}, iv::OutputProduction::sequential)};
    }

    static void tick_block_batch(
        iv::TickBlockBatchContext<NativeBatchAliasingProbe> const& context)
    {
        for (auto lane : context) {
            auto& state = lane.state();
            ++state.tick_batch_calls;
            state.last_lane_count = context.size();
            state.configured_id = lane.node().id;
            for (auto& sample : lane.template output<"out">().block()) {
                sample = static_cast<float>(lane.node().id);
            }
        }
    }
};

struct NativeBatchReplaySource {
    std::uint64_t id = 0;
    static constexpr iv::TickReplayMode replay_mode = iv::TickReplayMode::full;

    constexpr explicit NativeBatchReplaySource(std::uint64_t id_) : id(id_) {}

    static constexpr auto inputs() { return std::tuple{}; }
    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>(
            {"out", "Out"}, iv::OutputProduction::sequential)};
    }

    static void tick_block_batch(
        iv::TickBlockBatchContext<NativeBatchReplaySource> const& context)
    {
        for (auto lane : context) {
            auto output = lane.template output<"out">().block();
            auto const begin = lane.context().index.whole().bits();
            for (std::size_t i = 0; i < output.size(); ++i) {
                output[i] = static_cast<float>(lane.node().id + begin + i);
            }
        }
    }
};

struct NativeBatchBackgroundProbe {
    std::uint64_t id = 0;

    struct State {
        std::uint64_t tock_batch_calls = 0;
        std::uint64_t last_lane_count = 0;
        std::uint64_t configured_id = 0;
        std::uint64_t sample_rate = 0;
    };

    constexpr explicit NativeBatchBackgroundProbe(std::uint64_t id_) : id(id_) {}

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>(
            {"in", "In"}, iv::InputAccess::coverage)};
    }
    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>(
            {"out", "Out"}, iv::OutputProduction::coverage)};
    }

    static void propagate_forward_coverage_batch(
        iv::PropagateForwardCoverageBatchContext<
            NativeBatchBackgroundProbe>& context)
    {
        for (auto lane : context) {
            iv::IndexCoverage const selected{iv::GlobalIndexRange{
                .begin = iv::GlobalIndex::from_bits(lane.node().id),
                .extent = context.size(),
            }};
            std::array const regions{iv::GlobalIndexRange{
                    .begin = iv::GlobalIndex::from_bits(lane.node().id),
                    .extent = context.size(),
                }};
            lane.template output<"out">().publish(regions);
            lane.template output<"out">().change(selected);
        }
    }

    static void tock_coverage_batch(
        iv::TockCoverageBatchContext<NativeBatchBackgroundProbe>& context)
    {
        for (auto lane : context) {
            auto& state = lane.state();
            ++state.tock_batch_calls;
            state.last_lane_count = context.size();
            state.configured_id = lane.node().id;
            state.sample_rate = lane.context().sample_rate;
        }
    }

    static void propagate_reverse_coverage_batch(
        iv::PropagateReverseCoverageBatchContext<
            NativeBatchBackgroundProbe>& context)
    {
        for (auto lane : context) {
            lane.template input<"in">().require(iv::GlobalIndexRange{
                .begin = iv::GlobalIndex::from_bits(lane.node().id),
                .extent = context.size(),
            });
        }
    }
};

struct NativeBatchTockProbe {
    std::uint64_t id = 0;

    struct State {
        std::uint64_t tock_batch_calls = 0;
        std::uint64_t last_lane_count = 0;
        std::uint64_t configured_id = 0;
        std::uint64_t sample_rate = 0;
    };

    constexpr explicit NativeBatchTockProbe(std::uint64_t id_) : id(id_) {}

    static constexpr auto inputs() { return std::tuple{}; }
    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>(
            {"out", "Out"}, iv::OutputProduction::coverage)};
    }

    static void propagate_forward_coverage_batch(
        iv::PropagateForwardCoverageBatchContext<NativeBatchTockProbe>& context)
    {
        for (auto lane : context) {
            iv::IndexCoverage const selected{iv::GlobalIndexRange{
                .begin = iv::GlobalIndex::from_bits(lane.node().id),
                .extent = context.size(),
            }};
            std::array const regions{iv::GlobalIndexRange{
                    .begin = iv::GlobalIndex::from_bits(lane.node().id),
                    .extent = context.size(),
                }};
            lane.template output<"out">().publish(regions);
            lane.template output<"out">().change(selected);
        }
    }

    static void tock_coverage_batch(
        iv::TockCoverageBatchContext<NativeBatchTockProbe>& context)
    {
        for (auto lane : context) {
            auto& state = lane.state();
            ++state.tock_batch_calls;
            state.last_lane_count = context.size();
            state.configured_id = lane.node().id;
            state.sample_rate = lane.context().sample_rate;
        }
    }
};

struct NativeBatchActivityProbe {
    struct State {
        std::uint64_t tick_batch_calls = 0;
        std::uint64_t skip_batch_calls = 0;
        std::uint64_t tick_lane_total = 0;
        std::uint64_t skip_lane_total = 0;
    };

    // No continuous input: a block callback with one uses the shared
    // contiguous-input scratch area, which keeps its ready group scalar.
    static constexpr auto inputs() { return std::tuple{}; }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>(
            {"out", "Out"}, iv::OutputProduction::sequential)};
    }

    bool can_skip_block() const { return true; }

    static void tick_block_batch(
        iv::TickBlockBatchContext<NativeBatchActivityProbe> const& context)
    {
        for (auto lane : context) {
            auto& state = lane.state();
            ++state.tick_batch_calls;
            state.tick_lane_total += context.size();
            for (auto& sample : lane.template output<"out">().block()) {
                sample = {};
            }
        }
    }

    static void skip_block_batch(
        iv::SkipBlockBatchContext<NativeBatchActivityProbe> const& context)
    {
        for (auto lane : context) {
            auto& state = lane.state();
            ++state.skip_batch_calls;
            state.skip_lane_total += context.size();
            for (auto& sample : lane.template output<"out">().block()) {
                sample = {};
            }
        }
    }
};

struct NativeBatchActivitySink {
    static constexpr auto inputs()
    {
        return std::array{
            iv::input<iv::Sample>({"first", "First"}, iv::InputAccess::sequential),
            iv::input<iv::Sample>({"second", "Second"}, iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs() { return std::tuple{}; }

    void tick_block(
        iv::TickBlockContext<NativeBatchActivitySink> const&) const
    {}
};

struct ActivityStepSource {
    static constexpr auto inputs() { return std::tuple{}; }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>(
            {"out", "Out"}, iv::OutputProduction::sequential)};
    }

    void tick_block(
        iv::TickBlockContext<ActivityStepSource> const& ctx) const
    {
        auto output = ctx.output<"out">().block();
        auto const value = ctx.index.whole().bits() < 128 ? 0.0f : 1.0f;
        for (auto& sample : output) sample = value;
    }
};

struct ActivityProbe {
    struct State {
        std::uint64_t tick_calls = 0;
        std::uint64_t skip_calls = 0;
    };

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

    bool can_skip_block() const { return true; }

    void tick_block(iv::TickBlockContext<ActivityProbe> const& ctx) const
    {
        ++ctx.state().tick_calls;
        auto output = ctx.output<"out">().block();
        for (auto& sample : output) sample = {};
    }

    void skip_block(iv::SkipBlockContext<ActivityProbe> const& ctx) const
    {
        ++ctx.state().skip_calls;
        auto output = ctx.output<"out">().block();
        for (auto& sample : output) sample = {};
    }
};

struct ActivityTypeDefaultProbe {
    struct State {
        std::uint64_t tick_calls = 0;
        std::uint64_t skip_calls = 0;
    };

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

    std::optional<std::size_t> ttl_samples() const { return 64; }
    bool can_skip_block() const { return true; }

    void tick_block(
        iv::TickBlockContext<ActivityTypeDefaultProbe> const& ctx) const
    {
        ++ctx.state().tick_calls;
        auto output = ctx.output<"out">().block();
        for (auto& sample : output) sample = {};
    }

    void skip_block(
        iv::SkipBlockContext<ActivityTypeDefaultProbe> const& ctx) const
    {
        ++ctx.state().skip_calls;
        auto output = ctx.output<"out">().block();
        for (auto& sample : output) sample = {};
    }
};

struct EventActivityStepSource {
    static constexpr auto inputs() { return std::tuple{}; }

    static constexpr auto outputs()
    {
        return std::array{iv::event_output(
            {"out", "Out"},
            iv::EventTypeId::trigger,
            iv::OutputProduction::sequential,
            {.max_events_per_index = 1.0 / 64.0})};
    }

    void tick_block(
        iv::TickBlockContext<EventActivityStepSource> const& ctx) const
    {
        if (ctx.index.whole().bits() >= 128 && ctx.block_size != 0)
            ctx.output<"out">().push(iv::TriggerEvent{}, 0);
    }
};

struct EventActivityProbe {
    struct State {
        std::uint64_t tick_calls = 0;
        std::uint64_t skip_calls = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{iv::event_input(
            {"in", "In"}, iv::EventTypeId::trigger, iv::InputAccess::sequential)};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::event_output(
            {"out", "Out"},
            iv::EventTypeId::trigger,
            iv::OutputProduction::sequential,
            {.max_events_per_index = 1.0 / 64.0})};
    }

    std::optional<std::size_t> ttl_samples() const { return 64; }
    bool can_skip_block() const { return true; }

    void tick_block(
        iv::TickBlockContext<EventActivityProbe> const& ctx) const
    {
        ++ctx.state().tick_calls;
    }

    void skip_block(
        iv::SkipBlockContext<EventActivityProbe> const& ctx) const
    {
        ++ctx.state().skip_calls;
    }
};

void activity_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<
        "iv.test.graph_jit.state_context.activity_step_source">();
    auto first = graph.node<
        "iv.test.graph_jit.state_context.activity_probe">();
    first(source);
    first.ttl(128);
    auto second = graph.node<
        "iv.test.graph_jit.state_context.activity_probe">();
    second(source);
    second.ttl(128);
    graph.outputs();
}

void activity_disabled_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<
        "iv.test.graph_jit.state_context.activity_step_source">();
    auto probe = graph.node<
        "iv.test.graph_jit.state_context.activity_probe">();
    probe(source);
    graph.outputs();
}

void activity_type_default_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<
        "iv.test.graph_jit.state_context.activity_step_source">();
    auto probe = graph.node<
        "iv.test.graph_jit.state_context.activity_type_default_probe">();
    probe(source);
    graph.outputs();
}

void event_activity_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<
        "iv.test.graph_jit.state_context.event_activity_step_source">();
    auto probe = graph.node<
        "iv.test.graph_jit.state_context.event_activity_probe">();
    probe.connect_event_input(0, source.event_port());
    graph.outputs();
}

struct DeclarationLayoutLocal {
    struct State {
        std::span<std::uint64_t> local;
        std::uint64_t calls = 0;
        std::uint64_t local_sum = 0;
    };

    static constexpr auto inputs() { return std::tuple{}; }
    static constexpr auto outputs() { return std::tuple{}; }

    void declare(iv::DeclarationContext<DeclarationLayoutLocal> const& ctx) const
    {
        ctx.local_array<&State::local>(3);
    }

    void initialize(iv::InitializationContext<DeclarationLayoutLocal> const& ctx) const
    {
        auto& state = ctx.state();
        state.local[0] = 2;
        state.local[1] = 3;
        state.local[2] = 5;
    }

    void tick_block(iv::TickBlockContext<DeclarationLayoutLocal> const& ctx) const
    {
        auto& state = ctx.state();
        ++state.calls;
        state.local_sum = state.local[0] + state.local[1] + state.local[2];
    }
};

struct DeclarationLayoutProducer {
    struct State {
        std::span<std::uint64_t> values;
        std::uint64_t calls = 0;
    };

    static constexpr auto inputs() { return std::tuple{}; }
    static constexpr auto outputs() { return std::tuple{}; }

    void declare(iv::DeclarationContext<DeclarationLayoutProducer> const& ctx) const
    {
        ctx.local_array<&State::values>(3);
        ctx.export_array<&State::values>("declaration_layout_values");
    }

    void initialize(iv::InitializationContext<DeclarationLayoutProducer> const& ctx) const
    {
        auto& state = ctx.state();
        state.values[0] = 11;
        state.values[1] = 13;
        state.values[2] = 17;
    }

    void tick_block(iv::TickBlockContext<DeclarationLayoutProducer> const& ctx) const
    {
        ++ctx.state().calls;
    }
};

struct DeclarationLayoutConsumer {
    struct State {
        std::span<std::uint64_t> imported;
        std::uint64_t initialize_sum = 0;
        std::uint64_t tick_sum = 0;
        std::uint64_t calls = 0;
    };

    static constexpr auto inputs() { return std::tuple{}; }
    static constexpr auto outputs() { return std::tuple{}; }

    void declare(iv::DeclarationContext<DeclarationLayoutConsumer> const& ctx) const
    {
        ctx.import_array<&State::imported>("declaration_layout_values");
    }

    void initialize(iv::InitializationContext<DeclarationLayoutConsumer> const& ctx) const
    {
        auto& state = ctx.state();
        state.initialize_sum =
            state.imported[0] + state.imported[1] + state.imported[2];
    }

    void tick_block(iv::TickBlockContext<DeclarationLayoutConsumer> const& ctx) const
    {
        auto& state = ctx.state();
        ++state.calls;
        state.tick_sum =
            state.imported[0] + state.imported[1] + state.imported[2];
    }
};

struct FFTValueProducer {
    static constexpr auto inputs()
    {
        return std::array{
            iv::input<iv::Sample>({"samples", "Samples"}, iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{
            iv::output<iv::FFTBlock>({"spectra", "Spectra"}, iv::OutputProduction::sequential),
        };
    }

    void constrain_ports(iv::ConstrainPortsContext<FFTValueProducer>& ctx) const
    {
        ctx.input<"samples">().pace() = 4;
        ctx.input<"samples">().pace().absolute() = 1;
        ctx.input<"samples">().history() = 4;
        ctx.output<"spectra">().pace() = 1;
        ctx.output<"spectra">().size() = 3;
    }

    void tick_block(iv::TickBlockContext<FFTValueProducer> const& ctx) const
    {
        auto const input = ctx.input<"samples">();
        auto const samples = input.block_extended().elements();
        auto output = ctx.output<"spectra">();
        auto spectra = output.block();
        for (std::size_t value = 0; value < spectra.size(); ++value) {
            auto const transform_end = (output.value_index() + value) * 4;
            auto const input_offset = static_cast<std::size_t>(
                transform_end - input.value_index());
            IV_ASSERT(input_offset <= samples.size()
                    && 4 <= samples.size() - input_offset,
                "FFT-value test producer lost its input history window");
            auto spectrum = spectra[value];
            IV_ASSERT(spectrum.size() == 3,
                "FFT-value test producer received the wrong resolved size");
            for (std::size_t bin = 0; bin < spectrum.size(); ++bin) {
                spectrum[bin] = iv::FFTBin{
                    static_cast<float>(samples[input_offset + bin]),
                    static_cast<float>(bin),
                };
            }
        }
    }
};

struct FFTValueConsumer {
    struct alignas(256) State {
        std::uint64_t calls = 0;
        std::uint64_t root_block_size = 0;
        std::uint64_t input_block_size = 0;
        std::uint64_t extended_block_size = 0;
        std::uint64_t value_index = 0;
        float preceding_bin = 0.0f;
        float first_bin = 0.0f;
        float last_bin = 0.0f;
    };

    static constexpr auto inputs()
    {
        return std::array{
            iv::input<iv::Sample, iv::FFTBlock>(
                {"spectra", "Spectra"}, iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs() { return std::tuple{}; }

    void constrain_ports(iv::ConstrainPortsContext<FFTValueConsumer>& ctx) const
    {
        ctx.input<"spectra">().size() = 3;
        ctx.input<"spectra">().history() = 1;
        ctx.input<"spectra">().pace() = 1;
    }

    void tick_block(iv::TickBlockContext<FFTValueConsumer> const& ctx) const
    {
        auto const selected = ctx.input<"spectra">();
        IV_ASSERT(selected.template is<iv::FFTBlock>(),
            "FFT-value test consumer selected the wrong continuous type");
        auto const input = selected.template as<iv::FFTBlock>();
        auto const values = input.block();
        auto const extended = input.block_extended();
        auto& state = ctx.state();
        ++state.calls;
        state.root_block_size = ctx.block_size;
        state.input_block_size = values.size();
        state.extended_block_size = extended.size();
        state.value_index = input.value_index();
        if (!extended.empty()) {
            state.preceding_bin = extended[0][0].real();
        }
        if (!values.empty()) {
            state.first_bin = values[0][0].real();
            state.last_bin = values[values.size() - 1][0].real();
        }
    }
};

struct FractionalIndexProducer {
    static constexpr auto inputs() { return std::tuple{}; }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::FractionalGlobalIndex>(
            {"coordinates", "Coordinates"}, iv::OutputProduction::sequential)};
    }

    void constrain_ports(
        iv::ConstrainPortsContext<FractionalIndexProducer>& ctx) const
    {
        ctx.output<"coordinates">().pace() = 2;
        ctx.output<"coordinates">().pace().absolute() = 2;
    }

    void tick_block(
        iv::TickBlockContext<FractionalIndexProducer> const& ctx) const
    {
        auto output = ctx.output<"coordinates">();
        auto values = output.block();
        for (std::size_t offset = 0; offset < values.size(); ++offset) {
            auto const index = output.value_index() + offset;
            values[offset] = iv::FractionalGlobalIndex::from_parts(
                iv::GlobalIndex::from_bits(index),
                index ^ 0x8000000000000000ull);
        }
    }
};

struct FractionalIndexConsumer {
    struct alignas(512) State {
        std::uint64_t calls = 0;
        std::uint64_t root_block_size = 0;
        std::uint64_t input_block_size = 0;
        std::uint64_t value_index = 0;
        std::uint64_t first_whole = 0;
        std::uint64_t first_fraction = 0;
        std::uint64_t last_whole = 0;
        std::uint64_t last_fraction = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::FractionalGlobalIndex>(
            {"coordinates", "Coordinates"}, iv::InputAccess::sequential)};
    }

    static constexpr auto outputs() { return std::tuple{}; }

    void constrain_ports(
        iv::ConstrainPortsContext<FractionalIndexConsumer>& ctx) const
    {
        ctx.input<"coordinates">().pace() = 1;
    }

    void tick_block(
        iv::TickBlockContext<FractionalIndexConsumer> const& ctx) const
    {
        auto const input = ctx.input<"coordinates">();
        auto const values = input.block();
        auto& state = ctx.state();
        ++state.calls;
        state.root_block_size = ctx.block_size;
        state.input_block_size = values.size();
        state.value_index = input.value_index();
        if (!values.empty()) {
            auto const first = values[0];
            auto const last = values[values.size() - 1];
            state.first_whole = first.whole().bits();
            state.first_fraction = first.fraction();
            state.last_whole = last.whole().bits();
            state.last_fraction = last.fraction();
        }
    }
};

struct PersistedFFTValueProducer {
    static constexpr auto inputs() { return std::tuple{}; }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::FFTBlock>(
            {"spectra", "Spectra"},
            iv::OutputProduction::sequential,
            iv::OutputRealization::buffered)};
    }

    void constrain_ports(
        iv::ConstrainPortsContext<PersistedFFTValueProducer>& ctx) const
    {
        ctx.output<"spectra">().size() = 3;
    }

    void tick_block(
        iv::TickBlockContext<PersistedFFTValueProducer> const& ctx) const
    {
        ctx.output<"spectra">().write_void();
    }
};

struct PersistedNonProjectRateSampleProducer {
    static constexpr auto inputs() { return std::tuple{}; }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>(
            {"samples", "Samples"},
            iv::OutputProduction::sequential,
            iv::OutputRealization::buffered)};
    }

    void constrain_ports(
        iv::ConstrainPortsContext<PersistedNonProjectRateSampleProducer>& ctx) const
    {
        ctx.output<"samples">().pace() = 2;
    }

    void tick_block(
        iv::TickBlockContext<PersistedNonProjectRateSampleProducer> const& ctx) const
    {
        ctx.output<"samples">().write_void();
    }
};

void declaration_layout_module(iv::GraphBuilder& graph)
{
    (void)graph.node<"iv.test.graph_jit.state_context.declaration_layout_local">();
    (void)graph.node<"iv.test.graph_jit.state_context.declaration_layout_producer">();
    (void)graph.node<"iv.test.graph_jit.state_context.declaration_layout_consumer">();
    graph.outputs();
}

void fft_value_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto transform = graph.node<"iv.test.graph_jit.state_context.fft_value_producer">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.fft_value_consumer">();
    transform(source);
    sink(transform);
    graph.outputs();
}

void fractional_index_value_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.fractional_index_producer">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.fractional_index_consumer">();
    sink(source);
    graph.outputs();
}

void persisted_fft_value_module(iv::GraphBuilder& graph)
{
    (void)graph.node<"iv.test.graph_jit.state_context.persisted_fft_value_producer">();
    graph.outputs();
}

void persisted_non_project_rate_sample_module(iv::GraphBuilder& graph)
{
    (void)graph.node<"iv.test.graph_jit.state_context.persisted_non_project_rate_sample_producer">();
    graph.outputs();
}
