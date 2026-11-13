#include "test_support.h"

// Authored package source and its shared build/load path. CTest builds this
// package once before running the independent GraphJitRuntimeFixture tests.

namespace iv::test::graph_jit_tests {

namespace {

constexpr char graph_jit_state_package_id[] = "iv.test.graph_jit.state_context.package";

constexpr char graph_jit_runtime_fixture_name[] = "graph_jit_runtime_shared_package";

std::string_view graph_jit_runtime_package_source()
{
    // Keep each raw string literal comfortably below the implementation
    // minimum 64 KiB literal-size limit. The shared runtime package keeps
    // growing as GraphJit coverage expands, so assemble it once from
    // stable fragments instead of relying on one oversized literal.
    static constexpr std::string_view part_1 = R"cpp(
#include <intravenous/dsl.h>

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

    struct alignas(128) TockState {
        std::uint64_t tick_calls = 0;
        std::uint64_t skip_calls = 0;
        std::uint64_t last_index = 0;
        std::uint64_t last_block_size = 0;
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
    struct TockState {
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

    void tick_block(iv::TickBlockContext<BackgroundOnlyProbe> const&) const {}
};

struct BackgroundExecutionProbe {
    struct TockState {
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
        auto& state = context.tock_state();
        ++state.calls;
        state.sample_rate = context.sample_rate;
        auto output = context.template output<"out">().regions();
        for (auto region : output) {
            for (std::uint64_t offset = 0;
                 offset < region.range().extent;
                 ++offset) {
                auto const position = region.range().begin.advanced_by(offset);
                region.at(position) =
                    iv::Sample{static_cast<float>(position.signed_value())};
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
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::tick)};
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
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::tick)};
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
            iv::output<iv::Sample>("samples", iv::OutputProduction::tick, iv::OutputRetention::persisted),
            iv::event_output("events", iv::EventTypeId::trigger, iv::OutputProduction::tick, iv::OutputRetention::persisted, {.max_events_per_index = 0.25}),
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
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::tick)};
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
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::tick)};
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
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::tick)};
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
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::tick)};
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
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::tick)};
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
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::tick)};
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
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::tick)};
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
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::tick)};
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
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::tick)};
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
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::tick)};
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
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::tick,
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
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::tick)};
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
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::tick)};
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
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::tick)};
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
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::tick)};
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
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::tick)};
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
    revising_tile_rewire_module);
IV_MODULE(
    "iv.test.graph_jit.state_context.event_feedback_module",
    event_feedback_module);
IV_MODULE(
    "iv.test.graph_jit.state_context.shared_neutral_compensated_module",
    shared_neutral_compensated_module);
IV_MODULE(
    "iv.test.graph_jit.state_context.unequal_latency_composition_module",
    unequal_latency_composition_module);
IV_MODULE(
    "iv.test.graph_jit.state_context.added_feedback_channel_module",
    added_feedback_channel_module);

)cpp";
    static constexpr std::string_view part_2 = R"cpp(struct LatencyCompensationProbe {
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
            iv::input<iv::Sample>("fast", iv::InputAccess::sequential),
            iv::input<iv::Sample>("slow", iv::InputAccess::sequential),
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
            iv::input<iv::Sample>("fast", iv::InputAccess::sequential,
                {.channel_layout = {
                    .channel_type = iv::ChannelTypeId::mono,
                    .sample_layout = iv::SampleStreamLayout::interleaved,
                }}),
            iv::input<iv::Sample>("slow", iv::InputAccess::sequential,
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
        return std::array{iv::input<iv::Sample>("in", iv::InputAccess::sequential,
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
            iv::input<iv::Sample>("value", iv::InputAccess::sequential),
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
            iv::event_output("trigger", iv::EventTypeId::trigger, iv::OutputProduction::tick),
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
            iv::event_output("midi", iv::EventTypeId::midi, iv::OutputProduction::tick),
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
            iv::event_output("trigger", iv::EventTypeId::trigger, iv::OutputProduction::tick),
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
        return std::array{iv::event_output("out", iv::EventTypeId::trigger, iv::OutputProduction::tick, {.max_events_per_index = 0.125})};
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
        return std::array{iv::event_output("out", iv::EventTypeId::trigger, iv::OutputProduction::tick, {.max_events_per_index = 0.125})};
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
            iv::event_input("in", iv::EventTypeId::trigger, iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{iv::event_output("out", iv::EventTypeId::trigger, iv::OutputProduction::tick, {.max_events_per_index = 0.25})};
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
            iv::event_input("in", iv::EventTypeId::trigger, iv::InputAccess::sequential),
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
            iv::event_input("in", iv::EventTypeId::trigger, iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{iv::event_output("out", iv::EventTypeId::trigger, iv::OutputProduction::tick, {.max_events_per_index = 0.25})};
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
            iv::event_input("in", iv::EventTypeId::trigger, iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{iv::event_output("out", iv::EventTypeId::trigger, iv::OutputProduction::tick, {.max_events_per_index = 0.25})};
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
            iv::event_input("in", iv::EventTypeId::boundary, iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{iv::event_output("out", iv::EventTypeId::boundary, iv::OutputProduction::tick, {.max_events_per_index = 0.25})};
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
            iv::event_input("in", iv::EventTypeId::trigger, iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{iv::event_output("out", iv::EventTypeId::trigger, iv::OutputProduction::tick, {.max_events_per_index = 16.0})};
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
            iv::event_input("in_a", iv::EventTypeId::trigger, iv::InputAccess::sequential),
            iv::event_input("in_b", iv::EventTypeId::trigger, iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{iv::event_output("out", iv::EventTypeId::trigger, iv::OutputProduction::tick, {.max_events_per_index = 0.25})};
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
            iv::event_input("in", iv::EventTypeId::trigger, iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{iv::event_output("out", iv::EventTypeId::trigger, iv::OutputProduction::tick, {.max_events_per_index = 0.25})};
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
            iv::event_input("exact", iv::EventTypeId::trigger, iv::InputAccess::sequential),
            iv::event_input("converted", iv::EventTypeId::trigger, iv::InputAccess::sequential),
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
        return std::array{iv::event_output("out", iv::EventTypeId::trigger, iv::OutputProduction::tick, {.max_events_per_index = 0.25})};
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
            iv::event_input("in", iv::EventTypeId::trigger, iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{
            iv::event_output("exact", iv::EventTypeId::trigger, iv::OutputProduction::tick, {.max_events_per_index = 0.25}),
            iv::event_output("converted", iv::EventTypeId::boundary, iv::OutputProduction::tick, {.max_events_per_index = 0.25}),
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
            iv::event_input("trigger", iv::EventTypeId::trigger, iv::InputAccess::sequential),
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
            iv::event_input("midi", iv::EventTypeId::midi, iv::InputAccess::sequential),
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
            iv::event_input("in", iv::EventTypeId::boundary, iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{iv::event_output("out", iv::EventTypeId::boundary, iv::OutputProduction::tick, {.max_events_per_index = 0.25})};
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
            iv::event_input("in", iv::EventTypeId::boundary, iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{iv::event_output("out", iv::EventTypeId::boundary, iv::OutputProduction::tick, {.max_events_per_index = 0.25})};
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
            iv::event_input("trigger", iv::EventTypeId::trigger, iv::InputAccess::sequential),
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
            iv::event_output("trigger", iv::EventTypeId::trigger, iv::OutputProduction::tick, {.max_events_per_index = 0.5}),
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
            iv::event_input("trigger", iv::EventTypeId::trigger, iv::InputAccess::sequential),
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
            iv::event_output("trigger", iv::EventTypeId::trigger, iv::OutputProduction::tick, {.max_events_per_index = 0.5}),
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
            iv::event_input("trigger", iv::EventTypeId::trigger, iv::InputAccess::sequential),
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
            iv::event_input("trigger", iv::EventTypeId::trigger, iv::InputAccess::sequential),
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
        return std::array<iv::TickOutputConfig<iv::Sample>, 1>{};
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

IV_NODE("iv.test.graph_jit.state_context.stateful", StatefulProbe);
IV_NODE("iv.test.graph_jit.state_context.state_only", StateOnlyProbe);
IV_NODE("iv.test.graph_jit.state_context.background_only", BackgroundOnlyProbe);
IV_NODE("iv.test.graph_jit.state_context.background_evaluation_call", BackgroundExecutionProbe);
IV_NODE("iv.test.graph_jit.state_context.stateless", StatelessProbe);
IV_NODE("iv.test.graph_jit.state_context.configured", ConfiguredProbe);
IV_NODE(
    "iv.test.graph_jit.state_context.message_replacement",
    MessageReplacementProbe);
IV_NODE(
    "iv.test.graph_jit.state_context.message_successor",
    MessageSuccessorProbe);
IV_NODE("iv.test.graph_jit.state_context.pointer_configured", PointerConfiguredProbe);
IV_NODE("iv.test.graph_jit.state_context.limited_block", LimitedBlockProbe);
IV_NODE("iv.test.graph_jit.state_context.sample_ramp_source", SampleRampSource);
IV_NODE("iv.test.graph_jit.state_context.revising_sample_source", RevisingSampleSource);
IV_NODE("iv.test.graph_jit.state_context.tick_capture_source", TickCaptureSource);
IV_NODE("iv.test.graph_jit.state_context.persistent_revising_sample_source", PersistentRevisingSampleSource);
IV_NODE("iv.test.graph_jit.state_context.tick_fallback_sample_source", TickFallbackSampleSource);
IV_NODE("iv.test.graph_jit.state_context.limited_sample_ramp_source", LimitedSampleRampSource);
IV_NODE("iv.test.graph_jit.state_context.sample_consumer", SampleConsumerProbe);
IV_NODE("iv.test.graph_jit.state_context.sample_feedback_a", SampleFeedbackA);
IV_NODE("iv.test.graph_jit.state_context.sample_feedback_b", SampleFeedbackB);
IV_NODE("iv.test.graph_jit.state_context.multi_branch_sample_feedback", MultiBranchSampleFeedback);
IV_NODE("iv.test.graph_jit.state_context.temporal_sample_feedback", TemporalSampleFeedback);
IV_NODE("iv.test.graph_jit.state_context.revising_sample_feedback", RevisingSampleFeedback);
IV_NODE("iv.test.graph_jit.state_context.projected_revising_sample_feedback", ProjectedRevisingSampleFeedback);
IV_NODE("iv.test.graph_jit.state_context.converted_sample_feedback", ConvertedSampleFeedback);
IV_NODE("iv.test.graph_jit.state_context.mono_interleaved_consumer", MonoInterleavedConsumerProbe);
IV_NODE("iv.test.graph_jit.state_context.stereo_ramp_source", StereoRampSource);
IV_NODE("iv.test.graph_jit.state_context.stereo_planar_consumer", StereoPlanarConsumerProbe);
IV_NODE("iv.test.graph_jit.state_context.stereo_sample_consumer", StereoSampleConsumerProbe);
IV_NODE("iv.test.graph_jit.state_context.history_ramp_source", HistoryRampSource);
IV_NODE("iv.test.graph_jit.state_context.history_consumer", HistoryConsumerProbe);
IV_NODE("iv.test.graph_jit.state_context.stereo_history_consumer", StereoHistoryConsumerProbe);
IV_NODE("iv.test.graph_jit.state_context.large_history_ramp_source", LargeHistoryRampSource);
IV_NODE("iv.test.graph_jit.state_context.large_history_consumer", LargeHistoryConsumerProbe);
IV_NODE("iv.test.graph_jit.state_context.five_sample_delay", FiveSampleDelay);
IV_NODE("iv.test.graph_jit.state_context.latency_compensation_probe", LatencyCompensationProbe);
IV_NODE("iv.test.graph_jit.state_context.interleaved_latency_compensation_probe", InterleavedLatencyCompensationProbe);
IV_NODE("iv.test.graph_jit.state_context.disconnected_sample_input_probe", DisconnectedSampleInputProbe);
IV_NODE("iv.test.graph_jit.state_context.manual_control_slice_probe", ManualControlSliceProbe);
IV_NODE("iv.test.graph_jit.state_context.trigger_event_source", TriggerEventSource);
IV_NODE("iv.test.graph_jit.state_context.midi_event_source", MidiEventSource);
IV_NODE("iv.test.graph_jit.state_context.limited_trigger_event_source", LimitedTriggerEventSource);
IV_NODE("iv.test.graph_jit.state_context.fan_in_burst_event_source", FanInBurstEventSource);
IV_NODE("iv.test.graph_jit.state_context.fan_in_sparse_event_source", FanInSparseEventSource);
IV_NODE("iv.test.graph_jit.state_context.event_feedback_a", EventFeedbackA);
IV_NODE("iv.test.graph_jit.state_context.retained_fan_in_event_feedback_a", RetainedFanInEventFeedbackA);
IV_NODE("iv.test.graph_jit.state_context.latent_event_feedback_a", LatentEventFeedbackA);
IV_NODE("iv.test.graph_jit.state_context.persistent_latent_event_feedback_a", PersistentLatentEventFeedbackA);
IV_NODE("iv.test.graph_jit.state_context.persistent_latent_boundary_event_feedback_a", PersistentLatentBoundaryEventFeedbackA);
IV_NODE("iv.test.graph_jit.state_context.event_feedback_b", EventFeedbackB);
IV_NODE("iv.test.graph_jit.state_context.boundary_event_feedback_a", BoundaryEventFeedbackA);
IV_NODE("iv.test.graph_jit.state_context.boundary_event_feedback_b", BoundaryEventFeedbackB);
IV_NODE("iv.test.graph_jit.state_context.event_feedback_burst_a", EventFeedbackBurstA);
IV_NODE("iv.test.graph_jit.state_context.event_feedback_fanout_a", EventFeedbackFanoutA);
IV_NODE("iv.test.graph_jit.state_context.retained_dual_event_feedback_a", RetainedDualEventFeedbackA);
IV_NODE("iv.test.graph_jit.state_context.dual_event_feedback_b", DualEventFeedbackB);
IV_NODE("iv.test.graph_jit.state_context.trigger_event_consumer", TriggerEventConsumer);
IV_NODE("iv.test.graph_jit.state_context.limited_trigger_event_consumer", LimitedTriggerEventConsumer);
IV_NODE("iv.test.graph_jit.state_context.retained_trigger_event_source", RetainedTriggerEventSource);
IV_NODE("iv.test.graph_jit.state_context.retained_trigger_event_consumer", RetainedTriggerEventConsumer);
IV_NODE("iv.test.graph_jit.state_context.persistent_event_ring_source", PersistentEventRingSource);
IV_NODE("iv.test.graph_jit.state_context.persistent_event_ring_consumer", PersistentEventRingConsumer);
IV_NODE("iv.test.graph_jit.state_context.persistent_event_feedback_consumer", PersistentEventFeedbackConsumer);
IV_NODE("iv.test.graph_jit.state_context.retained_midi_event_consumer", RetainedMidiEventConsumer);
IV_NODE("iv.test.graph_jit.state_context.ported", PortedProbe);
IV_MODULE("iv.test.graph_jit.state_context.stateful_module", stateful_module);
IV_MODULE("iv.test.graph_jit.state_context.state_only_module", state_only_module);
IV_MODULE("iv.test.graph_jit.state_context.background_only_module", background_only_module);
IV_MODULE("iv.test.graph_jit.state_context.background_execution_module", background_execution_module);
IV_MODULE("iv.test.graph_jit.state_context.stateless_module", stateless_module);
IV_MODULE("iv.test.graph_jit.state_context.configured_module", configured_module);
IV_MODULE("iv.test.graph_jit.state_context.pointer_configured_module", pointer_configured_module);
IV_MODULE("iv.test.graph_jit.state_context.multiple_module", multiple_module);
IV_MODULE("iv.test.graph_jit.state_context.skippable_pair_module", skippable_pair_module);
IV_MODULE("iv.test.graph_jit.state_context.limited_block_module", limited_block_module);
IV_MODULE("iv.test.graph_jit.state_context.disconnected_sample_input_module", disconnected_sample_input_module);
IV_MODULE("iv.test.graph_jit.state_context.manual_control_slice_module", manual_control_slice_module);
IV_MODULE("iv.test.graph_jit.state_context.disconnected_sample_output_module", disconnected_sample_output_module);
IV_MODULE("iv.test.graph_jit.state_context.tick_capture_module", tick_capture_module);
IV_MODULE("iv.test.graph_jit.state_context.direct_sample_module", direct_sample_module);
IV_MODULE("iv.test.graph_jit.state_context.transient_sample_module", transient_sample_module);
IV_MODULE("iv.test.graph_jit.state_context.reused_sample_arena_module", reused_sample_arena_module);
IV_MODULE("iv.test.graph_jit.state_context.sample_fanout_conversion_module", sample_fanout_conversion_module);
IV_MODULE("iv.test.graph_jit.state_context.sample_revision_module", sample_revision_module);
IV_MODULE("iv.test.graph_jit.state_context.persistent_sample_revision_module", persistent_sample_revision_module);
IV_MODULE("iv.test.graph_jit.state_context.composed_sample_revision_module", composed_sample_revision_module);
IV_MODULE("iv.test.graph_jit.state_context.tick_fallback_sample_module", tick_fallback_sample_module);
IV_MODULE("iv.test.graph_jit.state_context.stereo_conversion_module", stereo_conversion_module);
IV_MODULE("iv.test.graph_jit.state_context.stereo_history_conversion_module", stereo_history_conversion_module);
IV_MODULE("iv.test.graph_jit.state_context.history_fanout_module", history_fanout_module);
IV_MODULE("iv.test.graph_jit.state_context.persistent_history_module", persistent_history_module);
IV_MODULE(
    "iv.test.graph_jit.state_context.large_history_rewire_module",
    large_history_rewire_module);
IV_MODULE("iv.test.graph_jit.state_context.latency_compensation_module", latency_compensation_module);
IV_MODULE("iv.test.graph_jit.state_context.latency_conversion_fanout_module", latency_conversion_fanout_module);
IV_MODULE("iv.test.graph_jit.state_context.composed_latency_module", composed_latency_module);
IV_MODULE("iv.test.graph_jit.state_context.composed_history_module", composed_history_module);
IV_MODULE("iv.test.graph_jit.state_context.projected_composition_module", projected_composition_module);
IV_MODULE("iv.test.graph_jit.state_context.direct_event_module", direct_event_module);
IV_MODULE("iv.test.graph_jit.state_context.transient_event_module", transient_event_module);
IV_MODULE("iv.test.graph_jit.state_context.converted_event_fanout_module", converted_event_fanout_module);
IV_MODULE("iv.test.graph_jit.state_context.retained_event_module", retained_event_module);
IV_MODULE("iv.test.graph_jit.state_context.persistent_event_ring_module", persistent_event_ring_module);
IV_MODULE("iv.test.graph_jit.state_context.retained_converted_event_fanout_module", retained_converted_event_fanout_module);
IV_MODULE("iv.test.graph_jit.state_context.ported_module", ported_module);
)cpp";
    static constexpr std::string_view part_3 = R"cpp(
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
            "out", iv::OutputProduction::tick)};
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
    static constexpr bool intrinsically_replayable = true;

    constexpr explicit NativeBatchReplaySource(std::uint64_t id_) : id(id_) {}

    static constexpr auto inputs() { return std::tuple{}; }
    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>(
            "out", iv::OutputProduction::tick)};
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

    struct TockState {
        std::uint64_t tock_batch_calls = 0;
        std::uint64_t last_lane_count = 0;
        std::uint64_t configured_id = 0;
        std::uint64_t sample_rate = 0;
    };

    constexpr explicit NativeBatchBackgroundProbe(std::uint64_t id_) : id(id_) {}

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>(
            "in", iv::InputAccess::coverage)};
    }
    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>(
            "out", iv::OutputProduction::coverage)};
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
            auto& state = lane.tock_state();
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

    struct TockState {
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
            "out", iv::OutputProduction::coverage)};
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
            auto& state = lane.tock_state();
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
            "out", iv::OutputProduction::tick)};
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
            iv::input<iv::Sample>("first", iv::InputAccess::sequential),
            iv::input<iv::Sample>("second", iv::InputAccess::sequential),
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
            "out", iv::OutputProduction::tick)};
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
            "in", iv::InputAccess::sequential)};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>(
            "out", iv::OutputProduction::tick)};
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
            "in", iv::InputAccess::sequential)};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>(
            "out", iv::OutputProduction::tick)};
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
            "out",
            iv::EventTypeId::trigger,
            iv::OutputProduction::tick,
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
            "in", iv::EventTypeId::trigger, iv::InputAccess::sequential)};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::event_output(
            "out",
            iv::EventTypeId::trigger,
            iv::OutputProduction::tick,
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

struct DeclarationLayoutLeaf {
    struct State {
        std::uint64_t marker = 17;
    };

    struct TockState {
        std::uint64_t marker = 23;
    };
};

struct DeclarationLayoutParent {
    struct State {
        std::span<std::uint64_t> local;
        std::span<std::span<std::byte>> nested;
        std::uint64_t calls = 0;
        std::uint64_t local_sum = 0;
        std::uint64_t nested_sum = 0;
    };

    static constexpr auto inputs() { return std::tuple{}; }
    static constexpr auto outputs() { return std::tuple{}; }

    void declare(iv::DeclarationContext<DeclarationLayoutParent> const& ctx) const
    {
        ctx.local_array<&State::local>(3);
        DeclarationLayoutLeaf first;
        DeclarationLayoutLeaf second;
        do_declare(first, ctx, {});
        do_declare(second, ctx, {});
        ctx.nested_node_states<&State::nested>();
    }

    void initialize(iv::InitializationContext<DeclarationLayoutParent> const& ctx) const
    {
        auto& state = ctx.state();
        state.local[0] = 2;
        state.local[1] = 3;
        state.local[2] = 5;
    }

    void tick_block(iv::TickBlockContext<DeclarationLayoutParent> const& ctx) const
    {
        auto& state = ctx.state();
        ++state.calls;
        state.local_sum = state.local[0] + state.local[1] + state.local[2];
        state.nested_sum = 0;
        for (auto const child : state.nested) {
            state.nested_sum +=
                reinterpret_cast<DeclarationLayoutLeaf::State const*>(
                    child.data())->marker;
        }
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
            iv::input<iv::Sample>("samples", iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{
            iv::output<iv::FFTBlock>("spectra", iv::OutputProduction::tick),
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
                "spectra", iv::InputAccess::sequential),
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
            "coordinates", iv::OutputProduction::tick)};
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
            "coordinates", iv::InputAccess::sequential)};
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
            "spectra",
            iv::OutputProduction::tick,
            iv::OutputRetention::persisted)};
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
            "samples",
            iv::OutputProduction::tick,
            iv::OutputRetention::persisted)};
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
    (void)graph.node<"iv.test.graph_jit.state_context.declaration_layout_parent">();
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

struct MultiRegionCoverageSource {
    static constexpr auto inputs() { return std::tuple{}; }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>(
            "multi-region-source-output",
            iv::OutputProduction::coverage)};
    }

    static constexpr auto regions()
    {
        return std::array{
            iv::GlobalIndexRange::from_begin_and_end(
                iv::GlobalIndex::from_signed(10),
                iv::GlobalIndex::from_signed(14)),
            iv::GlobalIndexRange::from_begin_and_end(
                iv::GlobalIndex::from_signed(12),
                iv::GlobalIndex::from_signed(16)),
        };
    }

    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<MultiRegionCoverageSource>& ctx) const
    {
        auto const published = regions();
        auto output = ctx.output<"multi-region-source-output">();
        output.publish(published);
        for (auto const region : published) output.change(region);
    }

    void tock_coverage(
        iv::TockCoverageContext<MultiRegionCoverageSource>& ctx) const
    {
        auto output = ctx.output<"multi-region-source-output">().regions();
        IV_ASSERT(output.size() == 2,
            "multi-Region source did not receive two output Regions");
        for (std::size_t region = 0; region < output.size(); ++region) {
            for (std::size_t frame = 0;
                 frame < output[region].frame_count(); ++frame) {
                output[region].value(frame) =
                    iv::Sample{static_cast<float>(region + 1)};
            }
        }
    }
};

struct MultiRegionCoverageConsumer {
    struct alignas(64) TockState {
        std::uint64_t calls = 0;
        std::uint64_t region_count = 0;
        std::uint64_t first_begin = 0;
        std::uint64_t second_begin = 0;
        float first_overlap_value = 0.0f;
        float second_overlap_value = 0.0f;
    };

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>(
            "multi-region-coverage-input",
            iv::InputAccess::coverage)};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>(
            "multi-region-consumer-output",
            iv::OutputProduction::coverage)};
    }

    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<MultiRegionCoverageConsumer>& ctx) const
    {
        auto input = ctx.input<"multi-region-coverage-input">();
        auto output = ctx.output<"multi-region-consumer-output">();
        output.publish(input.regions());
        output.change(input.changed());
    }

    void propagate_reverse_coverage(
        iv::PropagateReverseCoverageContext<MultiRegionCoverageConsumer>& ctx) const
    {
        ctx.input<"multi-region-coverage-input">().require(
            ctx.output<"multi-region-consumer-output">().required());
    }

    void tock_coverage(
        iv::TockCoverageContext<MultiRegionCoverageConsumer>& ctx) const
    {
        auto input = ctx.input<"multi-region-coverage-input">().regions();
        auto output = ctx.output<"multi-region-consumer-output">().regions();
        IV_ASSERT(input.size() == 2 && output.size() == input.size(),
            "multi-Region consumer did not receive two matching Region lists");

        auto& state = ctx.tock_state();
        ++state.calls;
        state.region_count = input.size();
        state.first_begin = input[0].range().begin.bits();
        state.second_begin = input[1].range().begin.bits();
        auto const overlap = iv::GlobalIndex::from_signed(12);
        state.first_overlap_value = static_cast<float>(input[0].at(overlap));
        state.second_overlap_value = static_cast<float>(input[1].at(overlap));

        for (std::size_t region = 0; region < input.size(); ++region) {
            IV_ASSERT(input[region].range() == output[region].range(),
                "multi-Region consumer input and output ranges differ");
            for (std::size_t frame = 0;
                 frame < input[region].frame_count(); ++frame) {
                output[region].value(frame) = input[region].value(frame);
            }
        }
    }
};

struct MultiRegionSequentialConsumer {
    std::uint8_t configuration_marker = 0;

    struct alignas(64) State {
        std::uint64_t calls = 0;
        std::uint64_t sample_count = 0;
        std::array<float, 6> values{};
    };

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>(
            "multi-region-sequential-input",
            iv::InputAccess::sequential)};
    }

    static constexpr auto outputs() { return std::tuple{}; }

    void tick_block(
        iv::TickBlockContext<MultiRegionSequentialConsumer> const& ctx) const
    {
        auto const input = ctx.input<"multi-region-sequential-input">().block();
        auto& state = ctx.state();
        ++state.calls;
        state.sample_count = input.size();
        auto const count = std::min(input.size(), state.values.size());
        for (std::size_t i = 0; i < count; ++i) {
            state.values[i] = static_cast<float>(input[i]);
        }
    }
};

struct MultiRegionEventCoverageSource {
    static constexpr auto inputs() { return std::tuple{}; }

    static constexpr auto outputs()
    {
        return std::array{iv::event_output(
            "multi-region-event-source-output",
            iv::EventTypeId::trigger,
            iv::OutputProduction::coverage,
            iv::EventOutputOptions{.max_events_per_index = 1.0})};
    }

    static constexpr auto regions()
    {
        return std::array{
            iv::GlobalIndexRange::from_begin_and_end(
                iv::GlobalIndex::from_signed(10),
                iv::GlobalIndex::from_signed(14)),
            iv::GlobalIndexRange::from_begin_and_end(
                iv::GlobalIndex::from_signed(12),
                iv::GlobalIndex::from_signed(16)),
        };
    }

    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<
            MultiRegionEventCoverageSource>& ctx) const
    {
        auto const published = regions();
        auto output = ctx.output<"multi-region-event-source-output">();
        output.publish(published);
        for (auto const region : published) output.change(region);
    }

    void tock_coverage(
        iv::TockCoverageContext<MultiRegionEventCoverageSource>& ctx) const
    {
        auto output =
            ctx.output<"multi-region-event-source-output">().regions();
        IV_ASSERT(output.size() == 2,
            "multi-Region event source did not receive two output Regions");
        output[0].write({
            .time = iv::GlobalIndex::from_signed(10),
            .value = iv::TriggerEvent{},
        });
        output[0].write({
            .time = iv::GlobalIndex::from_signed(13),
            .value = iv::TriggerEvent{},
        });
        output[1].write({
            .time = iv::GlobalIndex::from_signed(12),
            .value = iv::TriggerEvent{},
        });
        output[1].write({
            .time = iv::GlobalIndex::from_signed(15),
            .value = iv::TriggerEvent{},
        });
    }
};

struct MultiRegionEventCoverageConsumer {
    struct alignas(64) TockState {
        std::uint64_t calls = 0;
        std::uint64_t region_count = 0;
        std::uint64_t first_event_count = 0;
        std::uint64_t second_event_count = 0;
        std::uint64_t first_last_time = 0;
        std::uint64_t second_first_time = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{iv::event_input(
            "multi-region-event-coverage-input",
            iv::EventTypeId::trigger,
            iv::InputAccess::coverage)};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::event_output(
            "multi-region-event-consumer-output",
            iv::EventTypeId::trigger,
            iv::OutputProduction::coverage,
            iv::EventOutputOptions{.max_events_per_index = 1.0})};
    }

    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<
            MultiRegionEventCoverageConsumer>& ctx) const
    {
        auto input = ctx.input<"multi-region-event-coverage-input">();
        auto output = ctx.output<"multi-region-event-consumer-output">();
        output.publish(input.regions());
        output.change(input.changed());
    }

    void propagate_reverse_coverage(
        iv::PropagateReverseCoverageContext<
            MultiRegionEventCoverageConsumer>& ctx) const
    {
        ctx.input<"multi-region-event-coverage-input">().require(
            ctx.output<"multi-region-event-consumer-output">().required());
    }

    void tock_coverage(
        iv::TockCoverageContext<MultiRegionEventCoverageConsumer>& ctx) const
    {
        auto input =
            ctx.input<"multi-region-event-coverage-input">().regions();
        auto output =
            ctx.output<"multi-region-event-consumer-output">().regions();
        IV_ASSERT(input.size() == 2 && output.size() == input.size(),
            "multi-Region event consumer did not receive matching Region lists");

        auto& state = ctx.tock_state();
        ++state.calls;
        state.region_count = input.size();
        for (std::size_t region = 0; region < input.size(); ++region) {
            IV_ASSERT(input[region].range() == output[region].range(),
                "multi-Region event input and output ranges differ");
            input[region].for_each([&](iv::TimedEvent const& event) {
                if (region == 0) {
                    ++state.first_event_count;
                    state.first_last_time = event.time.bits();
                } else {
                    if (state.second_event_count == 0) {
                        state.second_first_time = event.time.bits();
                    }
                    ++state.second_event_count;
                }
                output[region].write(event);
            });
        }
    }
};

struct MultiRegionSequentialEventConsumer {
    std::uint8_t configuration_marker = 0;

    struct alignas(128) State {
        std::uint64_t calls = 0;
        std::uint64_t event_count = 0;
        std::array<std::uint64_t, 5> times{};
    };

    static constexpr auto inputs()
    {
        return std::array{iv::event_input(
            "multi-region-sequential-event-input",
            iv::EventTypeId::trigger,
            iv::InputAccess::sequential)};
    }

    static constexpr auto outputs() { return std::tuple{}; }

    void tick_block(
        iv::TickBlockContext<MultiRegionSequentialEventConsumer> const& ctx)
        const
    {
        auto const events =
            ctx.input<"multi-region-sequential-event-input">().events();
        auto& state = ctx.state();
        ++state.calls;
        state.event_count = events.size();
        auto const count = std::min(events.size(), state.times.size());
        for (std::size_t event = 0; event < count; ++event) {
            state.times[event] = events[event].time.bits();
        }
    }
};

struct SingleRegionSampleSource {
    static constexpr auto inputs() { return std::tuple{}; }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>(
            "single-region-sample-source-output",
            iv::OutputProduction::region)};
    }

    static constexpr auto region()
    {
        return iv::GlobalIndexRange::from_begin_and_end(
            iv::GlobalIndex::from_signed(20),
            iv::GlobalIndex::from_signed(24));
    }

    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<SingleRegionSampleSource>& ctx) const
    {
        auto output = ctx.output<"single-region-sample-source-output">();
        output.publish(region());
        output.change(region());
    }

    void tock_coverage(
        iv::TockCoverageContext<SingleRegionSampleSource>& ctx) const
    {
        auto output = ctx.output<"single-region-sample-source-output">().region();
        IV_ASSERT(output.range() == region(),
            "single sample Region source received the wrong output range");
        for (std::size_t frame = 0; frame < output.frame_count(); ++frame) {
            output.value(frame) = iv::Sample{static_cast<float>(frame + 4)};
        }
    }
};

struct SingleRegionSampleConsumer {
    struct alignas(64) TockState {
        std::uint64_t calls = 0;
        std::uint64_t begin = 0;
        std::uint64_t extent = 0;
        float first_value = 0.0f;
        float last_value = 0.0f;
    };

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>(
            "single-region-sample-input", iv::InputAccess::region)};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>(
            "single-region-sample-consumer-output",
            iv::OutputProduction::region)};
    }

    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<SingleRegionSampleConsumer>& ctx) const
    {
        auto input = ctx.input<"single-region-sample-input">();
        auto const regions = input.regions();
        IV_ASSERT(regions.size() == 1,
            "single sample Region input did not propagate one range");
        auto output = ctx.output<"single-region-sample-consumer-output">();
        output.publish(regions.front());
        output.change(input.changed());
    }

    void propagate_reverse_coverage(
        iv::PropagateReverseCoverageContext<SingleRegionSampleConsumer>& ctx) const
    {
        ctx.input<"single-region-sample-input">().require(
            ctx.output<"single-region-sample-consumer-output">().required());
    }

    void tock_coverage(
        iv::TockCoverageContext<SingleRegionSampleConsumer>& ctx) const
    {
        auto input = ctx.input<"single-region-sample-input">().region();
        auto output = ctx.output<"single-region-sample-consumer-output">().region();
        IV_ASSERT(input.range() == output.range(),
            "single sample Region input and output ranges differ");
        auto& state = ctx.tock_state();
        ++state.calls;
        state.begin = input.range().begin.bits();
        state.extent = input.range().extent;
        state.first_value = static_cast<float>(input.value(0));
        state.last_value = static_cast<float>(
            input.value(input.frame_count() - 1));
        for (std::size_t frame = 0; frame < input.frame_count(); ++frame) {
            output.value(frame) = input.value(frame);
        }
    }
};

struct SingleRegionEventSource {
    static constexpr auto inputs() { return std::tuple{}; }

    static constexpr auto outputs()
    {
        return std::array{iv::event_output(
            "single-region-event-source-output",
            iv::EventTypeId::trigger,
            iv::OutputProduction::region,
            iv::EventOutputOptions{.max_events_per_index = 1.0})};
    }

    static constexpr auto region()
    {
        return iv::GlobalIndexRange::from_begin_and_end(
            iv::GlobalIndex::from_signed(30),
            iv::GlobalIndex::from_signed(34));
    }

    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<SingleRegionEventSource>& ctx) const
    {
        auto output = ctx.output<"single-region-event-source-output">();
        output.publish(region());
        output.change(region());
    }

    void tock_coverage(
        iv::TockCoverageContext<SingleRegionEventSource>& ctx) const
    {
        auto output = ctx.output<"single-region-event-source-output">().region();
        IV_ASSERT(output.range() == region(),
            "single event Region source received the wrong output range");
        output.write({
            .time = iv::GlobalIndex::from_signed(30),
            .value = iv::TriggerEvent{},
        });
        output.write({
            .time = iv::GlobalIndex::from_signed(33),
            .value = iv::TriggerEvent{},
        });
    }
};

struct SingleRegionEventConsumer {
    struct alignas(64) TockState {
        std::uint64_t calls = 0;
        std::uint64_t begin = 0;
        std::uint64_t extent = 0;
        std::uint64_t event_count = 0;
        std::uint64_t first_time = 0;
        std::uint64_t last_time = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{iv::event_input(
            "single-region-event-input",
            iv::EventTypeId::trigger,
            iv::InputAccess::region)};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::event_output(
            "single-region-event-consumer-output",
            iv::EventTypeId::trigger,
            iv::OutputProduction::region,
            iv::EventOutputOptions{.max_events_per_index = 1.0})};
    }

    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<SingleRegionEventConsumer>& ctx) const
    {
        auto input = ctx.input<"single-region-event-input">();
        auto const regions = input.regions();
        IV_ASSERT(regions.size() == 1,
            "single event Region input did not propagate one range");
        auto output = ctx.output<"single-region-event-consumer-output">();
        output.publish(regions.front());
        output.change(input.changed());
    }

    void propagate_reverse_coverage(
        iv::PropagateReverseCoverageContext<SingleRegionEventConsumer>& ctx) const
    {
        ctx.input<"single-region-event-input">().require(
            ctx.output<"single-region-event-consumer-output">().required());
    }

    void tock_coverage(
        iv::TockCoverageContext<SingleRegionEventConsumer>& ctx) const
    {
        auto input = ctx.input<"single-region-event-input">().region();
        auto output = ctx.output<"single-region-event-consumer-output">().region();
        IV_ASSERT(input.range() == output.range(),
            "single event Region input and output ranges differ");
        auto& state = ctx.tock_state();
        ++state.calls;
        state.begin = input.range().begin.bits();
        state.extent = input.range().extent;
        input.for_each([&](iv::TimedEvent const& event) {
            if (state.event_count == 0) state.first_time = event.time.bits();
            state.last_time = event.time.bits();
            ++state.event_count;
            output.write(event);
        });
    }
};

void multi_region_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<
        "iv.test.graph_jit.state_context.multi_region_source">();
    auto coverage_consumer = graph.node<
        "iv.test.graph_jit.state_context.multi_region_coverage_consumer">();
    auto sequential_consumer = graph.node<
        "iv.test.graph_jit.state_context.multi_region_sequential_consumer">();
    coverage_consumer(source);
    sequential_consumer(source);
    auto event_source = graph.node<
        "iv.test.graph_jit.state_context.multi_region_event_source">();
    auto event_coverage_consumer = graph.node<
        "iv.test.graph_jit.state_context.multi_region_event_coverage_consumer">();
    auto sequential_event_consumer = graph.node<
        "iv.test.graph_jit.state_context.multi_region_sequential_event_consumer">();
    event_coverage_consumer.connect_event_input(0, event_source.event_port());
    sequential_event_consumer.connect_event_input(
        0, event_source.event_port());
    auto single_sample_source = graph.node<
        "iv.test.graph_jit.state_context.single_region_sample_source">();
    auto single_sample_consumer = graph.node<
        "iv.test.graph_jit.state_context.single_region_sample_consumer">();
    single_sample_consumer(single_sample_source);
    auto single_event_source = graph.node<
        "iv.test.graph_jit.state_context.single_region_event_source">();
    auto single_event_consumer = graph.node<
        "iv.test.graph_jit.state_context.single_region_event_consumer">();
    single_event_consumer.connect_event_input(
        0, single_event_source.event_port());
    graph.outputs();
}

void native_batch_tick_module(iv::GraphBuilder& graph)
{
    (void)graph.node<"iv.test.graph_jit.state_context.native_batch_tick">(
        std::uint64_t{11});
    (void)graph.node<"iv.test.graph_jit.state_context.native_batch_tick">(
        std::uint64_t{22});
    graph.outputs();
}

void native_batch_aliasing_module(iv::GraphBuilder& graph)
{
    (void)graph.node<"iv.test.graph_jit.state_context.native_batch_aliasing">(
        std::uint64_t{11});
    (void)graph.node<"iv.test.graph_jit.state_context.native_batch_aliasing">(
        std::uint64_t{22});
    graph.outputs();
}

void native_batch_replay_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<
        "iv.test.graph_jit.state_context.native_batch_replay">(
        std::uint64_t{11});
    auto consumer = graph.node<
        "iv.test.graph_jit.state_context.native_batch_background">(
        std::uint64_t{22});
    consumer(source);
    graph.outputs();
}

void native_batch_activity_module(iv::GraphBuilder& graph)
{
    auto first = graph.node<
        "iv.test.graph_jit.state_context.native_batch_activity">();
    first.ttl(64);
    auto second = graph.node<
        "iv.test.graph_jit.state_context.native_batch_activity">();
    second.ttl(128);
    // Keeping both outputs live until one consumer prevents transient
    // storage sharing from forcing scalar fallback.
    auto sink = graph.node<
        "iv.test.graph_jit.state_context.native_batch_activity_sink">();
    sink(first, second);
    graph.outputs();
}

void native_batch_background_module(iv::GraphBuilder& graph)
{
    // These probes have no inputs, so this module isolates forward propagation
    // and Tock batching from Random Access input coverage.
    (void)graph.node<
        "iv.test.graph_jit.state_context.native_batch_tock">(
        std::uint64_t{11});
    (void)graph.node<
        "iv.test.graph_jit.state_context.native_batch_tock">(
        std::uint64_t{22});
    graph.outputs();
}

void native_batch_reverse_module(iv::GraphBuilder& graph)
{
    auto first_source = graph.node<
        "iv.test.graph_jit.state_context.background_evaluation_call">();
    auto second_source = graph.node<
        "iv.test.graph_jit.state_context.background_evaluation_call">();
    auto first = graph.node<
        "iv.test.graph_jit.state_context.native_batch_background">(
        std::uint64_t{11});
    auto second = graph.node<
        "iv.test.graph_jit.state_context.native_batch_background">(
        std::uint64_t{22});
    first(first_source);
    second(second_source);
    graph.outputs();
}

} // namespace

IV_NODE("iv.test.graph_jit.state_context.native_batch_tick", NativeBatchTickProbe);
IV_NODE(
    "iv.test.graph_jit.state_context.native_batch_aliasing",
    NativeBatchAliasingProbe);
IV_NODE(
    "iv.test.graph_jit.state_context.native_batch_replay",
    NativeBatchReplaySource);
IV_NODE(
    "iv.test.graph_jit.state_context.native_batch_background",
    NativeBatchBackgroundProbe);
IV_NODE(
    "iv.test.graph_jit.state_context.native_batch_tock",
    NativeBatchTockProbe);
IV_NODE(
    "iv.test.graph_jit.state_context.native_batch_activity",
    NativeBatchActivityProbe);
IV_NODE(
    "iv.test.graph_jit.state_context.native_batch_activity_sink",
    NativeBatchActivitySink);
IV_NODE("iv.test.graph_jit.state_context.activity_step_source", ActivityStepSource);
IV_NODE("iv.test.graph_jit.state_context.activity_probe", ActivityProbe);
IV_NODE("iv.test.graph_jit.state_context.activity_type_default_probe", ActivityTypeDefaultProbe);
IV_NODE("iv.test.graph_jit.state_context.event_activity_step_source", EventActivityStepSource);
IV_NODE("iv.test.graph_jit.state_context.event_activity_probe", EventActivityProbe);
IV_NODE("iv.test.graph_jit.state_context.declaration_layout_parent", DeclarationLayoutParent);
IV_NODE("iv.test.graph_jit.state_context.declaration_layout_producer", DeclarationLayoutProducer);
IV_NODE("iv.test.graph_jit.state_context.declaration_layout_consumer", DeclarationLayoutConsumer);
IV_NODE("iv.test.graph_jit.state_context.fft_value_producer", FFTValueProducer);
IV_NODE("iv.test.graph_jit.state_context.fft_value_consumer", FFTValueConsumer);
IV_NODE("iv.test.graph_jit.state_context.fractional_index_producer", FractionalIndexProducer);
IV_NODE("iv.test.graph_jit.state_context.fractional_index_consumer", FractionalIndexConsumer);
IV_NODE("iv.test.graph_jit.state_context.persisted_fft_value_producer", PersistedFFTValueProducer);
IV_NODE("iv.test.graph_jit.state_context.persisted_non_project_rate_sample_producer", PersistedNonProjectRateSampleProducer);
IV_NODE(
    "iv.test.graph_jit.state_context.multi_region_source",
    MultiRegionCoverageSource);
IV_NODE(
    "iv.test.graph_jit.state_context.multi_region_coverage_consumer",
    MultiRegionCoverageConsumer);
IV_NODE(
    "iv.test.graph_jit.state_context.multi_region_sequential_consumer",
    MultiRegionSequentialConsumer);
IV_NODE(
    "iv.test.graph_jit.state_context.multi_region_event_source",
    MultiRegionEventCoverageSource);
IV_NODE(
    "iv.test.graph_jit.state_context.multi_region_event_coverage_consumer",
    MultiRegionEventCoverageConsumer);
IV_NODE(
    "iv.test.graph_jit.state_context.multi_region_sequential_event_consumer",
    MultiRegionSequentialEventConsumer);
IV_NODE(
    "iv.test.graph_jit.state_context.single_region_sample_source",
    SingleRegionSampleSource);
IV_NODE(
    "iv.test.graph_jit.state_context.single_region_sample_consumer",
    SingleRegionSampleConsumer);
IV_NODE(
    "iv.test.graph_jit.state_context.single_region_event_source",
    SingleRegionEventSource);
IV_NODE(
    "iv.test.graph_jit.state_context.single_region_event_consumer",
    SingleRegionEventConsumer);
IV_MODULE("iv.test.graph_jit.state_context.activity_module", activity_module);
IV_MODULE("iv.test.graph_jit.state_context.activity_disabled_module", activity_disabled_module);
IV_MODULE("iv.test.graph_jit.state_context.activity_type_default_module", activity_type_default_module);
IV_MODULE("iv.test.graph_jit.state_context.event_activity_module", event_activity_module);
IV_MODULE("iv.test.graph_jit.state_context.declaration_layout_module", declaration_layout_module);
IV_MODULE("iv.test.graph_jit.state_context.fft_value_module", fft_value_module);
IV_MODULE("iv.test.graph_jit.state_context.fractional_index_value_module", fractional_index_value_module);
IV_MODULE("iv.test.graph_jit.state_context.persisted_fft_value_module", persisted_fft_value_module);
IV_MODULE("iv.test.graph_jit.state_context.persisted_non_project_rate_sample_module", persisted_non_project_rate_sample_module);
IV_MODULE(
    "iv.test.graph_jit.state_context.native_batch_tick_module",
    native_batch_tick_module);
IV_MODULE(
    "iv.test.graph_jit.state_context.native_batch_aliasing_module",
    native_batch_aliasing_module);
IV_MODULE(
    "iv.test.graph_jit.state_context.native_batch_replay_module",
    native_batch_replay_module);
IV_MODULE(
    "iv.test.graph_jit.state_context.native_batch_background_module",
    native_batch_background_module);
IV_MODULE(
    "iv.test.graph_jit.state_context.native_batch_activity_module",
    native_batch_activity_module);
IV_MODULE(
    "iv.test.graph_jit.state_context.native_batch_reverse_module",
    native_batch_reverse_module);
IV_MODULE(
    "iv.test.graph_jit.state_context.multi_region_module",
    multi_region_module);

namespace {
// stereo_ramp_source offset by 100000.
struct OffsetStereoRampSource {
    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::tick,
            {.channel_layout = {
                .channel_type = iv::ChannelTypeId::stereo,
                .sample_layout = iv::SampleStreamLayout::interleaved,
            }})};
    }

    void tick_block(iv::TickBlockContext<OffsetStereoRampSource> const& ctx) const
    {
        auto output = ctx.output<"out">();
        for (std::size_t i = 0; i < ctx.block_size; ++i) {
            auto const position = static_cast<float>(
                ctx.index.advanced_by({.whole = i}).whole().signed_value());
            output.value(i, 0) = iv::Sample{position + 100000.0f};
            output.value(i, 1) = iv::Sample{position + 101000.0f};
        }
    }
};

// The probe's interleaved mono input downmixes the stereo ramp, compensated by
// 7 for its slower input, which reads the delay node. `rewired` downmixes the
// offset stereo ramp instead.
void downmixed_rewire_module(iv::GraphBuilder& graph, bool rewired = false)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.stereo_ramp_source">();
    auto offset_source =
        graph.node<"iv.test.graph_jit.state_context.offset_stereo_ramp_source">();
    auto ramp = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto delayed = graph.node<"iv.test.graph_jit.state_context.five_sample_delay">();
    auto probe =
        graph.node<"iv.test.graph_jit.state_context.converted_sequence_probe">();
    delayed(ramp);
    auto const in = rewired
        ? static_cast<iv::SamplePortRef>(offset_source)
        : static_cast<iv::SamplePortRef>(source);
    probe(in, delayed);
    graph.outputs();
}

// The probe's interleaved mono input downmixes a stereo tile of the ramp and a
// second ramp, or of the ramp and the offset ramp when `rewired`, compensated
// by 7 for its slower input, which reads the delay node.
void downmixed_tile_rewire_module(iv::GraphBuilder& graph, bool rewired = false)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto other = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto offset_source =
        graph.node<"iv.test.graph_jit.state_context.offset_sample_ramp_source">();
    auto delayed = graph.node<"iv.test.graph_jit.state_context.five_sample_delay">();
    auto probe =
        graph.node<"iv.test.graph_jit.state_context.converted_sequence_probe">();
    delayed(source);
    auto const right = rewired
        ? static_cast<iv::SamplePortRef>(offset_source)
        : static_cast<iv::SamplePortRef>(other);
    probe(graph.tile<iv::stereo>(source, right), delayed);
    graph.outputs();
}

// The probe's interleaved mono input downmixes the stereo ramp. Its slower
// input reads a ramp, or the delay node when `delayed_slower`, which
// compensates the downmixed input by 7. A history reader keeps the stereo
// ramp's recent values.
void downmixed_compensation_module(
    iv::GraphBuilder& graph,
    bool delayed_slower = false)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.stereo_ramp_source">();
    auto ramp = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto delayed = graph.node<"iv.test.graph_jit.state_context.five_sample_delay">();
    auto history_reader =
        graph.node<"iv.test.graph_jit.state_context.large_history_consumer">();
    auto probe =
        graph.node<"iv.test.graph_jit.state_context.converted_sequence_probe">();
    delayed(ramp);
    history_reader(source);
    auto const slower = delayed_slower
        ? static_cast<iv::SamplePortRef>(delayed)
        : static_cast<iv::SamplePortRef>(ramp);
    probe(source, slower);
    graph.outputs();
}

// The probe's interleaved mono input downmixes a stereo tile of the ramp and a
// second ramp. Its slower input reads the second ramp, or the delay node when
// `delayed_slower`, which compensates the downmixed input by 7. History
// readers keep both ramps' recent values.
void downmixed_tile_compensation_module(
    iv::GraphBuilder& graph,
    bool delayed_slower = false)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto other = graph.node<"iv.test.graph_jit.state_context.sample_ramp_source">();
    auto delayed = graph.node<"iv.test.graph_jit.state_context.five_sample_delay">();
    auto source_history =
        graph.node<"iv.test.graph_jit.state_context.large_history_consumer">();
    auto other_history =
        graph.node<"iv.test.graph_jit.state_context.large_history_consumer">();
    auto probe =
        graph.node<"iv.test.graph_jit.state_context.converted_sequence_probe">();
    delayed(source);
    source_history(source);
    other_history(other);
    auto const slower = delayed_slower
        ? static_cast<iv::SamplePortRef>(delayed)
        : static_cast<iv::SamplePortRef>(other);
    probe(graph.tile<iv::stereo>(source, other), slower);
    graph.outputs();
}

// neutral_latency_module, with a history reader that keeps the ramp's recent
// values in storage.
void retained_neutral_latency_module(
    iv::GraphBuilder& graph,
    std::size_t latency = 0)
{
    auto source = graph.node<
        "iv.test.graph_jit.state_context.latency_argument_ramp_source">(latency);
    auto history_reader =
        graph.node<"iv.test.graph_jit.state_context.history_consumer">();
    auto probe =
        graph.node<"iv.test.graph_jit.state_context.neutral_sequence_probe">();
    history_reader(source);
    probe(source);
    graph.outputs();
}
} // namespace

IV_NODE(
    "iv.test.graph_jit.state_context.offset_stereo_ramp_source",
    OffsetStereoRampSource);
IV_MODULE(
    "iv.test.graph_jit.state_context.downmixed_rewire_module",
    downmixed_rewire_module);
IV_MODULE(
    "iv.test.graph_jit.state_context.downmixed_tile_rewire_module",
    downmixed_tile_rewire_module);
IV_MODULE(
    "iv.test.graph_jit.state_context.downmixed_compensation_module",
    downmixed_compensation_module);
IV_MODULE(
    "iv.test.graph_jit.state_context.downmixed_tile_compensation_module",
    downmixed_tile_compensation_module);
IV_MODULE(
    "iv.test.graph_jit.state_context.retained_neutral_latency_module",
    retained_neutral_latency_module);
)cpp";
    static constexpr std::string_view part_4 = R"cpp(
namespace {
// Background-evaluated sources whose playback the tests supply directly as a
// TickMaterializationSnapshot. Their own evaluation is covered elsewhere.
struct PlaybackSampleSource {
    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>(
            "out", iv::OutputProduction::coverage)};
    }

    void tock_coverage(iv::TockCoverageContext<PlaybackSampleSource>&) const {}
    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<PlaybackSampleSource>&) const {}
};

struct PlaybackTriggerSource {
    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::event_output(
            "out",
            iv::EventTypeId::trigger,
            iv::OutputProduction::coverage,
            {.max_events_per_index = 0.25})};
    }

    void tock_coverage(iv::TockCoverageContext<PlaybackTriggerSource>&) const {}
    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<PlaybackTriggerSource>&) const {}
};

// Records its complete visible history window: window[k] = get(4 - k).
struct PlaybackHistoryConsumer {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t last_index = 0;
        std::array<float, 5> window{};
    };

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>("in", iv::InputAccess::sequential)};
    }

    void constrain_ports(
        iv::ConstrainPortsContext<PlaybackHistoryConsumer>& ctx) const
    {
        ctx.input<"in">().history() = 4;
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(
        iv::TickBlockContext<PlaybackHistoryConsumer> const& ctx) const
    {
        auto& state = ctx.state();
        ++state.calls;
        state.last_index = ctx.index.whole().bits();
        for (std::size_t k = 0; k < state.window.size(); ++k) {
            state.window[k] = static_cast<float>(
                ctx.template input<0>().get(state.window.size() - 1 - k));
        }
    }
};

// A second consumer type, so a test can tell two consumers apart by state
// size.
struct PlaybackHistoryPeer {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t last_index = 0;
        std::array<float, 5> window{};
        std::uint64_t marker = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>("in", iv::InputAccess::sequential)};
    }

    void constrain_ports(iv::ConstrainPortsContext<PlaybackHistoryPeer>& ctx) const
    {
        ctx.input<"in">().history() = 4;
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(iv::TickBlockContext<PlaybackHistoryPeer> const& ctx) const
    {
        auto& state = ctx.state();
        ++state.calls;
        state.last_index = ctx.index.whole().bits();
        for (std::size_t k = 0; k < state.window.size(); ++k) {
            state.window[k] = static_cast<float>(
                ctx.template input<0>().get(state.window.size() - 1 - k));
        }
        state.marker = 0x91e3u;
    }
};

// left[k] and right[k] = get(2 - k, channel).
struct PlaybackStereoHistoryConsumer {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t last_index = 0;
        std::array<float, 3> left{};
        std::array<float, 3> right{};
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
        iv::ConstrainPortsContext<PlaybackStereoHistoryConsumer>& ctx) const
    {
        ctx.input<"in">().history() = 2;
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(
        iv::TickBlockContext<PlaybackStereoHistoryConsumer> const& ctx) const
    {
        auto& state = ctx.state();
        ++state.calls;
        state.last_index = ctx.index.whole().bits();
        for (std::size_t k = 0; k < state.left.size(); ++k) {
            auto const history = state.left.size() - 1 - k;
            state.left[k] = static_cast<float>(
                ctx.template input<0>().get(history, 0));
            state.right[k] = static_cast<float>(
                ctx.template input<0>().get(history, 1));
        }
    }
};

// Records the event times of its window [index - 32, index + block).
struct PlaybackEventHistoryConsumer {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t count = 0;
        std::array<std::uint64_t, 8> times{};
    };

    static constexpr auto inputs()
    {
        return std::array{
            iv::event_input("trigger", iv::EventTypeId::trigger, iv::InputAccess::sequential),
        };
    }

    void constrain_ports(
        iv::ConstrainPortsContext<PlaybackEventHistoryConsumer>& ctx) const
    {
        ctx.input<"trigger">().history() = 32;
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(
        iv::TickBlockContext<PlaybackEventHistoryConsumer> const& ctx) const
    {
        auto& state = ctx.state();
        ++state.calls;
        constexpr std::size_t history = 32;
        auto const events = ctx.template input<0>().events(
            ctx.index.retreated_by({.whole = history}).whole(),
            ctx.block_size + history);
        state.count = events.size();
        for (std::size_t i = 0; i < state.times.size(); ++i) {
            state.times[i] = i < events.size() ? events[i].time.bits() : 0;
        }
    }
};

struct PlaybackEventHistoryPeer {
    struct State {
        std::uint64_t calls = 0;
        std::uint64_t count = 0;
        std::array<std::uint64_t, 8> times{};
        std::uint64_t marker = 0;
    };

    static constexpr auto inputs()
    {
        return std::array{
            iv::event_input("trigger", iv::EventTypeId::trigger, iv::InputAccess::sequential),
        };
    }

    void constrain_ports(
        iv::ConstrainPortsContext<PlaybackEventHistoryPeer>& ctx) const
    {
        ctx.input<"trigger">().history() = 32;
    }

    static constexpr auto outputs()
    {
        return std::tuple{};
    }

    void tick_block(
        iv::TickBlockContext<PlaybackEventHistoryPeer> const& ctx) const
    {
        auto& state = ctx.state();
        ++state.calls;
        constexpr std::size_t history = 32;
        auto const events = ctx.template input<0>().events(
            ctx.index.retreated_by({.whole = history}).whole(),
            ctx.block_size + history);
        state.count = events.size();
        for (std::size_t i = 0; i < state.times.size(); ++i) {
            state.times[i] = i < events.size() ? events[i].time.bits() : 0;
        }
        state.marker = 0x91e4u;
    }
};

struct ModuleBoundaryCoverageObserver {
    struct alignas(64) TockState {
        std::uint64_t calls = 0;
        std::uint64_t sample_region_count = 0;
        std::uint64_t event_region_count = 0;
        std::uint64_t first_event_count = 0;
        std::uint64_t second_event_count = 0;
        float first_overlap_value = 0.0f;
        float second_overlap_value = 0.0f;
    };

    static constexpr auto inputs()
    {
        return std::tuple{
            iv::input<iv::Sample>(
                "module-boundary-sample-input",
                iv::InputAccess::coverage),
            iv::event_input(
                "module-boundary-event-input",
                iv::EventTypeId::trigger,
                iv::InputAccess::coverage),
        };
    }

    static constexpr auto outputs()
    {
        return std::tuple{
            iv::output<iv::Sample>(
                "module-boundary-sample-output",
                iv::OutputProduction::coverage),
            iv::event_output(
                "module-boundary-event-output",
                iv::EventTypeId::trigger,
                iv::OutputProduction::coverage,
                {.max_events_per_index = 1.0}),
        };
    }

    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<
            ModuleBoundaryCoverageObserver>& ctx) const
    {
        auto samples = ctx.input<"module-boundary-sample-input">();
        ctx.output<"module-boundary-sample-output">().publish(
            samples.regions());
        ctx.output<"module-boundary-sample-output">().change(
            samples.changed());
        auto events = ctx.input<"module-boundary-event-input">();
        ctx.output<"module-boundary-event-output">().publish(
            events.regions());
        ctx.output<"module-boundary-event-output">().change(
            events.changed());
    }

    void propagate_reverse_coverage(
        iv::PropagateReverseCoverageContext<
            ModuleBoundaryCoverageObserver>& ctx) const
    {
        ctx.input<"module-boundary-sample-input">().require(
            ctx.output<"module-boundary-sample-output">().required());
        ctx.input<"module-boundary-event-input">().require(
            ctx.output<"module-boundary-event-output">().required());
    }

    void tock_coverage(
        iv::TockCoverageContext<ModuleBoundaryCoverageObserver>& ctx) const
    {
        auto samples = ctx.input<"module-boundary-sample-input">().regions();
        auto sample_outputs =
            ctx.output<"module-boundary-sample-output">().regions();
        auto events = ctx.input<"module-boundary-event-input">().regions();
        auto event_outputs =
            ctx.output<"module-boundary-event-output">().regions();
        IV_ASSERT(samples.size() == 2 && sample_outputs.size() == 2,
            "module boundary did not preserve two sample Regions");
        IV_ASSERT(events.size() == 2 && event_outputs.size() == 2,
            "module boundary did not preserve two event Regions");

        auto& state = ctx.tock_state();
        ++state.calls;
        state.sample_region_count = samples.size();
        state.event_region_count = events.size();
        auto const overlap = iv::GlobalIndex::from_signed(12);
        state.first_overlap_value = static_cast<float>(samples[0].at(overlap));
        state.second_overlap_value = static_cast<float>(samples[1].at(overlap));
        events[0].for_each([&](iv::TimedEvent const&) {
            ++state.first_event_count;
        });
        events[1].for_each([&](iv::TimedEvent const&) {
            ++state.second_event_count;
        });

        for (std::size_t region = 0; region < samples.size(); ++region) {
            for (std::size_t frame = 0;
                 frame < samples[region].frame_count(); ++frame) {
                sample_outputs[region].value(frame) =
                    samples[region].value(frame);
            }
            events[region].for_each([&](iv::TimedEvent const& event) {
                event_outputs[region].write(event);
            });
        }
    }
};

void multi_region_boundary_child_module(iv::GraphBuilder& graph)
{
    auto sample_input = graph.input<"samples", iv::Sample>();
    auto event_input = graph.event_input<"events">(iv::EventTypeId::trigger);
    auto sample = graph.node<
        "iv.test.graph_jit.state_context.multi_region_coverage_consumer">();
    auto events = graph.node<
        "iv.test.graph_jit.state_context.multi_region_event_coverage_consumer">();
    sample(sample_input);
    events.connect_event_input(0, event_input);
    graph.outputs(sample);
    graph.event_outputs(events.event_port());
}

struct PersistedStereoRecordingSource {
    static constexpr auto inputs() { return std::tuple{}; }

    static constexpr auto outputs()
    {
        return std::tuple{
            iv::output<iv::Sample>(
                "recorded-samples",
                iv::OutputProduction::tick,
                iv::OutputRetention::persisted,
                {.channel_layout = {
                    .channel_type = iv::ChannelTypeId::stereo,
                    .sample_layout = iv::SampleStreamLayout::planar,
                }}),
            iv::event_output(
                "recorded-events",
                iv::EventTypeId::trigger,
                iv::OutputProduction::tick,
                iv::OutputRetention::persisted,
                {.max_events_per_index = 0.25}),
        };
    }

    void tick_block(
        iv::TickBlockContext<PersistedStereoRecordingSource> const& ctx) const
    {
        auto samples = ctx.output<"recorded-samples">();
        for (std::size_t frame = 0; frame < ctx.block_size; ++frame) {
            auto const position = ctx.index.advanced_by({.whole = frame}).whole();
            samples.value(frame, 0) =
                iv::Sample{static_cast<float>(position.signed_value())};
            samples.value(frame, 1) = iv::Sample{
                1000.0f + static_cast<float>(position.signed_value())};
        }
        if (ctx.block_size > 2) {
            ctx.output<"recorded-events">().push(iv::TriggerEvent{}, 2);
        }
    }
};

struct PersistedRecordingCoverageConsumer {
    struct alignas(64) TockState {
        std::uint64_t calls = 0;
        std::uint64_t sample_region_count = 0;
        std::uint64_t sample_layout = 0;
        std::uint64_t event_region_count = 0;
        std::uint64_t event_count = 0;
        std::uint64_t first_event_time = 0;
        float first_left = 0.0f;
        float first_right = 0.0f;
        float last_left = 0.0f;
        float last_right = 0.0f;
    };

    static constexpr auto inputs()
    {
        return std::tuple{
            iv::input<iv::Sample>(
                "recorded-sample-coverage",
                iv::InputAccess::coverage,
                {.channel_layout = {
                    .channel_type = iv::ChannelTypeId::stereo,
                    .sample_layout = iv::SampleStreamLayout::planar,
                }}),
            iv::event_input(
                "recorded-event-coverage",
                iv::EventTypeId::trigger,
                iv::InputAccess::coverage),
        };
    }

    static constexpr auto outputs()
    {
        return std::tuple{
            iv::output<iv::Sample>(
                "observed-sample-coverage",
                iv::OutputProduction::coverage,
                {.channel_layout = {
                    .channel_type = iv::ChannelTypeId::stereo,
                    .sample_layout = iv::SampleStreamLayout::planar,
                }}),
            iv::event_output(
                "observed-event-coverage",
                iv::EventTypeId::trigger,
                iv::OutputProduction::coverage,
                {.max_events_per_index = 0.25}),
        };
    }

    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<
            PersistedRecordingCoverageConsumer>& ctx) const
    {
        auto samples = ctx.input<"recorded-sample-coverage">();
        ctx.output<"observed-sample-coverage">().publish(samples.regions());
        ctx.output<"observed-sample-coverage">().change(samples.changed());
        auto events = ctx.input<"recorded-event-coverage">();
        ctx.output<"observed-event-coverage">().publish(events.regions());
        ctx.output<"observed-event-coverage">().change(events.changed());
    }

    void propagate_reverse_coverage(
        iv::PropagateReverseCoverageContext<
            PersistedRecordingCoverageConsumer>& ctx) const
    {
        ctx.input<"recorded-sample-coverage">().require(
            ctx.output<"observed-sample-coverage">().required());
        ctx.input<"recorded-event-coverage">().require(
            ctx.output<"observed-event-coverage">().required());
    }

    void tock_coverage(
        iv::TockCoverageContext<PersistedRecordingCoverageConsumer>& ctx) const
    {
        auto sample_inputs =
            ctx.input<"recorded-sample-coverage">().regions();
        auto sample_outputs =
            ctx.output<"observed-sample-coverage">().regions();
        auto event_inputs =
            ctx.input<"recorded-event-coverage">().regions();
        auto event_outputs =
            ctx.output<"observed-event-coverage">().regions();
        IV_ASSERT(sample_inputs.size() == sample_outputs.size(),
            "recorded sample input and output Region counts differ");
        IV_ASSERT(event_inputs.size() == event_outputs.size(),
            "recorded event input and output Region counts differ");

        auto& state = ctx.tock_state();
        ++state.calls;
        state.sample_region_count = sample_inputs.size();
        state.event_region_count = event_inputs.size();
        if (!sample_inputs.empty()) {
            auto const first = sample_inputs[0];
            auto const last = sample_inputs[sample_inputs.size() - 1];
            state.sample_layout = static_cast<std::uint64_t>(
                first.channel_layout().sample_layout);
            state.first_left = static_cast<float>(first.value(0, 0));
            state.first_right = static_cast<float>(first.value(0, 1));
            state.last_left = static_cast<float>(
                last.value(last.frame_count() - 1, 0));
            state.last_right = static_cast<float>(
                last.value(last.frame_count() - 1, 1));
        }
        for (auto const region : event_inputs) {
            region.for_each([&](iv::TimedEvent const& event) {
                if (state.event_count == 0) {
                    state.first_event_time = event.time.bits();
                }
                ++state.event_count;
            });
        }

        for (std::size_t region = 0; region < sample_inputs.size(); ++region) {
            for (std::size_t frame = 0;
                 frame < sample_inputs[region].frame_count(); ++frame) {
                sample_outputs[region].value(frame, 0) =
                    sample_inputs[region].value(frame, 0);
                sample_outputs[region].value(frame, 1) =
                    sample_inputs[region].value(frame, 1);
            }
        }
        for (std::size_t region = 0; region < event_inputs.size(); ++region) {
            event_inputs[region].for_each(
                [&](iv::TimedEvent const& event) {
                    event_outputs[region].write(event);
                });
        }
    }
};

void persisted_recording_coverage_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<
        "iv.test.graph_jit.state_context.persisted_stereo_recording_source">();
    auto consumer = graph.node<
        "iv.test.graph_jit.state_context.persisted_recording_coverage_consumer">();
    consumer(source);
    consumer.connect_event_input(0, source.event_port());

    auto sample_source = graph.node<
        "iv.test.graph_jit.state_context.multi_region_source">();
    auto event_source = graph.node<
        "iv.test.graph_jit.state_context.multi_region_event_source">();
    auto child = graph.node<
        "iv.test.graph_jit.state_context.multi_region_boundary_child_module">();
    child(sample_source);
    child.connect_event_input(0, event_source.event_port());
    auto observer = graph.node<
        "iv.test.graph_jit.state_context.module_boundary_coverage_observer">();
    observer(child);
    observer.connect_event_input(0, child.event_port());
    graph.outputs();
}

void playback_sample_history_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.playback_sample_source">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.playback_history_consumer">();
    sink(source);
    graph.outputs();
}

// This replacement has the same node and connection shape as
// playback_sample_history_module, but its module definition gives the
// ConfiguredGraph a different identity. The event replacement below does the
// same for playback_event_history_module. They exercise reuse of the same
// generation-local bundle/port coordinates by different semantic nodes.
void playback_sample_history_replacement_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.playback_sample_source">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.playback_history_consumer">();
    sink(source);
    graph.outputs();
}

void playback_sample_window_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.playback_sample_source">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.sample_consumer">();
    sink(source);
    graph.outputs();
}

void playback_mixed_sample_module(iv::GraphBuilder& graph)
{
    auto realtime = graph.node<"iv.test.graph_jit.state_context.large_history_ramp_source">();
    auto background = graph.node<"iv.test.graph_jit.state_context.playback_sample_source">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.playback_stereo_history_consumer">();
    sink(graph.tile<iv::stereo>(realtime, background));
    graph.outputs();
}

void playback_event_history_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.playback_trigger_source">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.playback_event_history_consumer">();
    sink.connect_event_input(0, source.event_port());
    graph.outputs();
}

void playback_event_history_replacement_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.playback_trigger_source">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.playback_event_history_consumer">();
    sink.connect_event_input(0, source.event_port());
    graph.outputs();
}

void playback_mixed_event_module(iv::GraphBuilder& graph)
{
    auto realtime = graph.node<"iv.test.graph_jit.state_context.trigger_event_source">();
    auto background = graph.node<"iv.test.graph_jit.state_context.playback_trigger_source">();
    auto sink = graph.node<"iv.test.graph_jit.state_context.playback_event_history_consumer">();
    std::array ports{realtime.event_port(), background.event_port()};
    sink.connect_event_input(0, graph.make_event_port(ports));
    graph.outputs();
}

// The consumer fed by the realtime source and the consumer fed by playback
// exchange sources in the rewired successor.
void playback_sample_rewire_module(iv::GraphBuilder& graph)
{
    auto realtime = graph.node<"iv.test.graph_jit.state_context.large_history_ramp_source">();
    auto background = graph.node<"iv.test.graph_jit.state_context.playback_sample_source">();
    auto realtime_fed = graph.node<"iv.test.graph_jit.state_context.playback_history_consumer">();
    auto background_fed = graph.node<"iv.test.graph_jit.state_context.playback_history_peer">();
    realtime_fed(realtime);
    background_fed(background);
    graph.outputs();
}

void playback_event_rewire_module(iv::GraphBuilder& graph)
{
    auto realtime = graph.node<"iv.test.graph_jit.state_context.trigger_event_source">();
    auto background = graph.node<"iv.test.graph_jit.state_context.playback_trigger_source">();
    auto realtime_fed = graph.node<"iv.test.graph_jit.state_context.playback_event_history_consumer">();
    auto background_fed = graph.node<"iv.test.graph_jit.state_context.playback_event_history_peer">();
    realtime_fed.connect_event_input(0, realtime.event_port());
    background_fed.connect_event_input(0, background.event_port());
    graph.outputs();
}

// Authors a latent Tick sample output whose newest values stay provisional
// until its next tick revises them through OutputPort::update(). Before
// authoring, it checks every value of its owned window, the last
// history + latency values before this tick, against the value it authored or
// revised for that position. The newest min(latency, block size) values of
// each block are provisional, so the fixture runs equal-sized blocks only.
template<class State, class Output>
void author_latent_window(
    State& state,
    Output const& output,
    std::int64_t begin,
    std::size_t block_size)
{
    auto const final_value = [](std::int64_t position) {
        return static_cast<float>(position);
    };
    auto const provisional_value = [](std::int64_t position) {
        return -1.0f - static_cast<float>(position);
    };
    auto const window = output.history() + output.latency();
    auto const pending = std::min(output.latency(), block_size);
    ++state.calls;
    state.checked = 0;
    state.mismatches = 0;
    for (std::size_t offset = 0; offset < window; ++offset) {
        auto const position = begin - 1 - static_cast<std::int64_t>(offset);
        if (position < 0) break;
        auto const expected = offset < pending
            ? provisional_value(position)
            : final_value(position);
        ++state.checked;
        if (static_cast<float>(output.get(offset)) != expected) {
            ++state.mismatches;
        }
    }
    for (std::size_t offset = 0; offset < pending; ++offset) {
        auto const position = begin - 1 - static_cast<std::int64_t>(offset);
        if (position < 0) break;
        output.update(iv::Sample{final_value(position)}, offset);
    }
    for (std::size_t offset = 0; offset < block_size; ++offset) {
        auto const position = begin + static_cast<std::int64_t>(offset);
        output.push(iv::Sample{offset + pending >= block_size
            ? provisional_value(position)
            : final_value(position)});
    }
}

struct LatentWindowSource {
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
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::tick)};
    }

    void constrain_ports(
        iv::ConstrainPortsContext<LatentWindowSource>& ctx) const
    {
        ctx.output<"out">().history() = 3;
        ctx.output<"out">().latency() = 2;
    }

    void tick_block(iv::TickBlockContext<LatentWindowSource> const& ctx) const
    {
        author_latent_window(
            ctx.state(),
            ctx.output<"out">(),
            ctx.index.whole().signed_value(),
            ctx.block_size);
    }
};

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
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::tick)};
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
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::tick)};
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
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::tick)};
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
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::tick)};
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

IV_NODE("iv.test.graph_jit.state_context.playback_sample_source", PlaybackSampleSource);
IV_NODE("iv.test.graph_jit.state_context.playback_trigger_source", PlaybackTriggerSource);
IV_NODE("iv.test.graph_jit.state_context.playback_history_consumer", PlaybackHistoryConsumer);
IV_NODE("iv.test.graph_jit.state_context.playback_history_peer", PlaybackHistoryPeer);
IV_NODE("iv.test.graph_jit.state_context.playback_stereo_history_consumer", PlaybackStereoHistoryConsumer);
IV_NODE("iv.test.graph_jit.state_context.playback_event_history_consumer", PlaybackEventHistoryConsumer);
IV_NODE("iv.test.graph_jit.state_context.playback_event_history_peer", PlaybackEventHistoryPeer);
IV_NODE(
    "iv.test.graph_jit.state_context.module_boundary_coverage_observer",
    ModuleBoundaryCoverageObserver);
IV_MODULE(
    "iv.test.graph_jit.state_context.multi_region_boundary_child_module",
    multi_region_boundary_child_module);
IV_NODE(
    "iv.test.graph_jit.state_context.persisted_stereo_recording_source",
    PersistedStereoRecordingSource);
IV_NODE(
    "iv.test.graph_jit.state_context.persisted_recording_coverage_consumer",
    PersistedRecordingCoverageConsumer);
IV_MODULE(
    "iv.test.graph_jit.state_context.persisted_recording_coverage_module",
    persisted_recording_coverage_module);
IV_MODULE("iv.test.graph_jit.state_context.playback_sample_history_module", playback_sample_history_module);
IV_MODULE("iv.test.graph_jit.state_context.playback_sample_history_replacement_module", playback_sample_history_replacement_module);
IV_MODULE("iv.test.graph_jit.state_context.playback_sample_window_module", playback_sample_window_module);
IV_MODULE("iv.test.graph_jit.state_context.playback_mixed_sample_module", playback_mixed_sample_module);
IV_MODULE("iv.test.graph_jit.state_context.playback_event_history_module", playback_event_history_module);
IV_MODULE("iv.test.graph_jit.state_context.playback_event_history_replacement_module", playback_event_history_replacement_module);
IV_MODULE("iv.test.graph_jit.state_context.playback_mixed_event_module", playback_mixed_event_module);
IV_MODULE("iv.test.graph_jit.state_context.playback_sample_rewire_module", playback_sample_rewire_module);
IV_MODULE("iv.test.graph_jit.state_context.playback_event_rewire_module", playback_event_rewire_module);
IV_NODE("iv.test.graph_jit.state_context.latent_window_source", LatentWindowSource);
IV_NODE("iv.test.graph_jit.state_context.large_latent_window_source", LargeLatentWindowSource);
IV_NODE("iv.test.graph_jit.state_context.latent_window_consumer", LatentWindowConsumer);
IV_MODULE("iv.test.graph_jit.state_context.latent_window_compact_carry_module", latent_window_compact_carry_module);
IV_MODULE("iv.test.graph_jit.state_context.latent_window_ring_module", latent_window_ring_module);
IV_MODULE("iv.test.graph_jit.state_context.latent_window_consumer_compact_carry_module", latent_window_consumer_compact_carry_module);
IV_MODULE("iv.test.graph_jit.state_context.latent_window_consumer_ring_module", latent_window_consumer_ring_module);
IV_NODE("iv.test.graph_jit.state_context.compensated_ramp_probe", CompensatedRampProbe);
IV_NODE("iv.test.graph_jit.state_context.compensated_ramp_history_probe", CompensatedRampHistoryProbe);
IV_MODULE("iv.test.graph_jit.state_context.compensated_ramp_second_reader_module", compensated_ramp_second_reader_module);
IV_MODULE("iv.test.graph_jit.state_context.compensated_ramp_history_reader_module", compensated_ramp_history_reader_module);
IV_MODULE("iv.test.graph_jit.state_context.compensated_ramp_history_probe_module", compensated_ramp_history_probe_module);
IV_NODE("iv.test.graph_jit.state_context.moving_state_probe", MovingStateProbe);
IV_MODULE("iv.test.graph_jit.state_context.moving_state_module", moving_state_module);
IV_NODE("iv.test.graph_jit.state_context.offset_sample_ramp_source", OffsetSampleRampSource);
IV_NODE("iv.test.graph_jit.state_context.compensated_sequence_probe", CompensatedSequenceProbe);
IV_MODULE("iv.test.graph_jit.state_context.compensated_sequence_module", compensated_sequence_module);
IV_MODULE("iv.test.graph_jit.state_context.compensated_sequence_rewire_module", compensated_sequence_rewire_module);
IV_MODULE("iv.test.graph_jit.state_context.compensated_latent_history_probe_module", compensated_latent_history_probe_module);
IV_MODULE("iv.test.graph_jit.state_context.compensated_converted_history_reader_module", compensated_converted_history_reader_module);
IV_NODE("iv.test.graph_jit.state_context.sample_sequence_probe", SampleSequenceProbe);
IV_NODE("iv.test.graph_jit.state_context.planar_stereo_alignment_probe", PlanarStereoAlignmentProbe);
IV_NODE("iv.test.graph_jit.state_context.interleaved_stereo_alignment_probe", InterleavedStereoAlignmentProbe);
IV_NODE("iv.test.graph_jit.state_context.feedback_counter", FeedbackCounter);
IV_NODE("iv.test.graph_jit.state_context.latency_argument_ramp_source", LatencyArgumentRampSource);
IV_MODULE("iv.test.graph_jit.state_context.compensated_planar_composition_module", compensated_planar_composition_module);
IV_MODULE("iv.test.graph_jit.state_context.compensated_interleaved_composition_module", compensated_interleaved_composition_module);
IV_MODULE("iv.test.graph_jit.state_context.feedback_counter_module", feedback_counter_module);
IV_MODULE("iv.test.graph_jit.state_context.long_compensation_module", long_compensation_module);
IV_MODULE("iv.test.graph_jit.state_context.leading_node_module", leading_node_module);
IV_MODULE("iv.test.graph_jit.state_context.source_latency_module", source_latency_module);
IV_MODULE("iv.test.graph_jit.state_context.routed_feedback_module", routed_feedback_module);
IV_NODE("iv.test.graph_jit.state_context.neutral_feedback_counter", NeutralFeedbackCounter);
IV_NODE("iv.test.graph_jit.state_context.routed_history_probe", RoutedHistoryProbe);
IV_MODULE("iv.test.graph_jit.state_context.neutral_feedback_counter_module", neutral_feedback_counter_module);
IV_MODULE("iv.test.graph_jit.state_context.routed_rewire_module", routed_rewire_module);
IV_NODE("iv.test.graph_jit.state_context.interleaved_left_probe", InterleavedLeftProbe);
IV_NODE("iv.test.graph_jit.state_context.converted_sequence_probe", ConvertedSequenceProbe);
IV_NODE("iv.test.graph_jit.state_context.neutral_sequence_probe", NeutralSequenceProbe);
IV_NODE("iv.test.graph_jit.state_context.neutral_compensated_sequence_probe", NeutralCompensatedSequenceProbe);
IV_MODULE("iv.test.graph_jit.state_context.tiled_rewire_module", tiled_rewire_module);
IV_MODULE("iv.test.graph_jit.state_context.interleaved_mono_rewire_module", interleaved_mono_rewire_module);
IV_MODULE("iv.test.graph_jit.state_context.neutral_latency_module", neutral_latency_module);
IV_MODULE("iv.test.graph_jit.state_context.neutral_compensated_module", neutral_compensated_module);
IV_MODULE("iv.test.graph_jit.state_context.connected_input_module", connected_input_module);
IV_MODULE("iv.test.graph_jit.state_context.tiled_compensation_module", tiled_compensation_module);
IV_MODULE("iv.test.graph_jit.state_context.detach_feedback_module", detach_feedback_module);

)cpp";
    static constexpr std::string_view part_5 = R"cpp(
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
            "in", iv::InputAccess::sequential)};
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>(
            "out", iv::OutputProduction::tick)};
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
    NativeBatchFeedbackProbe);
IV_MODULE(
    "iv.test.graph_jit.state_context.native_batch_feedback_module",
    native_batch_feedback_module);

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
IV_MODULE("iv.test.graph_jit.state_context.added_event_consumer_module", added_event_consumer_module);

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
            iv::input<iv::Sample>("audio", iv::InputAccess::sequential),
            iv::event_input("trigger", iv::EventTypeId::trigger, iv::InputAccess::sequential),
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
IV_NODE("iv.test.graph_jit.state_context.compensated_trigger_probe", CompensatedTriggerProbe);

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
IV_MODULE("iv.test.graph_jit.state_context.compensated_trigger_module", compensated_trigger_module);

// A trigger source drives a subgraph's event input, which feeds the probe,
// whose sample input reads a ramp of latency 7, and a consumer (history 8)
// without other inputs: one authored connection with two targets, which need
// compensation 7 and 0 for the same source.
void routed_unequal_compensation_module(iv::GraphBuilder& graph)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.trigger_event_source">();
    auto routed = graph.subgraph([&](iv::SubgraphBuilder& boundary) {
        auto trigger = boundary.event_input<"trigger">(iv::EventTypeId::trigger);
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
IV_MODULE("iv.test.graph_jit.state_context.routed_unequal_compensation_module", routed_unequal_compensation_module);

// A trigger output of history 8 and the latency its argument gives. Each
// block authors one trigger just before itself, in its latency window, one at
// 5, and one 2 positions before its end, which is not final when it ends.
struct LatentTriggerSource {
    std::size_t latency = 1;
    explicit LatentTriggerSource(std::size_t latency_ = 1) : latency(latency_) {}
    static constexpr auto inputs() { return std::tuple{}; }
    static constexpr auto outputs()
    {
        return std::array{iv::event_output("trigger", iv::EventTypeId::trigger, iv::OutputProduction::tick, {.max_events_per_index = 0.5})};
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
IV_NODE("iv.test.graph_jit.state_context.latent_trigger_source", LatentTriggerSource);

// The latent source read by a consumer of history 8, which reads the
// source's own retained stream: the source keeps at least that history.
void latent_direct_event_module(iv::GraphBuilder& graph, std::size_t latency = 4)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.latent_trigger_source">(latency);
    auto sink = graph.node<"iv.test.graph_jit.state_context.retained_trigger_event_consumer">();
    sink.connect_event_input(0, source.event_port());
    graph.outputs();
}
IV_MODULE("iv.test.graph_jit.state_context.latent_direct_event_module", latent_direct_event_module);

// A persisted sample output at pace 2 with latency 1 (one value). Each value is
// first written as its value index and revised, while the next value is
// written, to 1000 plus its value index.
struct RevisingPacedCaptureSource {
    static constexpr auto inputs() { return std::tuple{}; }
    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("samples", iv::OutputProduction::tick, iv::OutputRetention::persisted)};
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
IV_NODE("iv.test.graph_jit.state_context.revising_paced_capture_source", RevisingPacedCaptureSource);
void revising_paced_capture_module(iv::GraphBuilder& graph)
{
    (void)graph.node<"iv.test.graph_jit.state_context.revising_paced_capture_source">();
    graph.outputs();
}
IV_MODULE("iv.test.graph_jit.state_context.revising_paced_capture_module", revising_paced_capture_module);

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
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::tick)};
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
IV_NODE("iv.test.graph_jit.state_context.latent_window_history_source", LatentWindowHistorySource);

// The source read by LatentWindowConsumer (history 4), which reads its ring.
void latent_window_growth_module(iv::GraphBuilder& graph, std::size_t history = 3)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.latent_window_history_source">(history);
    auto consumer = graph.node<"iv.test.graph_jit.state_context.latent_window_consumer">();
    consumer(source);
    graph.outputs();
}
IV_MODULE("iv.test.graph_jit.state_context.latent_window_growth_module", latent_window_growth_module);

// A ramp (value = position) whose input only closes a feedback loop.
struct LoopRampSource {
    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>("in", iv::InputAccess::sequential)};
    }
    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::tick)};
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
IV_NODE("iv.test.graph_jit.state_context.loop_ramp_source", LoopRampSource);

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
            iv::input<iv::Sample>("ramp", iv::InputAccess::sequential),
            iv::input<iv::Sample>("loop", iv::InputAccess::sequential),
        };
    }
    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::tick)};
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
IV_NODE("iv.test.graph_jit.state_context.aligned_pair_passthrough", AlignedPairPassthrough);

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
IV_MODULE("iv.test.graph_jit.state_context.scc_compensation_module", scc_compensation_module);

// Persisted Tick outputs of latency 1. Each sample is first written as its
// position and then revised, during the next position, to 1000 + its position.
// Each block adds a trigger at its last position and one in its latency window,
// the position before the block.
struct RevisingCaptureSource {
    static constexpr auto inputs() { return std::tuple{}; }
    static constexpr auto outputs()
    {
        return std::tuple{
            iv::output<iv::Sample>("samples", iv::OutputProduction::tick, iv::OutputRetention::persisted),
            iv::event_output("events", iv::EventTypeId::trigger, iv::OutputProduction::tick, iv::OutputRetention::persisted, {.max_events_per_index = 0.25}),
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
IV_NODE("iv.test.graph_jit.state_context.revising_capture_source", RevisingCaptureSource);
void revising_capture_module(iv::GraphBuilder& graph)
{
    (void)graph.node<"iv.test.graph_jit.state_context.revising_capture_source">();
    graph.outputs();
}
IV_MODULE("iv.test.graph_jit.state_context.revising_capture_module", revising_capture_module);
IV_NODE("iv.test.graph_jit.state_context.constant", iv::Constant);
// A Constant read by a history reader. `raised` selects its value.
void constant_history_module(iv::GraphBuilder& graph, bool raised = false)
{
    auto source = graph.node<"iv.test.graph_jit.state_context.constant">(
        iv::Sample{raised ? 0.75f : 0.25f});
    auto sink = graph.node<"iv.test.graph_jit.state_context.history_consumer">();
    sink(source);
    graph.outputs();
}
IV_MODULE("iv.test.graph_jit.state_context.constant_history_module", constant_history_module);

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
            iv::input<iv::Sample>("in", iv::InputAccess::sequential),
            iv::input<iv::Sample>("gain", iv::InputAccess::sequential),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::tick)};
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
IV_NODE("iv.test.graph_jit.state_context.feedback_gain", FeedbackGain);

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
    constant_gain_feedback_module);

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
            iv::input<iv::Sample>("in"),
            iv::input<iv::Sample>("gain"),
            iv::input<iv::Sample>("offset", {
                .default_value = iv::Sample{0.125f},
            }),
        };
    }

    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("out")};
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
IV_NODE("iv.test.graph_jit.state_context.immutable_history_reader", ImmutableHistoryReader);

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
    immutable_history_module);

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
    distinct_immutable_history_module);
)cpp";
    static_assert(part_1.size() < 60 * 1024);
    static_assert(part_2.size() < 60 * 1024);
    static_assert(part_3.size() < 60 * 1024);
    static_assert(part_4.size() < 60 * 1024);
    static_assert(part_5.size() < 60 * 1024);
    static std::string const source = [] {
        std::string result;
        result.reserve(
            part_1.size() + part_2.size() + part_3.size() + part_4.size()
            + part_5.size());
        result.append(part_1);
        result.append(part_2);
        result.append(part_3);
        result.append(part_4);
        result.append(part_5);
        return result;
    }();
    return std::string_view{source};
}

void write_graph_jit_fixture_file_if_changed(
    std::filesystem::path const& path,
    std::string_view contents)
{
    if (std::filesystem::exists(path)
        && iv::test::read_text(path) == contents) {
        return;
    }
    iv::test::write_text(path, std::string(contents));
}

std::filesystem::path ensure_graph_jit_runtime_workspace()
{
    auto const root = iv::test::shared_test_fixtures_root();
    auto const token = iv::test::sanitize_test_token(graph_jit_runtime_fixture_name);
    auto const workspace = root / token;
    auto const lock = iv::test::ScopedFileLock(root / (token + ".lock"));
    std::filesystem::create_directories(workspace);
    if (!std::filesystem::exists(workspace / "iv_project.jsonl")) {
        iv::test::write_text(workspace / "iv_project.jsonl", "");
    }
    write_graph_jit_fixture_file_if_changed(
        workspace / "iv_package.json",
        "{\n  \"schema\": 2,\n  \"entry\": \"module.cpp\"\n}\n");
    write_graph_jit_fixture_file_if_changed(
        workspace / "module.cpp",
        graph_jit_runtime_package_source());
    return workspace;
}

} // namespace

std::shared_ptr<iv::PackageRevision const> load_graph_jit_runtime_revision()
{
    auto const workspace = std::filesystem::weakly_canonical(
        ensure_graph_jit_runtime_workspace());
    iv::StartupConfig startup_config(workspace, iv::test::repo_root(), {});
    iv::PackageJit package_jit(startup_config.initialize());
    iv::PackageJitBatchRequest request{
        .declarations = {iv::IvPackageDeclaration{
            .package_id = graph_jit_state_package_id,
            .package_root = workspace,
        }},
    };
    package_jit.handle_build_request(request);
    if (!request.result.failed.empty()) {
        throw std::runtime_error(request.result.failed.front().message);
    }
    if (request.result.revisions.size() != 1u) {
        throw std::runtime_error("GraphJit shared runtime fixture did not produce one package revision");
    }
    return std::make_shared<iv::PackageRevision const>(
        std::move(request.result.revisions.front()));
}

} // namespace iv::test::graph_jit_tests
