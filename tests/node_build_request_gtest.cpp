#include <intravenous/graph/reflected_node_description.h>
#include <intravenous/node/coverage_port_context.h>
#include <intravenous/node/tick.h>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

struct RequestNode {
    std::size_t latency = 0;

    static constexpr auto inputs()
    {
        return std::array{iv::sequential_sample_input("level")};
    }
    static constexpr auto outputs()
    {
        return std::array{iv::tick_sample_output(
            "signal", {}, {}, iv::OutputRetention::persisted)};
    }
    std::size_t internal_latency() const { return latency; }
    std::optional<std::size_t> ttl_samples() const { return 64; }
    bool can_skip_block() const { return true; }
    void tick_block(auto const&) const {}
};

struct ReplayableNode {
    static constexpr bool intrinsically_replayable = true;
    std::size_t latency = 0;

    static constexpr auto inputs()
    {
        return std::array{iv::sequential_sample_input("input")};
    }
    static constexpr auto outputs()
    {
        return std::array{iv::tick_sample_output("output")};
    }
    void tick(iv::TickSampleContext<ReplayableNode> const&) const {}
    std::size_t internal_latency() const { return latency; }
};

struct ReplayWithState {
    static constexpr bool intrinsically_replayable = true;
    struct State {};
    void tick(iv::TickSampleContext<ReplayWithState> const&) const {}
};

struct ReplayWithInputHistory {
    static constexpr bool intrinsically_replayable = true;
    static constexpr auto inputs()
    {
        return std::array{iv::sequential_sample_input(
            "input", {}, {.history = 1})};
    }
    void tick(iv::TickSampleContext<ReplayWithInputHistory> const&) const {}
};

struct ReplayWithOutputLatency {
    static constexpr bool intrinsically_replayable = true;
    static constexpr auto outputs()
    {
        return std::array{iv::tick_sample_output(
            "output", {}, {.latency = 1})};
    }
    void tick(iv::TickSampleContext<ReplayWithOutputLatency> const&) const {}
};

static_assert(iv::details::replay_declaration_is_valid_v<ReplayableNode>);
static_assert(!iv::details::replay_declaration_is_valid_v<ReplayWithState>);
static_assert(!iv::details::replay_declaration_is_valid_v<ReplayWithInputHistory>);
static_assert(!iv::details::replay_declaration_is_valid_v<ReplayWithOutputLatency>);

TEST(NodeBuildRequest, ValidatesAndReflectsIntrinsicReplayability)
{
    ReplayableNode node{};
    auto request = iv::details::make_node_build_request(node);
    ASSERT_NE(request.compiler_record, nullptr);
    EXPECT_TRUE(request.compiler_record->intrinsically_replayable);
    auto storage = iv::details::copy_node_config_bytes(
        request.config, request.config_size, request.config_alignment);
    auto description = iv::details::materialize_node_description(
        request, std::move(storage));
    EXPECT_TRUE(description.intrinsically_replayable);
    EXPECT_EQ(description.internal_latency_samples, 0u);
    EXPECT_TRUE(iv::is_tick(description.outputs().front()));

    ReplayableNode delayed{.latency = 1};
    auto delayed_request = iv::details::make_node_build_request(delayed);
    auto delayed_storage = iv::details::copy_node_config_bytes(
        delayed_request.config, delayed_request.config_size,
        delayed_request.config_alignment);
    EXPECT_THROW(
        iv::details::materialize_node_description(
            delayed_request, std::move(delayed_storage)),
        std::invalid_argument);
}

template<class Config>
concept HasSampleRange = requires(Config const& config) { config.min; config.max; };
template<class Config>
concept HasHistory = requires(Config const& config) { config.history; };
template<class Config>
concept HasLatency = requires(Config const& config) { config.latency; };
template<class Config>
concept HasRetention = requires(Config const& config) { config.retention; };

static_assert(std::same_as<decltype(iv::sequential_sample_input()), iv::InputConfig>);
static_assert(std::same_as<decltype(iv::random_access_sample_input()), iv::InputConfig>);
static_assert(std::same_as<
    decltype(iv::sequential_event_input({}, iv::EventTypeId::empty)), iv::InputConfig>);
static_assert(std::same_as<
    decltype(iv::random_access_event_input({}, iv::EventTypeId::empty)), iv::InputConfig>);
static_assert(std::same_as<decltype(iv::tick_sample_output()), iv::OutputConfig>);
static_assert(std::same_as<decltype(iv::tock_sample_output()), iv::OutputConfig>);
static_assert(std::same_as<
    decltype(iv::tick_event_output({}, iv::EventTypeId::empty)), iv::OutputConfig>);
static_assert(std::same_as<
    decltype(iv::tock_event_output({}, iv::EventTypeId::empty)), iv::OutputConfig>);
static_assert(HasSampleRange<iv::SampleInputProperties>);
static_assert(!HasSampleRange<iv::EventInputProperties>);
static_assert(!HasHistory<iv::SampleInputProperties>);
static_assert(!HasHistory<iv::SampleOutputProperties>);
static_assert(!HasHistory<iv::EventInputProperties>);
static_assert(!HasHistory<iv::EventOutputProperties>);
static_assert(!HasLatency<iv::SampleOutputProperties>);
static_assert(!HasLatency<iv::EventOutputProperties>);
static_assert(HasHistory<iv::SequentialInputConfig>);
static_assert(HasHistory<iv::TickOutputConfig>);
static_assert(HasLatency<iv::TickOutputConfig>);
static_assert(!HasHistory<iv::RandomAccessInputConfig>);
static_assert(!HasLatency<iv::TockOutputConfig>);
static_assert(HasRetention<iv::OutputConfig>);
static_assert(HasRetention<iv::SampleOutputConfig>);
static_assert(HasRetention<iv::EventOutputConfig>);
static_assert(iv::sample_properties(iv::InputConfig {}).neutral_value.value == 0.0f);
static_assert(iv::port_history(
    iv::sequential_sample_input("history", {}, {.history = 7})) == 7);
static_assert(iv::port_history(
    iv::sequential_event_input(
        "history", iv::EventTypeId::trigger, {.history = 5})) == 5);
static_assert(iv::port_history(
    iv::tick_sample_output(
        "timing", {}, {.history = 11, .latency = 3})) == 11);
static_assert(iv::tick_latency(
    iv::tick_sample_output(
        "timing", {}, {.history = 11, .latency = 3})) == 3);
static_assert(iv::is_random_access(iv::random_access_sample_input("background")));
static_assert(iv::is_tock(iv::tock_event_output(
    "background", iv::EventTypeId::trigger)));
static_assert(!iv::is_persisted(iv::tock_sample_output("ephemeral")));
static_assert(iv::is_persisted(iv::tock_sample_output(
    "persisted", {}, iv::OutputRetention::persisted)));
static_assert(iv::is_persisted(iv::tick_sample_output(
    "persisted", {}, {}, iv::OutputRetention::persisted)));

TEST(NodeBuildRequest, MaterializesHostOwnedDescriptionFromTypeSpecificCallback)
{
    RequestNode source{
        .latency = 17,
    };
    auto request = iv::details::make_node_build_request(source);
    auto storage = iv::details::copy_node_config_bytes(
        request.config, request.config_size, request.config_alignment);
    auto description = iv::details::materialize_node_description(
        request, std::move(storage));

    ASSERT_NE(description.node_storage.get(), nullptr);
    EXPECT_NE(description.node_storage.get(), static_cast<void const*>(std::addressof(source)));
    EXPECT_EQ(description.operations.runtime.node_data, description.node_storage.get());
    EXPECT_EQ(description.code_key, iv::details::node_code_key_v<RequestNode>);
    EXPECT_EQ(description.type_name, iv::details::clang_type_name<RequestNode>());
    ASSERT_EQ(description.inputs().size(), 1u);
    EXPECT_EQ(description.inputs().front().name, "level");
    ASSERT_EQ(description.outputs().size(), 1u);
    EXPECT_EQ(description.outputs().front().name, "signal");
    EXPECT_TRUE(iv::is_persisted(description.outputs().front()));
    EXPECT_EQ(description.internal_latency(), 17u);
    ASSERT_TRUE(description.ttl_samples().has_value());
    EXPECT_EQ(*description.ttl_samples(), 64u);
    EXPECT_TRUE(description.can_skip_block());
}

TEST(Coverage, CanonicalizesAndCoalescesRegions)
{
    iv::Coverage coverage {{
        {40, 50}, {10, 20}, {18, 30}, {30, 35}, {70, 70},
    }};

    ASSERT_EQ(coverage.size(), 2u);
    EXPECT_EQ(coverage.regions()[0], (iv::IndexRegion{10, 35}));
    EXPECT_EQ(coverage.regions()[1], (iv::IndexRegion{40, 50}));
    EXPECT_TRUE(coverage.contains(12));
    EXPECT_FALSE(coverage.contains(35));
    EXPECT_TRUE(coverage.contains({12, 34}));
    EXPECT_FALSE(coverage.contains({25, 45}));
}

TEST(Coverage, IncludeExcludeIntersectionAndDifferenceStayCanonical)
{
    iv::Coverage coverage {{{10, 20}, {30, 40}}};
    coverage.include({20, 30});
    ASSERT_EQ(coverage.size(), 1u);
    EXPECT_EQ(coverage.regions()[0], (iv::IndexRegion{10, 40}));

    coverage.exclude({15, 35});
    ASSERT_EQ(coverage.size(), 2u);
    EXPECT_EQ(coverage.regions()[0], (iv::IndexRegion{10, 15}));
    EXPECT_EQ(coverage.regions()[1], (iv::IndexRegion{35, 40}));

    iv::Coverage const other {{{12, 38}, {50, 60}}};
    EXPECT_EQ(coverage.intersection(other),
        (iv::Coverage{{{12, 15}, {35, 38}}}));
    EXPECT_EQ(other.difference(coverage),
        (iv::Coverage{{{15, 35}, {50, 60}}}));
}

struct BackgroundSource {
    struct TockState {
        int calls = 0;
        std::size_t sample_rate = 0;
    };

    static constexpr auto outputs()
    {
        return std::array {iv::tock_sample_output("signal")};
    }

    void tick_block(iv::TickBlockContext<BackgroundSource> const&) const {}
    void tock_coverage(iv::TockCoverageContext<BackgroundSource>& ctx) const
    {
        ++ctx.tock_state().calls;
        ctx.tock_state().sample_rate = ctx.sample_rate;
    }
    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<BackgroundSource>& ctx) const
    {
        ctx.output<"signal">().publish_coverage({{10, 20}});
    }
};

struct BackgroundTransform {
    mutable std::size_t tock_sample_rate = 0;
    mutable std::size_t forward_sample_rate = 0;
    mutable std::size_t reverse_sample_rate = 0;

    static constexpr auto inputs()
    {
        return std::array {
            iv::random_access_sample_input("samples"),
            iv::random_access_event_input("events", iv::EventTypeId::trigger),
        };
    }

    static constexpr auto outputs()
    {
        return std::array {
            iv::tock_sample_output(
                "samples-out", {},
                iv::OutputRetention::ephemeral),
            iv::tock_event_output(
                "events-out", iv::EventTypeId::trigger,
                iv::OutputRetention::persisted),
        };
    }

    void tick_block(iv::TickBlockContext<BackgroundTransform> const&) const {}
    void tock_coverage(iv::TockCoverageContext<BackgroundTransform>& context) const
    {
        tock_sample_rate = context.sample_rate;
    }
    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<BackgroundTransform>& ctx) const
    {
        forward_sample_rate = ctx.sample_rate;
        ctx.output<"samples-out">().publish_coverage(
            ctx.input<"samples">().coverage());
        ctx.output<"samples-out">().change(ctx.input<"samples">().changed());
        ctx.output<"events-out">().publish_coverage(
            ctx.input<"events">().coverage());
        ctx.output<"events-out">().change(ctx.input<"events">().changed());
    }
    void propagate_reverse_coverage(
        iv::PropagateReverseCoverageContext<BackgroundTransform>& ctx) const
    {
        reverse_sample_rate = ctx.sample_rate;
        ctx.input<"samples">().require(ctx.output<"samples-out">().required());
        ctx.input<"events">().require(ctx.output<"events-out">().required());
    }
};

struct BackgroundInputOnly {
    static constexpr auto inputs()
    {
        return std::array {
            iv::random_access_sample_input("samples"),
            iv::random_access_event_input("events", iv::EventTypeId::trigger),
        };
    }
    void tick_block(iv::TickBlockContext<BackgroundInputOnly> const&) const {}
};

struct MissingBackgroundTock {
    static constexpr auto outputs()
    {
        return std::array {iv::tock_sample_output("output")};
    }
};

struct StrayTock {
    void tock_coverage(iv::TockCoverageContext<StrayTock>&) const {}
};

struct StrayForward {
    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<StrayForward>&) const {}
};

struct StrayReverse {
    static constexpr auto outputs()
    {
        return std::array {iv::tock_sample_output("output")};
    }
    void tock_coverage(iv::TockCoverageContext<StrayReverse>&) const {}
    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<StrayReverse>&) const {}
    void propagate_reverse_coverage(
        iv::PropagateReverseCoverageContext<StrayReverse>&) const {}
};

struct PersistedRealtimeNode {
    static constexpr auto outputs()
    {
        return std::array {
            iv::tick_sample_output(
                "samples", {}, {}, iv::OutputRetention::persisted),
            iv::tick_event_output(
                "events", iv::EventTypeId::trigger, {},
                iv::OutputRetention::persisted),
        };
    }

    void tick_block(
        iv::TickBlockContext<PersistedRealtimeNode> const& context) const
    {
        auto samples = context.output<"samples">();
        for (std::size_t i = 0; i < context.block_size; ++i) {
            samples[i] = static_cast<float>(i);
        }
        context.output<"events">().push(iv::TriggerEvent{}, 2);
    }
};

struct MixedAccessIndexNode {
    static constexpr auto inputs()
    {
        return std::array<iv::InputConfig, 0>{};
    }

    static constexpr auto outputs()
    {
        return std::array {
            iv::tock_sample_output("background-sample"),
            iv::tick_sample_output("realtime-sample"),
            iv::tock_event_output(
                "background-event", iv::EventTypeId::trigger),
            iv::tick_event_output(
                "realtime-event", iv::EventTypeId::trigger),
        };
    }
};

template<class Context>
concept ExposesBackgroundState = requires(Context const& context) {
    context.tock_state();
};

static_assert(std::same_as<iv::NodeBackgroundState<BackgroundSource>::Type,
    BackgroundSource::TockState>);
static_assert(iv::details::node_background_state_size<BackgroundSource>()
    == sizeof(BackgroundSource::TockState));
static_assert(iv::details::node_background_state_alignment<BackgroundSource>()
    == alignof(BackgroundSource::TockState));
static_assert(std::same_as<
    decltype(iv::TickBlockContext<BackgroundSource>::index),
    iv::SampleIndex>);
static_assert(iv::details::declares_tock_sample_outputs_v<BackgroundSource>);
static_assert(!iv::details::declares_random_access_inputs_v<BackgroundSource>);
static_assert(iv::details::declares_random_access_inputs_v<BackgroundTransform>);
static_assert(iv::details::declares_tock_outputs_v<BackgroundTransform>);
static_assert(iv::details::declares_random_access_sample_inputs_v<BackgroundTransform>);
static_assert(iv::details::declares_random_access_event_inputs_v<BackgroundTransform>);
static_assert(iv::details::declares_tock_sample_outputs_v<BackgroundTransform>);
static_assert(iv::details::declares_tock_event_outputs_v<BackgroundTransform>);
static_assert(iv::details::background_dsp_node_declaration_is_valid_v<BackgroundSource>);
static_assert(iv::details::background_dsp_node_declaration_is_valid_v<BackgroundTransform>);
static_assert(iv::details::background_dsp_node_declaration_is_valid_v<BackgroundInputOnly>);
static_assert(iv::details::background_dsp_node_declaration_is_valid_v<
    PersistedRealtimeNode>);
static_assert(!iv::details::background_dsp_node_declaration_is_valid_v<MissingBackgroundTock>);
static_assert(!iv::details::background_dsp_node_declaration_is_valid_v<StrayTock>);
static_assert(!iv::details::background_dsp_node_declaration_is_valid_v<StrayForward>);
static_assert(!iv::details::background_dsp_node_declaration_is_valid_v<StrayReverse>);
static_assert(iv::details::static_random_access_input_port_index<
    BackgroundTransform, "samples">() == 0);
static_assert(iv::details::static_random_access_event_input_port_index<
    BackgroundTransform, "events">() == 0);
static_assert(iv::details::static_tock_output_port_index<
    BackgroundTransform, "samples-out">() == 0);
static_assert(iv::details::static_tock_event_output_port_index<
    BackgroundTransform, "events-out">() == 0);
static_assert(iv::details::static_output_port_is_tock<
    BackgroundTransform, "events-out">());
static_assert(iv::details::static_output_port_is_tock<
    MixedAccessIndexNode, "background-event">());
static_assert(!iv::details::static_output_port_is_tock<
    MixedAccessIndexNode, "realtime-event">());
static_assert(iv::details::static_output_port_index<
    MixedAccessIndexNode, "realtime-sample">() == 1);
static_assert(iv::details::static_realtime_output_port_index<
    MixedAccessIndexNode, "realtime-sample">() == 0);
static_assert(iv::details::static_event_output_port_index<
    MixedAccessIndexNode, "realtime-event">() == 1);
static_assert(iv::details::static_realtime_event_output_port_index<
    MixedAccessIndexNode, "realtime-event">() == 0);
static_assert(iv::details::reflected_sample_output_count_v<
    MixedAccessIndexNode> == 1);
static_assert(iv::details::reflected_event_output_count_v<
    MixedAccessIndexNode> == 1);
static_assert(iv::is_persisted(PersistedRealtimeNode::outputs()[0]));
static_assert(iv::is_persisted(PersistedRealtimeNode::outputs()[1]));
static_assert(!ExposesBackgroundState<iv::TickBlockContext<BackgroundSource>>);
static_assert(!ExposesBackgroundState<
    iv::PropagateForwardCoverageContext<BackgroundSource>>);
static_assert(!ExposesBackgroundState<
    iv::PropagateReverseCoverageContext<BackgroundSource>>);
static_assert(ExposesBackgroundState<iv::TockCoverageContext<BackgroundSource>>);
static_assert(iv::details::node_compiler_operations<BackgroundSource>()
    .tock_coverage != nullptr);
static_assert(iv::details::node_compiler_operations<BackgroundSource>()
    .propagate_forward_coverage != nullptr);
static_assert(iv::details::node_compiler_operations<BackgroundSource>()
    .propagate_reverse_coverage == nullptr);
static_assert(iv::details::node_compiler_operations<BackgroundTransform>()
    .propagate_reverse_coverage != nullptr);
static_assert(iv::details::node_compiler_operations<BackgroundInputOnly>()
    .tock_coverage == nullptr);
static_assert(iv::details::node_compiler_operations<PersistedRealtimeNode>()
    .tock_coverage == nullptr);
static_assert(iv::details::node_compiler_operations<PersistedRealtimeNode>()
    .propagate_forward_coverage == nullptr);

struct SampleData {
    std::array<iv::Sample, 8> values {
        0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f,
    };
};

iv::Sample read_sample(void const* data, iv::SampleIndex index, std::size_t)
{
    return static_cast<SampleData const*>(data)->values[index];
}

struct SampleWrites { std::array<iv::Sample, 8> values {}; };
void write_sample(void* data, iv::SampleIndex index, std::size_t, iv::Sample value)
{
    static_cast<SampleWrites*>(data)->values[index] = value;
}

struct EventData {
    std::array<iv::TimedEvent, 4> events {{
        {.time = 2, .value = iv::TriggerEvent{}},
        {.time = 3, .value = iv::TriggerEvent{}},
        {.time = 10, .value = iv::TriggerEvent{}},
        {.time = 12, .value = iv::TriggerEvent{}},
    }};
};

void for_each_event(
    void const* data,
    iv::SampleIndex begin,
    iv::SampleIndex end,
    void* visitor_data,
    iv::RandomAccessEventInputPort::VisitEvent visitor)
{
    for (auto const& event : static_cast<EventData const*>(data)->events) {
        if (begin <= event.time && event.time < end) visitor(visitor_data, event);
    }
}

struct EventWrites { std::vector<iv::TimedEvent> events; };
void write_event(void* data, iv::TimedEvent const& event)
{
    static_cast<EventWrites*>(data)->events.push_back(event);
}

TEST(BackgroundDspPorts, PersistedRealtimeOutputsUseOrdinaryTickBindings)
{
    std::array<iv::Sample, 256> sample_storage {};
    iv::SharedPortData sample_shared(sample_storage, 0);
    std::array sample_outputs {iv::OutputPort(sample_shared, 0, 128)};
    std::array<iv::TimedEvent, 16> event_storage {};
    iv::EventSharedPortData event_shared {
        event_storage, 0, 0, iv::EventTypeId::trigger,
    };
    std::array event_outputs {iv::EventOutputPort(
        event_shared, iv::EventTypeId::trigger)};

    iv::do_tick_block(
        PersistedRealtimeNode{},
        iv::TickBlockContext<PersistedRealtimeNode>{
            iv::TickContext<PersistedRealtimeNode>{
                .outputs = sample_outputs,
                .event_outputs = event_outputs,
            },
            128,
            4,
        });

    EXPECT_FLOAT_EQ(sample_storage[128], 0.0f);
    EXPECT_FLOAT_EQ(sample_storage[129], 1.0f);
    EXPECT_FLOAT_EQ(sample_storage[130], 2.0f);
    EXPECT_FLOAT_EQ(sample_storage[131], 3.0f);
    EXPECT_EQ(sample_outputs[0].position(), 132u);
    ASSERT_EQ(event_shared.write_index, 1u);
    EXPECT_EQ(event_storage[0].time, 130u);
}

TEST(BackgroundDspPorts, BackgroundInputsRemainAvailableFromTickBlock)
{
    std::array<iv::Sample, 8> realtime_samples {
        10.0f, 11.0f, 12.0f, 13.0f, 14.0f, 15.0f, 16.0f, 17.0f,
    };
    iv::SharedPortData sample_shared {
        realtime_samples,
        0,
        {.channel_type = iv::ChannelTypeId::mono,
         .sample_layout = iv::SampleStreamLayout::planar},
        8,
    };
    std::array realtime_inputs {iv::InputPort(sample_shared, 7)};

    std::array<iv::TimedEvent, 8> realtime_event_storage {};
    realtime_event_storage[0] = {.time = 8, .value = iv::TriggerEvent {}};
    realtime_event_storage[1] = {.time = 13, .value = iv::TriggerEvent {}};
    iv::EventSharedPortData event_shared {
        realtime_event_storage,
        0,
        2,
        iv::EventTypeId::trigger,
    };
    std::array realtime_event_inputs {iv::EventInputPort(event_shared)};

    iv::Coverage const sample_coverage {{{0, 8}}};
    iv::Coverage const event_coverage {{{0, 4}, {10, 14}}};
    SampleData sample_data;
    EventData event_data;
    std::array random_access_inputs {iv::RandomAccessSampleInputPort{
        .data = &sample_data,
        .coverage_value = &sample_coverage,
        .read_sample = &read_sample,
    }};
    std::array random_access_event_inputs {iv::RandomAccessEventInputPort{
        .data = &event_data,
        .coverage_value = &event_coverage,
        .for_each_event = &for_each_event,
    }};

    iv::TickBlockContext<BackgroundInputOnly> context {
        iv::TickContext<BackgroundInputOnly> {
            .inputs = realtime_inputs,
            .event_inputs = realtime_event_inputs,
            .random_access_inputs = random_access_inputs,
            .random_access_event_inputs = random_access_event_inputs,
        },
        8,
        8,
    };

    auto samples = context.input<"samples">();
    EXPECT_FLOAT_EQ(static_cast<float>(samples[0]), 10.0f);
    EXPECT_EQ(samples.coverage(), sample_coverage);
    EXPECT_FLOAT_EQ(static_cast<float>(samples.at(2)), 2.0f);

    auto events = context.input<"events">();
    auto const current_block = events.events();
    ASSERT_EQ(current_block.size(), 2u);
    EXPECT_EQ(current_block[0].time, 8u);
    EXPECT_EQ(current_block[1].time, 13u);

    std::vector<iv::SampleIndex> arbitrary;
    events.for_each(10, 14, [&](iv::TimedEvent const& event) {
        arbitrary.push_back(event.time);
    });
    EXPECT_EQ(arbitrary, (std::vector<iv::SampleIndex>{10, 12}));
}

TEST(BackgroundDspPorts, TockContextExposesCoverageAndSampleEventBindings)
{
    iv::Coverage const input_coverage {{{0, 8}}};
    iv::Coverage const event_coverage {{{0, 4}, {10, 14}}};
    iv::Coverage const sample_request {{{2, 5}}};
    iv::Coverage const event_request {{{10, 14}}};
    SampleData sample_data;
    SampleWrites sample_writes;
    EventData event_data;
    EventWrites event_writes;

    std::array sample_inputs {iv::RandomAccessSampleInputPort{
        .data = &sample_data,
        .coverage_value = &input_coverage,
        .read_sample = &read_sample,
    }};
    std::array sample_outputs {iv::TockSampleOutputPort{
        .data = &sample_writes,
        .requested_coverage_value = &sample_request,
        .write_sample = &write_sample,
    }};
    std::array event_inputs {iv::RandomAccessEventInputPort{
        .data = &event_data,
        .coverage_value = &event_coverage,
        .for_each_event = &for_each_event,
    }};
    std::array event_outputs {iv::TockEventOutputPort{
        .data = &event_writes,
        .requested_coverage_value = &event_request,
        .write_event = &write_event,
    }};
    iv::TockCoverageContext<BackgroundTransform> context {
        .inputs = sample_inputs,
        .outputs = sample_outputs,
        .event_inputs = event_inputs,
        .event_outputs = event_outputs,
        .sample_rate = 88200,
    };
    BackgroundTransform node;
    iv::do_tock_coverage(node, context);
    EXPECT_EQ(node.tock_sample_rate, 88200u);

    EXPECT_EQ(context.input<"samples">().coverage(), input_coverage);
    EXPECT_FLOAT_EQ(static_cast<float>(context.input<"samples">().at(3)), 3.0f);
    context.output<"samples-out">().write(3, 0.75f);
    EXPECT_FLOAT_EQ(static_cast<float>(sample_writes.values[3]), 0.75f);

    std::vector<iv::SampleIndex> visited;
    context.input<"events">().for_each(10, 14, [&](iv::TimedEvent const& event) {
        visited.push_back(event.time);
    });
    EXPECT_EQ(visited, (std::vector<iv::SampleIndex>{10, 12}));
    context.output<"events-out">().write(
        {.time = 12, .value = iv::TriggerEvent{}});
    ASSERT_EQ(event_writes.events.size(), 1u);
    EXPECT_EQ(event_writes.events.front().time, 12u);
}

struct PublishedCoverage {
    iv::Coverage coverage;
    iv::Coverage changed;
};
void publish_coverage(void* data, iv::Coverage const& coverage)
{
    static_cast<PublishedCoverage*>(data)->coverage = coverage;
}
void publish_changed(void* data, iv::Coverage const& changed)
{
    static_cast<PublishedCoverage*>(data)->changed.include(changed);
}

struct RequiredCoverage { iv::Coverage required; };
void publish_required(void* data, iv::Coverage const& required)
{
    static_cast<RequiredCoverage*>(data)->required.include(required);
}

TEST(BackgroundDspPorts, ForwardAndReverseContextsKeepExactDisjointCoverage)
{
    iv::Coverage const sample_coverage {{{10, 20}, {40, 50}}};
    iv::Coverage const sample_changed {{{12, 14}, {45, 47}}};
    iv::Coverage const event_coverage {{{100, 120}}};
    iv::Coverage const event_changed {{{108, 109}}};
    PublishedCoverage sample_output;
    PublishedCoverage event_output;
    std::array input_changes {iv::InputCoverageChange{
        .coverage_value = &sample_coverage, .changed_value = &sample_changed,
    }};
    std::array event_input_changes {iv::InputCoverageChange{
        .coverage_value = &event_coverage, .changed_value = &event_changed,
    }};
    std::array output_changes {iv::OutputCoverageChange{
        .data = &sample_output,
        .publish_coverage_value = &publish_coverage,
        .publish_changed_value = &publish_changed,
    }};
    std::array event_output_changes {iv::OutputCoverageChange{
        .data = &event_output,
        .publish_coverage_value = &publish_coverage,
        .publish_changed_value = &publish_changed,
    }};
    BackgroundTransform node;
    iv::PropagateForwardCoverageContext<BackgroundTransform> forward {
        .inputs = input_changes,
        .outputs = output_changes,
        .event_inputs = event_input_changes,
        .event_outputs = event_output_changes,
        .sample_rate = 44100,
    };
    iv::do_propagate_forward_coverage(node, forward);

    EXPECT_EQ(sample_output.coverage, sample_coverage);
    EXPECT_EQ(sample_output.changed, sample_changed);
    EXPECT_EQ(event_output.coverage, event_coverage);
    EXPECT_EQ(event_output.changed, event_changed);
    EXPECT_EQ(node.forward_sample_rate, 44100u);

    iv::Coverage const sample_demand {{{13, 15}, {42, 44}}};
    iv::Coverage const event_demand {{{110, 112}}};
    RequiredCoverage sample_input;
    RequiredCoverage event_input;
    std::array input_requirements {iv::InputCoverageRequirement{
        .data = &sample_input,
        .coverage_value = &sample_coverage,
        .publish_required_value = &publish_required,
    }};
    std::array event_input_requirements {iv::InputCoverageRequirement{
        .data = &event_input,
        .coverage_value = &event_coverage,
        .publish_required_value = &publish_required,
    }};
    std::array output_requirements {iv::OutputCoverageRequirement{
        .required_value = &sample_demand,
    }};
    std::array event_output_requirements {iv::OutputCoverageRequirement{
        .required_value = &event_demand,
    }};
    iv::PropagateReverseCoverageContext<BackgroundTransform> reverse {
        .inputs = input_requirements,
        .outputs = output_requirements,
        .event_inputs = event_input_requirements,
        .event_outputs = event_output_requirements,
        .sample_rate = 96000,
    };
    iv::do_propagate_reverse_coverage(node, reverse);

    EXPECT_EQ(sample_input.required, sample_demand);
    EXPECT_EQ(event_input.required, event_demand);
    EXPECT_EQ(node.reverse_sample_rate, 96000u);
}

struct DefaultPropagationNode {
    static constexpr auto inputs()
    {
        return std::array {iv::random_access_sample_input("input")};
    }
    static constexpr auto outputs()
    {
        return std::array {iv::tock_sample_output("output")};
    }
    void tock_coverage(iv::TockCoverageContext<DefaultPropagationNode>&) const {}
    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<DefaultPropagationNode>& context) const
    {
        auto output = context.output<"output">();
        output.publish_coverage(output.previous_coverage());
        if (context.local_state_changed
            || !context.input<"input">().changed().empty()) {
            output.change(output.previous_coverage());
        }
    }
};

TEST(BackgroundDspPorts, ExactForwardPropagationRetainsCoverageAndInvalidatesIt)
{
    iv::Coverage const input_coverage {{{10, 20}, {50, 60}}};
    iv::Coverage const input_changed {{{12, 13}}};
    iv::Coverage const previous_output {{{100, 120}, {200, 220}}};
    PublishedCoverage captured;
    std::array inputs {iv::InputCoverageChange{
        .coverage_value = &input_coverage,
        .changed_value = &input_changed,
    }};
    std::array outputs {iv::OutputCoverageChange{
        .data = &captured,
        .previous_coverage_value = &previous_output,
        .publish_coverage_value = &publish_coverage,
        .publish_changed_value = &publish_changed,
    }};
    DefaultPropagationNode node;
    iv::PropagateForwardCoverageContext<DefaultPropagationNode> context {
        .inputs = inputs,
        .outputs = outputs,
    };

    iv::do_propagate_forward_coverage(node, context);

    EXPECT_EQ(captured.coverage, previous_output);
    EXPECT_EQ(captured.changed, previous_output);
}

TEST(BackgroundDspPorts, DefaultReversePropagationRequiresWholeInputCoverage)
{
    iv::Coverage const input_coverage {{{10, 20}, {50, 60}}};
    iv::Coverage const output_demand {{{100, 101}}};
    RequiredCoverage captured;
    std::array inputs {iv::InputCoverageRequirement{
        .data = &captured,
        .coverage_value = &input_coverage,
        .publish_required_value = &publish_required,
    }};
    std::array outputs {iv::OutputCoverageRequirement{
        .required_value = &output_demand,
    }};
    DefaultPropagationNode node;
    iv::PropagateReverseCoverageContext<DefaultPropagationNode> context {
        .inputs = inputs, .outputs = outputs,
    };

    iv::do_propagate_reverse_coverage(node, context);

    EXPECT_EQ(captured.required, input_coverage);
}

TEST(BackgroundDspPorts, BackgroundStateAndSampleRateAreTockOnly)
{
    alignas(BackgroundSource::TockState)
        std::array<std::byte, sizeof(BackgroundSource::TockState)> storage {};
    auto* state = std::construct_at(
        reinterpret_cast<BackgroundSource::TockState*>(storage.data()));
    BackgroundSource node;
    auto const operations = iv::details::node_compiler_operations<BackgroundSource>();

    operations.tick_block(&node, iv::ReflectedNodeTickContext {}, 0, 16);
    iv::ReflectedNodeTockCoverageContext tock {
        .background_state_storage = storage,
        .sample_rate = 96000,
    };
    operations.tock_coverage(&node, tock);

    EXPECT_EQ(state->calls, 1);
    EXPECT_EQ(state->sample_rate, 96000u);
    std::destroy_at(state);
}

TEST(BackgroundDspPorts, CompilerRecordAnchorsAllBackgroundOperations)
{
    auto const operations = iv::details::node_compiler_operations<BackgroundTransform>();
    ASSERT_NE(operations.tock_coverage, nullptr);
    ASSERT_NE(operations.propagate_forward_coverage, nullptr);
    ASSERT_NE(operations.propagate_reverse_coverage, nullptr);
}

} // namespace
