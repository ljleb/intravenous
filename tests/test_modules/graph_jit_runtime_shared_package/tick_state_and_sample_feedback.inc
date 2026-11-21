#include <intravenous/dsl.h>
#include <intravenous/basic_nodes/automation.h>
#include <intravenous/basic_nodes/beats.h>
#include <intravenous/basic_nodes/output_gain.h>
#include <intravenous/basic_nodes/timeline.h>

// This fixture has an independent package catalog and registers the curve
// module without loading the rest of the builtin package.
IV_INSTANTIATE_OUTPUT_GAIN_NODES();
IV_MODULE_CLASS("builtin.automation.curve", iv::automation::AutomationCurve, "Automation curve");
IV_NODE("builtin.beats", iv::Beats, "Beats");
IV_MODULE_CLASS("builtin.timeline", iv::Timeline, "Timeline");

#include <algorithm>
#include <array>
#include <cstdint>

namespace {
struct StatefulProbe {
    struct alignas(64) State {
        std::uint64_t tick_calls = 0;
        std::uint64_t skip_calls = 0;
        std::uint64_t last_index = 0;
        std::uint64_t last_block_size = 0;
        std::uint64_t sample_rate = 0;
        std::uint64_t observed_state_extent = 0;
        std::uint64_t observed_background_extent = 0;
    };

    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    bool can_skip_block() const { return true; }

    void tick_block(iv::TickBlockContext<StatefulProbe> const& ctx) const
    {
        auto& state = ctx.state();
        ++state.tick_calls;
        state.last_index = ctx.index.whole().bits();
        state.last_block_size = ctx.block_size;
        state.sample_rate = ctx.sample_rate;
        state.observed_state_extent = ctx.buffer.size();
        state.observed_background_extent = 0;
    }

    void skip_block(iv::SkipBlockContext<StatefulProbe> const& ctx) const
    {
        auto& state = ctx.state();
        ++state.skip_calls;
        state.last_index = ctx.index.whole().bits();
        state.last_block_size = ctx.block_size;
        state.sample_rate = ctx.sample_rate;
        state.observed_state_extent = ctx.buffer.size();
        state.observed_background_extent = 0;
    }
};

struct StateOnlyProbe {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t observed_state_extent = 0;
        std::uint64_t observed_background_extent = 0;
    };

    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(iv::TickBlockContext<StateOnlyProbe> const& ctx) const
    {
        auto& state = ctx.state();
        ++state.calls;
        state.observed_state_extent = ctx.buffer.size();
        state.observed_background_extent = 0;
    }
};

struct BackgroundOnlyProbe {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t observed_state_extent = 0;
        std::uint64_t observed_background_extent = 0;
    };

    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::tuple{iv::output<iv::Sample>("out", iv::OutputProduction::coverage)};
    }

    void propagate_forward_coverage(iv::PropagateForwardCoverageContext<BackgroundOnlyProbe>& ctx) const
    { ctx.output<"out">().publish(std::span<iv::GlobalIndexRange const>{}); }
    void tock_coverage(iv::TockCoverageContext<BackgroundOnlyProbe>&) const {}
};

struct BackgroundExecutionProbe {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t sample_rate = 0;
    };

    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::coverage)};
    }

    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<BackgroundExecutionProbe>& context) const
    {
        iv::IndexCoverage const selected{iv::GlobalIndexRange{
            .begin = iv::GlobalIndex::from_bits(10),
            .extent = 3,
        }};
        std::array const regions{iv::GlobalIndexRange{
                .begin = iv::GlobalIndex::from_bits(10),
                .extent = 3,
            }};
        context.template output<"out">().publish(regions);
        context.template output<"out">().change(selected);
    }

    void tock_coverage(
        iv::TockCoverageContext<BackgroundExecutionProbe>& context) const
    {
        auto& state = context.state();
        ++state.calls;
        state.sample_rate = context.sample_rate;
        for (auto run : context.template output<"out">().prepared_runs()) {
            for (std::size_t slot = 0; slot < run.size(); ++slot) {
                run.value(slot) = iv::Sample{static_cast<float>(run.position(slot).signed_value())};
            }
        }
    }
};

struct StatelessProbe {
    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(iv::TickBlockContext<StatelessProbe> const&) const {}
};

struct ConfiguredProbe {
    std::size_t first = 0;
    std::size_t second = 0;

    struct State {
        std::uint64_t calls = 0;
        std::size_t first = 0;
        std::size_t second = 0;
    };

    constexpr ConfiguredProbe(std::size_t first_, std::size_t second_)
        : first(first_), second(second_)
    {}

    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(iv::TickBlockContext<ConfiguredProbe> const& ctx) const
    {
        auto& state = ctx.state();
        ++state.calls;
        state.first = first;
        state.second = second;
    }
};

struct MessageReplacementEvent {
    int delta = 0;
};

struct MessageReplacementProbe {
    int successor_value = 40;

    static constexpr auto inputs() { return std::tuple{}; }
    static constexpr auto outputs() { return std::tuple{}; }

    void on_message(
        iv::NodeMessageContext<MessageReplacementProbe>& context,
        MessageReplacementEvent const& event) const
    {
        context.replace<"iv.test.graph_jit.state_context.message_successor">(
            successor_value + event.delta);
    }

    void tick(iv::TickSampleContext<MessageReplacementProbe> const&) const {}
};

struct MessageSuccessorProbe {
    int value = 0;

    constexpr explicit MessageSuccessorProbe(int value_) : value(value_) {}

    static constexpr auto inputs() { return std::tuple{}; }
    static constexpr auto outputs() { return std::tuple{}; }

    void tick(iv::TickSampleContext<MessageSuccessorProbe> const&) const {}
};

inline constexpr std::array<std::uint64_t, 4> pointer_probe_values{
    0x1111111111111111ull,
    0x2222222222222222ull,
    0x3333333333333333ull,
    0x4444444444444444ull,
};

struct PointerConfiguredProbe {
    std::uint32_t marker = 0;
    std::uint64_t const* first = nullptr;
    std::uint64_t const* second = nullptr;
    std::uint64_t const* optional = nullptr;
    std::uint16_t tag = 0;

    struct State {
        std::uint64_t calls = 0;
        std::uint64_t first_value = 0;
        std::uint64_t second_value = 0;
        std::uint64_t null_seen = 0;
        std::uint32_t marker = 0;
        std::uint16_t tag = 0;
    };

    constexpr PointerConfiguredProbe(
        std::uint32_t marker_,
        std::uint64_t const* first_,
        std::uint64_t const* second_,
        std::uint64_t const* optional_,
        std::uint16_t tag_)
        : marker(marker_), first(first_), second(second_), optional(optional_), tag(tag_)
    {}

    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(iv::TickBlockContext<PointerConfiguredProbe> const& ctx) const
    {
        auto& state = ctx.state();
        ++state.calls;
        state.first_value = *first;
        state.second_value = *second;
        state.null_seen = optional == nullptr ? 1u : 0u;
        state.marker = marker;
        state.tag = tag;
    }
};

struct LimitedBlockProbe {
    struct State {
        std::uint64_t tick_calls = 0;
        std::uint64_t skip_calls = 0;
        std::array<std::uint64_t, 8> tick_indices{};
        std::array<std::uint64_t, 8> tick_sizes{};
        std::array<std::uint64_t, 8> skip_indices{};
        std::array<std::uint64_t, 8> skip_sizes{};
    };

    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    std::size_t max_block_size() const { return 16; }
    bool can_skip_block() const { return true; }

    void tick_block(iv::TickBlockContext<LimitedBlockProbe> const& ctx) const
    {
        auto& state = ctx.state();
        auto const index = state.tick_calls++;
        if (index < state.tick_indices.size()) {
            state.tick_indices[index] = ctx.index.whole().bits();
            state.tick_sizes[index] = ctx.block_size;
        }
    }

    void skip_block(iv::SkipBlockContext<LimitedBlockProbe> const& ctx) const
    {
        auto& state = ctx.state();
        auto const index = state.skip_calls++;
        if (index < state.skip_indices.size()) {
            state.skip_indices[index] = ctx.index.whole().bits();
            state.skip_sizes[index] = ctx.block_size;
        }
    }
};

struct SampleRampSource {
    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::sequential)};
    }

    void tick_block(iv::TickBlockContext<SampleRampSource> const& ctx) const
    {
        auto output = ctx.template output<0>();
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            output.push(iv::Sample{
                static_cast<float>(ctx.index.advanced_by({.whole = i})
                    .whole().signed_value())});
        }
    }
};

struct RevisingSampleSource {
    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::sequential)};
    }

    void constrain_ports(
        iv::ConstrainPortsContext<RevisingSampleSource>& ctx) const
    {
        ctx.output<"out">().latency() = 1;
    }

    void tick_block(iv::TickBlockContext<RevisingSampleSource> const& ctx) const
    {
        auto output = ctx.template output<0>();
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            auto const position = ctx.index.advanced_by({.whole = i}).whole();
            if (position != iv::GlobalIndex{}) {
                output.update(iv::Sample{
                    static_cast<float>(99 + position.signed_value())});
            }
            output.push(iv::Sample{static_cast<float>(position.signed_value())});
        }
    }
};

struct TickCaptureSource {
    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::tuple{
            iv::output<iv::Sample>("samples", iv::OutputProduction::sequential, iv::OutputRealization::buffered),
            iv::event_output("events", iv::EventTypeId::trigger, iv::OutputProduction::sequential, iv::OutputRealization::buffered, {.max_events_per_index = 0.25}),
        };
    }

    void constrain_ports(
        iv::ConstrainPortsContext<TickCaptureSource>& ctx) const
    {
        ctx.output<"samples">().history() = 2;
        ctx.output<"samples">().latency() = 1;
        ctx.output<"events">().history() = 2;
        ctx.output<"events">().latency() = 1;
    }

    void tick_block(iv::TickBlockContext<TickCaptureSource> const& ctx) const
    {
        auto samples = ctx.template output<"samples">();
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            samples[i] = iv::Sample{
                static_cast<float>(ctx.index.advanced_by({.whole = i})
                    .whole().signed_value())
                    + 0.5f};
        }
        if (ctx.block_size > 3) {
            ctx.template output<"events">().push(iv::TriggerEvent{}, 3);
        }
    }
};

struct PersistentRevisingSampleSource {
    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::sequential)};
    }

    void constrain_ports(
        iv::ConstrainPortsContext<PersistentRevisingSampleSource>& ctx) const
    {
        ctx.output<"out">().history() = 5000;
        ctx.output<"out">().latency() = 2;
    }

    void tick_block(iv::TickBlockContext<PersistentRevisingSampleSource> const& ctx) const
    {
        auto output = ctx.template output<0>();
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            auto const position = ctx.index.advanced_by({.whole = i}).whole();
            if (position != iv::GlobalIndex{}) {
                output.update(iv::Sample{
                    static_cast<float>(99 + position.signed_value())});
            }
            output.push(iv::Sample{static_cast<float>(position.signed_value())});
        }
    }
};

struct TickFallbackSampleSource {
    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::sequential)};
    }

    void constrain_ports(
        iv::ConstrainPortsContext<TickFallbackSampleSource>& ctx) const
    {
        ctx.output<"out">().latency() = 1;
    }

    std::size_t max_block_size() const { return 2; }

    void tick(iv::TickSampleContext<TickFallbackSampleSource> const& ctx) const
    {
        auto output = ctx.template output<0>();
        auto const index = ctx.index.whole();
        if (index != iv::GlobalIndex{}) {
            output.update(iv::Sample{
                static_cast<float>(99 + index.signed_value())});
        }
        output.push(iv::Sample{static_cast<float>(index.signed_value())});
    }
};

struct LimitedSampleRampSource {
    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::sequential)};
    }

    std::size_t max_block_size() const { return 16; }

    void tick_block(iv::TickBlockContext<LimitedSampleRampSource> const& ctx) const
    {
        auto output = ctx.template output<0>();
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            output.push(iv::Sample{
                static_cast<float>(ctx.index.advanced_by({.whole = i})
                    .whole().signed_value())});
        }
    }
};

struct SampleConsumerProbe {
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
        return std::array{iv::input<iv::Sample>("in", iv::InputAccess::sequential)};
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(iv::TickBlockContext<SampleConsumerProbe> const& ctx) const
    {
        auto& state = ctx.state();
        auto const block = ctx.template input<0>().block();
        ++state.calls;
        state.last_index = ctx.index.whole().bits();
        state.last_block_size = ctx.block_size;
        state.first = block.empty() ? iv::Sample{0} : block[0];
        state.last = block.empty() ? iv::Sample{0} : block[block.size() - 1];
        state.sum = 0.0f;
        for (auto const sample : block) {
            state.sum += sample;
        }
    }
};

struct SampleFeedbackA {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t scc_feedback_latency = 0;
        std::array<std::uint64_t, 24> indices{};
        std::array<std::uint64_t, 24> block_sizes{};
        std::array<float, 24> first_inputs{};
        std::array<float, 24> last_inputs{};
        std::uint32_t marker = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>("in", iv::InputAccess::sequential)};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::sequential)};
    }

    void tick_block(iv::TickBlockContext<SampleFeedbackA> const& ctx) const
    {
        auto& state = ctx.state();
        auto const input = ctx.template input<0>().block();
        auto const index = static_cast<std::size_t>(state.calls);
        if (index < state.indices.size()) {
            state.indices[index] = ctx.index.whole().bits();
            state.block_sizes[index] = ctx.block_size;
            state.first_inputs[index] = input.empty()
                ? 0.0f
                : static_cast<float>(input[0]);
            state.last_inputs[index] = input.empty()
                ? 0.0f
                : static_cast<float>(input[input.size() - 1]);
        }
        ++state.calls;
        state.scc_feedback_latency = ctx.scc_feedback_latency;
        state.marker = 0x5a11ce01u;
        auto output = ctx.template output<0>();
        for (auto const sample : input) {
            output.push(sample + 1.0f);
        }
    }
};

struct SampleFeedbackB {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t scc_feedback_latency = 0;
        std::array<std::uint64_t, 24> indices{};
        std::array<std::uint64_t, 24> block_sizes{};
        std::array<float, 24> first_inputs{};
        std::array<float, 24> last_inputs{};
        std::uint64_t marker = 0;
        std::uint64_t distinct_padding = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>("in", iv::InputAccess::sequential)};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::sequential)};
    }

    void tick_block(iv::TickBlockContext<SampleFeedbackB> const& ctx) const
    {
        auto& state = ctx.state();
        auto const input = ctx.template input<0>().block();
        auto const index = static_cast<std::size_t>(state.calls);
        if (index < state.indices.size()) {
            state.indices[index] = ctx.index.whole().bits();
            state.block_sizes[index] = ctx.block_size;
            state.first_inputs[index] = input.empty()
                ? 0.0f
                : static_cast<float>(input[0]);
            state.last_inputs[index] = input.empty()
                ? 0.0f
                : static_cast<float>(input[input.size() - 1]);
        }
        ++state.calls;
        state.scc_feedback_latency = ctx.scc_feedback_latency;
        state.marker = 0x5b22ce02ull;
        auto output = ctx.template output<0>();
        for (auto const sample : input) {
            output.push(sample);
        }
    }
};

struct MultiBranchSampleFeedback {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t scc_feedback_latency = 0;
        std::array<std::uint64_t, 24> indices{};
        std::array<std::uint64_t, 24> block_sizes{};
        std::array<float, 24> first_fast{};
        std::array<float, 24> last_fast{};
        std::array<float, 24> first_slow{};
        std::array<float, 24> last_slow{};
        std::array<float, 24> first_initialized{};
        std::array<float, 24> last_initialized{};
        std::uint32_t marker = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{
            iv::input<iv::Sample>("fast", iv::InputAccess::sequential),
            iv::input<iv::Sample>("slow", iv::InputAccess::sequential),
            iv::input<iv::Sample>("initialized", iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::sequential)};
    }

    void tick_block(iv::TickBlockContext<MultiBranchSampleFeedback> const& ctx) const
    {
        auto& state = ctx.state();
        auto const fast = ctx.template input<0>().block();
        auto const slow = ctx.template input<1>().block();
        auto const initialized = ctx.template input<2>().block();
        auto const index = static_cast<std::size_t>(state.calls);
        if (index < state.indices.size()) {
            state.indices[index] = ctx.index.whole().bits();
            state.block_sizes[index] = ctx.block_size;
            if (!fast.empty()) {
                state.first_fast[index] = static_cast<float>(fast[0]);
                state.last_fast[index] = static_cast<float>(fast[fast.size() - 1]);
                state.first_slow[index] = static_cast<float>(slow[0]);
                state.last_slow[index] = static_cast<float>(slow[slow.size() - 1]);
                state.first_initialized[index] = static_cast<float>(initialized[0]);
                state.last_initialized[index] = static_cast<float>(initialized[initialized.size() - 1]);
            }
        }
        ++state.calls;
        state.scc_feedback_latency = ctx.scc_feedback_latency;
        state.marker = 0x6d756c74u;
        auto output = ctx.template output<0>();
        for (auto const sample : fast) {
            output.push(sample + 1.0f);
        }
    }
};

struct TemporalSampleFeedback {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t scc_feedback_latency = 0;
        std::array<std::uint64_t, 24> indices{};
        std::array<std::uint64_t, 24> block_sizes{};
        std::array<float, 24> current_inputs{};
        std::array<float, 24> history_3_inputs{};
        std::array<float, 24> first_inputs{};
        std::array<float, 24> last_inputs{};
        std::uint32_t marker = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>("in", iv::InputAccess::sequential)};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::sequential)};
    }

    void constrain_ports(
        iv::ConstrainPortsContext<TemporalSampleFeedback>& ctx) const
    {
        ctx.input<"in">().history() = 3;
        ctx.output<"out">().latency() = 2;
    }

    void tick_block(iv::TickBlockContext<TemporalSampleFeedback> const& ctx) const
    {
        auto& state = ctx.state();
        auto const input = ctx.template input<0>().block();
        auto const index = static_cast<std::size_t>(state.calls);
        if (index < state.indices.size()) {
            state.indices[index] = ctx.index.whole().bits();
            state.block_sizes[index] = ctx.block_size;
            state.current_inputs[index] = static_cast<float>(ctx.template input<0>().get());
            state.history_3_inputs[index] =
                static_cast<float>(ctx.template input<0>().get(3));
            state.first_inputs[index] = input.empty()
                ? 0.0f
                : static_cast<float>(input[0]);
            state.last_inputs[index] = input.empty()
                ? 0.0f
                : static_cast<float>(input[input.size() - 1]);
        }
        ++state.calls;
        state.scc_feedback_latency = ctx.scc_feedback_latency;
        state.marker = 0x7e4fba11u;
        auto output = ctx.template output<0>();
        for (auto const sample : input) {
            output.push(sample + 1.0f);
        }
    }
};

struct RevisingSampleFeedback {
    struct State {
        std::uint64_t calls = 0;
        std::array<std::uint64_t, 8> indices{};
        std::array<float, 8> first_inputs{};
        std::array<float, 8> last_inputs{};
        std::uint32_t marker = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>("in", iv::InputAccess::sequential)};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::sequential)};
    }

    void constrain_ports(
        iv::ConstrainPortsContext<RevisingSampleFeedback>& ctx) const
    {
        ctx.output<"out">().latency() = 2;
    }

    void tick_block(iv::TickBlockContext<RevisingSampleFeedback> const& ctx) const
    {
        auto& state = ctx.state();
        auto const index = static_cast<std::size_t>(state.calls);
        if (index < state.indices.size()) {
            state.indices[index] = ctx.index.whole().bits();
            auto const block = ctx.template input<0>().block();
            state.first_inputs[index] = block.empty()
                ? 0.0f
                : static_cast<float>(block[0]);
            state.last_inputs[index] = block.empty()
                ? 0.0f
                : static_cast<float>(block[block.size() - 1]);
        }
        ++state.calls;
        state.marker = 0x5a17e001u;

        auto output = ctx.template output<0>();
        if (ctx.index == iv::FractionalGlobalIndex::from_global(
                iv::GlobalIndex::from_bits(4))) {
            // Frame 3 was authored by the preceding root call and remains
            // revisable because the output declares two frames of latency.
            output.update(iv::Sample{103.0f});
        }
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            output.push(iv::Sample{
                static_cast<float>(ctx.index.advanced_by({.whole = i}).whole().signed_value())});
        }
    }
};

struct ProjectedRevisingSampleFeedback {
    struct State {
        std::uint64_t calls = 0;
        std::array<std::uint64_t, 8> indices{};
        std::array<float, 8> first_left{};
        std::array<float, 8> first_right{};
        std::array<float, 8> last_left{};
        std::array<float, 8> last_right{};
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

    void constrain_ports(
        iv::ConstrainPortsContext<ProjectedRevisingSampleFeedback>& ctx) const
    {
        ctx.output<"out">().latency() = 2;
    }

    void tick_block(
        iv::TickBlockContext<ProjectedRevisingSampleFeedback> const& ctx) const
    {
        auto& state = ctx.state();
        auto const index = static_cast<std::size_t>(state.calls);
        if (index < state.indices.size()) {
            state.indices[index] = ctx.index.whole().bits();
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
        state.marker = 0x52e71e55u;

        auto output = ctx.template output<0>();
        if (ctx.index == iv::FractionalGlobalIndex::from_global(
                iv::GlobalIndex::from_bits(4))) {
            output.update(iv::Sample{103.0f});
        }
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            output.push(iv::Sample{
                static_cast<float>(ctx.index.advanced_by({.whole = i}).whole().signed_value())});
        }
    }
};
