#include <intravenous/graph/reflected_node_description.h>
#include <intravenous/node/indexed_port_context.h>
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
        return std::array{iv::realtime_sample_input("level")};
    }
    static constexpr auto outputs()
    {
        return std::array{iv::realtime_sample_output(
            "signal", {}, {}, iv::OutputRetention::persisted)};
    }
    std::size_t internal_latency() const { return latency; }
    std::optional<std::size_t> ttl_samples() const { return 64; }
    bool can_skip_block() const { return true; }
    void tick_block(auto const&) const {}
};

template<class Config>
concept HasSampleRange = requires(Config const& config) { config.min; config.max; };
template<class Config>
concept HasHistory = requires(Config const& config) { config.history; };
template<class Config>
concept HasLatency = requires(Config const& config) { config.latency; };
template<class Config>
concept HasRetention = requires(Config const& config) { config.retention; };

static_assert(std::same_as<decltype(iv::realtime_sample_input()), iv::InputConfig>);
static_assert(std::same_as<decltype(iv::indexed_sample_input()), iv::InputConfig>);
static_assert(std::same_as<
    decltype(iv::realtime_event_input({}, iv::EventTypeId::empty)), iv::InputConfig>);
static_assert(std::same_as<
    decltype(iv::indexed_event_input({}, iv::EventTypeId::empty)), iv::InputConfig>);
static_assert(std::same_as<decltype(iv::realtime_sample_output()), iv::OutputConfig>);
static_assert(std::same_as<decltype(iv::indexed_sample_output()), iv::OutputConfig>);
static_assert(std::same_as<
    decltype(iv::realtime_event_output({}, iv::EventTypeId::empty)), iv::OutputConfig>);
static_assert(std::same_as<
    decltype(iv::indexed_event_output({}, iv::EventTypeId::empty)), iv::OutputConfig>);
static_assert(HasSampleRange<iv::SampleInputProperties>);
static_assert(!HasSampleRange<iv::EventInputProperties>);
static_assert(!HasHistory<iv::SampleInputProperties>);
static_assert(!HasHistory<iv::SampleOutputProperties>);
static_assert(!HasHistory<iv::EventInputProperties>);
static_assert(!HasHistory<iv::EventOutputProperties>);
static_assert(!HasLatency<iv::SampleOutputProperties>);
static_assert(!HasLatency<iv::EventOutputProperties>);
static_assert(HasHistory<iv::RealtimeInputConfig>);
static_assert(HasHistory<iv::RealtimeOutputConfig>);
static_assert(HasLatency<iv::RealtimeOutputConfig>);
static_assert(!HasHistory<iv::IndexedInputConfig>);
static_assert(!HasLatency<iv::IndexedOutputConfig>);
static_assert(HasRetention<iv::OutputConfig>);
static_assert(HasRetention<iv::SampleOutputConfig>);
static_assert(HasRetention<iv::EventOutputConfig>);
static_assert(iv::sample_properties(iv::InputConfig {}).neutral_value.value == 0.0f);
static_assert(iv::realtime_history(
    iv::realtime_sample_input("history", {}, {.history = 7})) == 7);
static_assert(iv::realtime_history(
    iv::realtime_event_input(
        "history", iv::EventTypeId::trigger, {.history = 5})) == 5);
static_assert(iv::realtime_history(
    iv::realtime_sample_output(
        "timing", {}, {.history = 11, .latency = 3})) == 11);
static_assert(iv::realtime_latency(
    iv::realtime_sample_output(
        "timing", {}, {.history = 11, .latency = 3})) == 3);
static_assert(iv::is_indexed(iv::indexed_sample_input("indexed")));
static_assert(iv::is_indexed(iv::indexed_event_output(
    "indexed", iv::EventTypeId::trigger)));
static_assert(!iv::is_persisted(iv::indexed_sample_output("ephemeral")));
static_assert(iv::is_persisted(iv::indexed_sample_output(
    "persisted", {}, iv::OutputRetention::persisted)));
static_assert(iv::is_persisted(iv::realtime_sample_output(
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

TEST(IndexedCoverage, CanonicalizesAndCoalescesRegions)
{
    iv::IndexedCoverage coverage {{
        {40, 50}, {10, 20}, {18, 30}, {30, 35}, {70, 70},
    }};

    ASSERT_EQ(coverage.size(), 2u);
    EXPECT_EQ(coverage.regions()[0], (iv::IndexedRegion{10, 35}));
    EXPECT_EQ(coverage.regions()[1], (iv::IndexedRegion{40, 50}));
    EXPECT_TRUE(coverage.contains(12));
    EXPECT_FALSE(coverage.contains(35));
    EXPECT_TRUE(coverage.contains({12, 34}));
    EXPECT_FALSE(coverage.contains({25, 45}));
}

TEST(IndexedCoverage, IncludeExcludeIntersectionAndDifferenceStayCanonical)
{
    iv::IndexedCoverage coverage {{{10, 20}, {30, 40}}};
    coverage.include({20, 30});
    ASSERT_EQ(coverage.size(), 1u);
    EXPECT_EQ(coverage.regions()[0], (iv::IndexedRegion{10, 40}));

    coverage.exclude({15, 35});
    ASSERT_EQ(coverage.size(), 2u);
    EXPECT_EQ(coverage.regions()[0], (iv::IndexedRegion{10, 15}));
    EXPECT_EQ(coverage.regions()[1], (iv::IndexedRegion{35, 40}));

    iv::IndexedCoverage const other {{{12, 38}, {50, 60}}};
    EXPECT_EQ(coverage.intersection(other),
        (iv::IndexedCoverage{{{12, 15}, {35, 38}}}));
    EXPECT_EQ(other.difference(coverage),
        (iv::IndexedCoverage{{{15, 35}, {50, 60}}}));
}

struct IndexedSource {
    struct IndexedState {
        int calls = 0;
        std::size_t sample_rate = 0;
    };

    static constexpr auto outputs()
    {
        return std::array {iv::indexed_sample_output("signal")};
    }

    void tick_block(iv::TickBlockContext<IndexedSource> const&) const {}
    void tock_coverage(iv::TockCoverageContext<IndexedSource>& ctx) const
    {
        ++ctx.indexed_state().calls;
        ctx.indexed_state().sample_rate = ctx.sample_rate;
    }
    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<IndexedSource>& ctx) const
    {
        ctx.output<"signal">().publish_coverage({{10, 20}});
    }
};

struct IndexedTransform {
    mutable std::size_t tock_sample_rate = 0;
    mutable std::size_t forward_sample_rate = 0;
    mutable std::size_t reverse_sample_rate = 0;

    static constexpr auto inputs()
    {
        return std::array {
            iv::indexed_sample_input("samples"),
            iv::indexed_event_input("events", iv::EventTypeId::trigger),
        };
    }

    static constexpr auto outputs()
    {
        return std::array {
            iv::indexed_sample_output(
                "samples-out", {},
                iv::OutputRetention::ephemeral),
            iv::indexed_event_output(
                "events-out", iv::EventTypeId::trigger,
                iv::OutputRetention::persisted),
        };
    }

    void tick_block(iv::TickBlockContext<IndexedTransform> const&) const {}
    void tock_coverage(iv::TockCoverageContext<IndexedTransform>& context) const
    {
        tock_sample_rate = context.sample_rate;
    }
    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<IndexedTransform>& ctx) const
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
        iv::PropagateReverseCoverageContext<IndexedTransform>& ctx) const
    {
        reverse_sample_rate = ctx.sample_rate;
        ctx.input<"samples">().require(ctx.output<"samples-out">().required());
        ctx.input<"events">().require(ctx.output<"events-out">().required());
    }
};

struct IndexedInputOnly {
    static constexpr auto inputs()
    {
        return std::array {
            iv::indexed_sample_input("samples"),
            iv::indexed_event_input("events", iv::EventTypeId::trigger),
        };
    }
    void tick_block(iv::TickBlockContext<IndexedInputOnly> const&) const {}
};

struct MissingIndexedTock {
    static constexpr auto outputs()
    {
        return std::array {iv::indexed_sample_output("output")};
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
        return std::array {iv::indexed_sample_output("output")};
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
            iv::realtime_sample_output(
                "samples", {}, {}, iv::OutputRetention::persisted),
            iv::realtime_event_output(
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

struct MixedAccessOrdinalNode {
    static constexpr auto inputs()
    {
        return std::array<iv::InputConfig, 0>{};
    }

    static constexpr auto outputs()
    {
        return std::array {
            iv::indexed_sample_output("indexed-sample"),
            iv::realtime_sample_output("realtime-sample"),
            iv::indexed_event_output(
                "indexed-event", iv::EventTypeId::trigger),
            iv::realtime_event_output(
                "realtime-event", iv::EventTypeId::trigger),
        };
    }
};

template<class Context>
concept ExposesIndexedState = requires(Context const& context) {
    context.indexed_state();
};

static_assert(std::same_as<iv::NodeIndexedState<IndexedSource>::Type,
    IndexedSource::IndexedState>);
static_assert(iv::details::node_indexed_state_size<IndexedSource>()
    == sizeof(IndexedSource::IndexedState));
static_assert(iv::details::node_indexed_state_alignment<IndexedSource>()
    == alignof(IndexedSource::IndexedState));
static_assert(std::same_as<
    decltype(iv::TickBlockContext<IndexedSource>::index),
    iv::SampleIndex>);
static_assert(iv::details::declares_indexed_sample_outputs_v<IndexedSource>);
static_assert(!iv::details::declares_indexed_inputs_v<IndexedSource>);
static_assert(iv::details::declares_indexed_inputs_v<IndexedTransform>);
static_assert(iv::details::declares_indexed_outputs_v<IndexedTransform>);
static_assert(iv::details::declares_indexed_sample_inputs_v<IndexedTransform>);
static_assert(iv::details::declares_indexed_event_inputs_v<IndexedTransform>);
static_assert(iv::details::declares_indexed_sample_outputs_v<IndexedTransform>);
static_assert(iv::details::declares_indexed_event_outputs_v<IndexedTransform>);
static_assert(iv::details::indexed_dsp_node_declaration_is_valid_v<IndexedSource>);
static_assert(iv::details::indexed_dsp_node_declaration_is_valid_v<IndexedTransform>);
static_assert(iv::details::indexed_dsp_node_declaration_is_valid_v<IndexedInputOnly>);
static_assert(iv::details::indexed_dsp_node_declaration_is_valid_v<
    PersistedRealtimeNode>);
static_assert(!iv::details::indexed_dsp_node_declaration_is_valid_v<MissingIndexedTock>);
static_assert(!iv::details::indexed_dsp_node_declaration_is_valid_v<StrayTock>);
static_assert(!iv::details::indexed_dsp_node_declaration_is_valid_v<StrayForward>);
static_assert(!iv::details::indexed_dsp_node_declaration_is_valid_v<StrayReverse>);
static_assert(iv::details::static_indexed_input_port_index<
    IndexedTransform, "samples">() == 0);
static_assert(iv::details::static_indexed_event_input_port_index<
    IndexedTransform, "events">() == 0);
static_assert(iv::details::static_tock_output_port_index<
    IndexedTransform, "samples-out">() == 0);
static_assert(iv::details::static_tock_event_output_port_index<
    IndexedTransform, "events-out">() == 0);
static_assert(iv::details::static_output_port_is_indexed<
    IndexedTransform, "events-out">());
static_assert(iv::details::static_output_port_is_indexed<
    MixedAccessOrdinalNode, "indexed-event">());
static_assert(!iv::details::static_output_port_is_indexed<
    MixedAccessOrdinalNode, "realtime-event">());
static_assert(iv::details::static_output_port_index<
    MixedAccessOrdinalNode, "realtime-sample">() == 1);
static_assert(iv::details::static_realtime_output_port_index<
    MixedAccessOrdinalNode, "realtime-sample">() == 0);
static_assert(iv::details::static_event_output_port_index<
    MixedAccessOrdinalNode, "realtime-event">() == 1);
static_assert(iv::details::static_realtime_event_output_port_index<
    MixedAccessOrdinalNode, "realtime-event">() == 0);
static_assert(iv::details::reflected_sample_output_count_v<
    MixedAccessOrdinalNode> == 1);
static_assert(iv::details::reflected_event_output_count_v<
    MixedAccessOrdinalNode> == 1);
static_assert(iv::is_persisted(PersistedRealtimeNode::outputs()[0]));
static_assert(iv::is_persisted(PersistedRealtimeNode::outputs()[1]));
static_assert(!ExposesIndexedState<iv::TickBlockContext<IndexedSource>>);
static_assert(!ExposesIndexedState<
    iv::PropagateForwardCoverageContext<IndexedSource>>);
static_assert(!ExposesIndexedState<
    iv::PropagateReverseCoverageContext<IndexedSource>>);
static_assert(ExposesIndexedState<iv::TockCoverageContext<IndexedSource>>);
static_assert(iv::details::node_compiler_operations<IndexedSource>()
    .tock_coverage != nullptr);
static_assert(iv::details::node_compiler_operations<IndexedSource>()
    .propagate_forward_coverage != nullptr);
static_assert(iv::details::node_compiler_operations<IndexedSource>()
    .propagate_reverse_coverage == nullptr);
static_assert(iv::details::node_compiler_operations<IndexedTransform>()
    .propagate_reverse_coverage != nullptr);
static_assert(iv::details::node_compiler_operations<IndexedInputOnly>()
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
    iv::IndexedEventInputPort::VisitEvent visitor)
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

TEST(IndexedDspPorts, PersistedRealtimeOutputsUseOrdinaryTickBindings)
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

TEST(IndexedDspPorts, IndexedInputsRemainAvailableFromTickBlock)
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

    iv::IndexedCoverage const sample_coverage {{{0, 8}}};
    iv::IndexedCoverage const event_coverage {{{0, 4}, {10, 14}}};
    SampleData sample_data;
    EventData event_data;
    std::array indexed_inputs {iv::IndexedSampleInputPort{
        .data = &sample_data,
        .coverage_value = &sample_coverage,
        .read_sample = &read_sample,
    }};
    std::array indexed_event_inputs {iv::IndexedEventInputPort{
        .data = &event_data,
        .coverage_value = &event_coverage,
        .for_each_event = &for_each_event,
    }};

    iv::TickBlockContext<IndexedInputOnly> context {
        iv::TickContext<IndexedInputOnly> {
            .inputs = realtime_inputs,
            .event_inputs = realtime_event_inputs,
            .indexed_inputs = indexed_inputs,
            .indexed_event_inputs = indexed_event_inputs,
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

TEST(IndexedDspPorts, TockContextExposesCoverageAndSampleEventBindings)
{
    iv::IndexedCoverage const input_coverage {{{0, 8}}};
    iv::IndexedCoverage const event_coverage {{{0, 4}, {10, 14}}};
    iv::IndexedCoverage const sample_request {{{2, 5}}};
    iv::IndexedCoverage const event_request {{{10, 14}}};
    SampleData sample_data;
    SampleWrites sample_writes;
    EventData event_data;
    EventWrites event_writes;

    std::array sample_inputs {iv::IndexedSampleInputPort{
        .data = &sample_data,
        .coverage_value = &input_coverage,
        .read_sample = &read_sample,
    }};
    std::array sample_outputs {iv::IndexedSampleOutputPort{
        .data = &sample_writes,
        .requested_coverage_value = &sample_request,
        .write_sample = &write_sample,
    }};
    std::array event_inputs {iv::IndexedEventInputPort{
        .data = &event_data,
        .coverage_value = &event_coverage,
        .for_each_event = &for_each_event,
    }};
    std::array event_outputs {iv::IndexedEventOutputPort{
        .data = &event_writes,
        .requested_coverage_value = &event_request,
        .write_event = &write_event,
    }};
    iv::TockCoverageContext<IndexedTransform> context {
        .inputs = sample_inputs,
        .outputs = sample_outputs,
        .event_inputs = event_inputs,
        .event_outputs = event_outputs,
        .sample_rate = 88200,
    };
    IndexedTransform node;
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
    iv::IndexedCoverage coverage;
    iv::IndexedCoverage changed;
};
void publish_coverage(void* data, iv::IndexedCoverage const& coverage)
{
    static_cast<PublishedCoverage*>(data)->coverage = coverage;
}
void publish_changed(void* data, iv::IndexedCoverage const& changed)
{
    static_cast<PublishedCoverage*>(data)->changed.include(changed);
}

struct RequiredCoverage { iv::IndexedCoverage required; };
void publish_required(void* data, iv::IndexedCoverage const& required)
{
    static_cast<RequiredCoverage*>(data)->required.include(required);
}

TEST(IndexedDspPorts, ForwardAndReverseContextsKeepExactDisjointCoverage)
{
    iv::IndexedCoverage const sample_coverage {{{10, 20}, {40, 50}}};
    iv::IndexedCoverage const sample_changed {{{12, 14}, {45, 47}}};
    iv::IndexedCoverage const event_coverage {{{100, 120}}};
    iv::IndexedCoverage const event_changed {{{108, 109}}};
    PublishedCoverage sample_output;
    PublishedCoverage event_output;
    std::array input_changes {iv::IndexedInputChange{
        .coverage_value = &sample_coverage, .changed_value = &sample_changed,
    }};
    std::array event_input_changes {iv::IndexedInputChange{
        .coverage_value = &event_coverage, .changed_value = &event_changed,
    }};
    std::array output_changes {iv::IndexedOutputChange{
        .data = &sample_output,
        .publish_coverage_value = &publish_coverage,
        .publish_changed_value = &publish_changed,
    }};
    std::array event_output_changes {iv::IndexedOutputChange{
        .data = &event_output,
        .publish_coverage_value = &publish_coverage,
        .publish_changed_value = &publish_changed,
    }};
    IndexedTransform node;
    iv::PropagateForwardCoverageContext<IndexedTransform> forward {
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

    iv::IndexedCoverage const sample_demand {{{13, 15}, {42, 44}}};
    iv::IndexedCoverage const event_demand {{{110, 112}}};
    RequiredCoverage sample_input;
    RequiredCoverage event_input;
    std::array input_requirements {iv::IndexedInputRequirement{
        .data = &sample_input,
        .coverage_value = &sample_coverage,
        .publish_required_value = &publish_required,
    }};
    std::array event_input_requirements {iv::IndexedInputRequirement{
        .data = &event_input,
        .coverage_value = &event_coverage,
        .publish_required_value = &publish_required,
    }};
    std::array output_requirements {iv::IndexedOutputRequirement{
        .required_value = &sample_demand,
    }};
    std::array event_output_requirements {iv::IndexedOutputRequirement{
        .required_value = &event_demand,
    }};
    iv::PropagateReverseCoverageContext<IndexedTransform> reverse {
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
        return std::array {iv::indexed_sample_input("input")};
    }
    static constexpr auto outputs()
    {
        return std::array {iv::indexed_sample_output("output")};
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

TEST(IndexedDspPorts, ExactForwardPropagationRetainsCoverageAndInvalidatesIt)
{
    iv::IndexedCoverage const input_coverage {{{10, 20}, {50, 60}}};
    iv::IndexedCoverage const input_changed {{{12, 13}}};
    iv::IndexedCoverage const previous_output {{{100, 120}, {200, 220}}};
    PublishedCoverage captured;
    std::array inputs {iv::IndexedInputChange{
        .coverage_value = &input_coverage,
        .changed_value = &input_changed,
    }};
    std::array outputs {iv::IndexedOutputChange{
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

TEST(IndexedDspPorts, DefaultReversePropagationRequiresWholeInputCoverage)
{
    iv::IndexedCoverage const input_coverage {{{10, 20}, {50, 60}}};
    iv::IndexedCoverage const output_demand {{{100, 101}}};
    RequiredCoverage captured;
    std::array inputs {iv::IndexedInputRequirement{
        .data = &captured,
        .coverage_value = &input_coverage,
        .publish_required_value = &publish_required,
    }};
    std::array outputs {iv::IndexedOutputRequirement{
        .required_value = &output_demand,
    }};
    DefaultPropagationNode node;
    iv::PropagateReverseCoverageContext<DefaultPropagationNode> context {
        .inputs = inputs, .outputs = outputs,
    };

    iv::do_propagate_reverse_coverage(node, context);

    EXPECT_EQ(captured.required, input_coverage);
}

TEST(IndexedDspPorts, IndexedStateAndSampleRateAreTockOnly)
{
    alignas(IndexedSource::IndexedState)
        std::array<std::byte, sizeof(IndexedSource::IndexedState)> storage {};
    auto* state = std::construct_at(
        reinterpret_cast<IndexedSource::IndexedState*>(storage.data()));
    IndexedSource node;
    auto const operations = iv::details::node_compiler_operations<IndexedSource>();

    operations.tick_block(&node, iv::ReflectedNodeTickContext {}, 0, 16);
    iv::TockCoverageContext<IndexedSource> tock {
        .indexed_state_storage = storage,
        .sample_rate = 96000,
    };
    operations.tock_coverage(&node, &tock);

    EXPECT_EQ(state->calls, 1);
    EXPECT_EQ(state->sample_rate, 96000u);
    std::destroy_at(state);
}

TEST(IndexedDspPorts, CompilerRecordAnchorsAllIndexedOperations)
{
    auto const operations = iv::details::node_compiler_operations<IndexedTransform>();
    ASSERT_NE(operations.tock_coverage, nullptr);
    ASSERT_NE(operations.propagate_forward_coverage, nullptr);
    ASSERT_NE(operations.propagate_reverse_coverage, nullptr);
}

} // namespace
