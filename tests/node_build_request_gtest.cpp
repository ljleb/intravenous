#include <intravenous/graph/reflected_node_description.h>
#include <intravenous/node/compiled_port_context.h>
#include <intravenous/node/tick.h>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>

namespace {

struct RequestNode {
    char const* input_name = "input";
    char const* output_name = "output";
    std::size_t latency = 0;

    auto inputs() const
    {
        return std::array{iv::realtime_sample_input(input_name)};
    }

    auto outputs() const
    {
        return std::array{iv::realtime_sample_output(output_name)};
    }

    std::size_t internal_latency() const
    {
        return latency;
    }

    std::optional<std::size_t> ttl_samples() const
    {
        return 64;
    }

    bool can_skip_block() const
    {
        return true;
    }

    void tick_block(auto const&) const {}
};

template<class Config>
concept HasSampleRange = requires(Config const& config) {
    config.min;
    config.max;
};

template<class Config>
concept HasHistory = requires(Config const& config) {
    config.history;
};

template<class Config>
concept HasLatency = requires(Config const& config) {
    config.latency;
};

static_assert(std::same_as<decltype(iv::realtime_sample_input()), iv::InputConfig>);
static_assert(std::same_as<
    decltype(iv::realtime_event_input({}, iv::EventTypeId::empty)), iv::InputConfig>);
static_assert(std::same_as<decltype(iv::realtime_sample_output()), iv::OutputConfig>);
static_assert(std::same_as<
    decltype(iv::realtime_event_output({}, iv::EventTypeId::empty)), iv::OutputConfig>);
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
static_assert(!HasHistory<iv::CompiledPortConfig>);
static_assert(!HasLatency<iv::CompiledPortConfig>);
static_assert(iv::sample_properties(iv::InputConfig {}).neutral_value.value == 0.0f);
static_assert(iv::realtime_history(
    iv::realtime_sample_input("history", {}, {.history = 7})) == 7);
static_assert(iv::realtime_history(
    iv::realtime_event_input("history", iv::EventTypeId::trigger, {.history = 5})) == 5);
static_assert(iv::realtime_history(
    iv::realtime_sample_output("timing", {}, {.history = 11, .latency = 3})) == 11);
static_assert(iv::realtime_latency(
    iv::realtime_sample_output("timing", {}, {.history = 11, .latency = 3})) == 3);
static_assert(iv::is_compiled(iv::compiled_sample_input("compiled")));
static_assert(iv::is_compiled(iv::compiled_event_output(
    "compiled", iv::EventTypeId::trigger)));

TEST(NodeBuildRequest, MaterializesHostOwnedDescriptionFromTypeSpecificCallback)
{
    RequestNode source{
        .input_name = "level",
        .output_name = "signal",
        .latency = 17,
    };
    auto request = iv::details::make_node_build_request(source);
    auto storage = iv::details::copy_node_config_bytes(
        request.config, request.config_size, request.config_alignment);
    auto description = iv::details::materialize_node_description(
        request, std::move(storage));

    ASSERT_NE(description.node_storage.get(), nullptr);
    EXPECT_NE(
        description.node_storage.get(),
        static_cast<void const*>(std::addressof(source)));
    EXPECT_EQ(description.operations.runtime.node_data, description.node_storage.get());
    EXPECT_EQ(description.code_key, iv::details::node_code_key_v<RequestNode>);
    EXPECT_EQ(description.type_name, iv::details::clang_type_name<RequestNode>());
    ASSERT_EQ(description.inputs().size(), 1u);
    EXPECT_EQ(description.inputs().front().name, "level");
    ASSERT_EQ(description.outputs().size(), 1u);
    EXPECT_EQ(description.outputs().front().name, "signal");
    EXPECT_EQ(description.internal_latency(), 17u);
    ASSERT_TRUE(description.ttl_samples().has_value());
    EXPECT_EQ(*description.ttl_samples(), 64u);
    EXPECT_TRUE(description.can_skip_block());
}

struct CompiledSource {
    struct CompiledState {
        int calls = 0;
    };

    static constexpr auto outputs()
    {
        return std::array {iv::compiled_sample_output("signal")};
    }

    void tick_block(iv::TickBlockContext<CompiledSource> const&) const {}
    void access_block(iv::AccessBlockContext<CompiledSource>&) const {}
};


struct TickCompiledStateRecorder {
    struct CompiledState {
        int writes = 0;
    };

    static constexpr auto inputs()
    {
        return std::array<iv::InputConfig, 0> {};
    }

    static constexpr auto outputs()
    {
        return std::array<iv::OutputConfig, 0> {};
    }

    void tick_block(iv::TickBlockContext<TickCompiledStateRecorder> const& ctx) const
    {
        ++ctx.compiled_state().writes;
    }
};

struct CompiledTransform {
    static constexpr auto inputs()
    {
        return std::array {iv::compiled_sample_input("input")};
    }

    static constexpr auto outputs()
    {
        return std::array {iv::compiled_sample_output("output")};
    }

    void tick_block(iv::TickBlockContext<CompiledTransform> const&) const {}
    void access_block_batch(iv::AccessBlockBatchContext<CompiledTransform>&) const {}
    void propagate_block_access_batch(
        iv::PropagateBlockAccessBatchContext<CompiledTransform>&) const {}
};


struct CompiledEvents {
    static constexpr auto inputs()
    {
        return std::array {iv::compiled_event_input("events-in", iv::EventTypeId::trigger)};
    }

    static constexpr auto outputs()
    {
        return std::array {iv::compiled_event_output("events-out", iv::EventTypeId::trigger)};
    }

    void tick_block(iv::TickBlockContext<CompiledEvents> const&) const {}
    void access_block(iv::AccessBlockContext<CompiledEvents>&) const {}
    void propagate_block_access(
        iv::PropagateBlockAccessContext<CompiledEvents>& ctx) const
    {
        ctx.input<"events-in">(ctx.output<"events-out">());
    }
};

struct CompiledInputOnly {
    static constexpr auto inputs()
    {
        return std::array {
            iv::compiled_sample_input("samples"),
            iv::compiled_event_input("events", iv::EventTypeId::trigger),
        };
    }

    void tick_block(iv::TickBlockContext<CompiledInputOnly> const&) const {}
};

struct MissingEventBlockAccessPropagation {
    static constexpr auto inputs()
    {
        return std::array {
            iv::compiled_event_input("events-in", iv::EventTypeId::trigger),
        };
    }

    static constexpr auto outputs()
    {
        return std::array {
            iv::compiled_event_output("events-out", iv::EventTypeId::trigger),
        };
    }

    void access_block(
        iv::AccessBlockContext<MissingEventBlockAccessPropagation>&) const {}
};

struct CompiledEventSource {
    static constexpr auto outputs()
    {
        return std::array {
            iv::compiled_event_output("events", iv::EventTypeId::trigger),
        };
    }

    void access_block(iv::AccessBlockContext<CompiledEventSource>&) const {}
};

struct MissingCompiledAccess {
    static constexpr auto outputs()
    {
        return std::array {iv::compiled_sample_output("output")};
    }
};

struct ConflictingCompiledAccess {
    static constexpr auto outputs()
    {
        return std::array {iv::compiled_sample_output("output")};
    }

    void access_block(iv::AccessBlockContext<ConflictingCompiledAccess>&) const {}
    void access_block_batch(
        iv::AccessBlockBatchContext<ConflictingCompiledAccess>&) const {}
};

struct MissingBlockAccessPropagation {
    static constexpr auto inputs()
    {
        return std::array {iv::compiled_sample_input("input")};
    }

    static constexpr auto outputs()
    {
        return std::array {iv::compiled_sample_output("output")};
    }

    void access_block(iv::AccessBlockContext<MissingBlockAccessPropagation>&) const {}
};

struct MixedCompiledPorts {
    static constexpr auto inputs()
    {
        return std::array {
            iv::realtime_sample_input("realtime"),
            iv::compiled_sample_input("compiled"),
        };
    }

    static constexpr auto outputs()
    {
        return std::array {
            iv::realtime_sample_output("realtime"),
            iv::compiled_sample_output("compiled"),
        };
    }

    void access_block(iv::AccessBlockContext<MixedCompiledPorts>&) const {}
    void propagate_block_access(
        iv::PropagateBlockAccessContext<MixedCompiledPorts>&) const {}
};

struct MixedCompiledKinds {
    static constexpr auto inputs()
    {
        return std::array {
            iv::compiled_sample_input("samples"),
            iv::compiled_event_input("events", iv::EventTypeId::trigger),
        };
    }

    static constexpr auto outputs()
    {
        return std::array {
            iv::compiled_sample_output("samples-out"),
            iv::compiled_event_output("events-out", iv::EventTypeId::trigger),
        };
    }

    void access_block(iv::AccessBlockContext<MixedCompiledKinds>&) const {}
};

struct UnbatchedAccessNode {
    int* calls = nullptr;

    static constexpr auto outputs()
    {
        return std::array {iv::compiled_sample_output("output")};
    }

    void access_block(iv::AccessBlockContext<UnbatchedAccessNode>&) const
    {
        ++*calls;
    }
};

struct UnbatchedBlockAccessPropagationNode {
    int* calls = nullptr;

    static constexpr auto inputs()
    {
        return std::array {iv::compiled_sample_input("input")};
    }

    static constexpr auto outputs()
    {
        return std::array {iv::compiled_sample_output("output")};
    }

    void access_block(
        iv::AccessBlockContext<UnbatchedBlockAccessPropagationNode>&) const {}

    void propagate_block_access(
        iv::PropagateBlockAccessContext<UnbatchedBlockAccessPropagationNode>&) const
    {
        ++*calls;
    }
};

struct BatchedCallbacksNode {
    int* access_calls = nullptr;
    int* propagation_calls = nullptr;

    static constexpr auto inputs()
    {
        return std::array {iv::compiled_sample_input("input")};
    }

    static constexpr auto outputs()
    {
        return std::array {iv::compiled_sample_output("output")};
    }

    void tick_block(iv::TickBlockContext<BatchedCallbacksNode> const&) const {}

    void access_block_batch(iv::AccessBlockBatchContext<BatchedCallbacksNode>&) const
    {
        ++*access_calls;
    }

    void propagate_block_access_batch(
        iv::PropagateBlockAccessBatchContext<BatchedCallbacksNode>&) const
    {
        ++*propagation_calls;
    }
};

static_assert(std::same_as<
    iv::NodeCompiledState<CompiledSource>::Type,
    CompiledSource::CompiledState>);
static_assert(
    iv::details::node_compiled_state_size<CompiledSource>()
    == sizeof(CompiledSource::CompiledState));
static_assert(
    iv::details::node_compiled_state_alignment<CompiledSource>()
    == alignof(CompiledSource::CompiledState));
static_assert(std::same_as<
    decltype(iv::TickBlockContext<CompiledSource>::index),
    iv::SampleIndex>);
static_assert(iv::details::declares_compiled_sample_outputs_v<CompiledSource>);
static_assert(!iv::details::declares_compiled_sample_inputs_v<CompiledSource>);
static_assert(iv::details::declares_compiled_inputs_v<CompiledEvents>);
static_assert(iv::details::declares_compiled_outputs_v<CompiledEvents>);
static_assert(!iv::details::declares_compiled_sample_inputs_v<CompiledEvents>);
static_assert(!iv::details::declares_compiled_sample_outputs_v<CompiledEvents>);
static_assert(iv::details::declares_compiled_event_inputs_v<CompiledEvents>);
static_assert(iv::details::declares_compiled_event_outputs_v<CompiledEvents>);
static_assert(iv::details::compiled_dsp_node_declaration_is_valid_v<CompiledEvents>);
static_assert(iv::details::declares_compiled_inputs_v<CompiledInputOnly>);
static_assert(!iv::details::declares_compiled_outputs_v<CompiledInputOnly>);
static_assert(iv::details::compiled_dsp_node_declaration_is_valid_v<CompiledInputOnly>);
static_assert(iv::details::compiled_dsp_node_declaration_is_valid_v<CompiledEventSource>);
static_assert(iv::details::access_block_callback_kind_v<CompiledSource>
    == iv::CompiledPortCallbackKind::unbatched);
static_assert(iv::details::compiled_dsp_node_declaration_is_valid_v<CompiledSource>);
static_assert(iv::details::access_block_callback_kind_v<CompiledTransform>
    == iv::CompiledPortCallbackKind::batch);
static_assert(iv::details::propagate_block_access_callback_kind_v<CompiledTransform>
    == iv::CompiledPortCallbackKind::batch);
static_assert(iv::details::compiled_dsp_node_declaration_is_valid_v<CompiledTransform>);
static_assert(!iv::details::compiled_dsp_node_declaration_is_valid_v<MissingCompiledAccess>);
static_assert(iv::details::access_block_callback_kind_v<ConflictingCompiledAccess>
    == iv::CompiledPortCallbackKind::conflicting);
static_assert(!iv::details::compiled_dsp_node_declaration_is_valid_v<ConflictingCompiledAccess>);
static_assert(iv::details::propagate_block_access_callback_kind_v<MissingBlockAccessPropagation>
    == iv::CompiledPortCallbackKind::none);
static_assert(iv::details::compiled_dsp_node_declaration_is_valid_v<MissingBlockAccessPropagation>);
static_assert(iv::details::static_compiled_input_port_index<MixedCompiledPorts, "compiled">() == 0);
static_assert(iv::details::static_compiled_output_port_index<MixedCompiledPorts, "compiled">() == 0);
static_assert(iv::details::compiled_dsp_node_declaration_is_valid_v<MixedCompiledPorts>);
static_assert(iv::details::static_compiled_input_port_index<MixedCompiledKinds, "samples">() == 0);
static_assert(iv::details::static_compiled_event_input_port_index<MixedCompiledKinds, "events">() == 0);
static_assert(iv::details::static_compiled_output_port_index<MixedCompiledKinds, "samples-out">() == 0);
static_assert(iv::details::static_compiled_event_output_port_index<MixedCompiledKinds, "events-out">() == 0);
static_assert(std::same_as<
    decltype(iv::do_propagate_block_access_batched<UnbatchedBlockAccessPropagationNode>()),
    iv::PropagateBlockAccessBatchedOperation<UnbatchedBlockAccessPropagationNode>>);
static_assert(iv::details::node_compiler_operations<CompiledSource>()
    .access_block_batched != nullptr);
static_assert(iv::details::node_compiler_operations<CompiledSource>()
    .propagate_block_access_batched == nullptr);
static_assert(iv::details::node_compiler_operations<CompiledTransform>()
    .access_block_batched != nullptr);
static_assert(iv::details::node_compiler_operations<CompiledTransform>()
    .propagate_block_access_batched != nullptr);
static_assert(iv::details::node_compiler_operations<CompiledEvents>()
    .access_block_batched != nullptr);
static_assert(iv::details::node_compiler_operations<CompiledEvents>()
    .propagate_block_access_batched != nullptr);
static_assert(iv::details::node_compiler_operations<CompiledInputOnly>()
    .access_block_batched == nullptr);
static_assert(iv::details::node_compiler_operations<CompiledInputOnly>()
    .propagate_block_access_batched == nullptr);

struct InputData {
    std::array<iv::Sample, 4> samples {1.0f, 2.0f, 3.0f, 4.0f};
};

iv::CompiledSampleExtent input_extent(void const*)
{
    return {.begin = 0, .end = 4};
}

iv::Sample input_sample(void const* data, iv::SampleIndex index, std::size_t)
{
    return static_cast<InputData const*>(data)->samples[index];
}

struct OutputData {
    std::array<iv::Sample, 4> samples {};
};

void write_output_sample(
    void* data, iv::SampleIndex index, std::size_t, iv::Sample value)
{
    static_cast<OutputData*>(data)->samples[index] = value;
}

struct EventInputData {
    std::array<iv::TimedEvent, 4> events {{
        {.time = 3, .value = iv::TriggerEvent {}},
        {.time = 11, .value = iv::TriggerEvent {}},
        {.time = 17, .value = iv::TriggerEvent {}},
        {.time = 31, .value = iv::TriggerEvent {}},
    }};
};

iv::CompiledEventExtent event_input_extent(void const*)
{
    return {.begin = 0, .end = 40};
}

std::span<iv::TimedEvent const> read_input_events(
    void const* data, iv::SampleIndex begin, iv::SampleIndex end)
{
    auto const& events = static_cast<EventInputData const*>(data)->events;
    auto first = std::ranges::find_if(events, [begin](iv::TimedEvent const& event) {
        return event.time >= begin;
    });
    auto last = std::ranges::find_if(first, events.end(), [end](iv::TimedEvent const& event) {
        return event.time >= end;
    });
    return std::span<iv::TimedEvent const>(first, last);
}

struct EventOutputData {
    std::array<iv::TimedEvent, 4> events {};
    std::size_t count = 0;
};

void write_output_event(void* data, iv::TimedEvent const& event)
{
    auto& output = *static_cast<EventOutputData*>(data);
    IV_ASSERT(output.count < output.events.size(),
        "compiled event test output exceeded its fixed capture buffer");
    output.events[output.count++] = event;
}

struct PropagatedAccessCapture {
    std::size_t input_index = 0;
    iv::AccessRequestSet requests {};
};

struct PropagatedEventAccessCapture {
    std::size_t input_index = 0;
    iv::EventAccessRequestSet requests {};
};

void capture_propagated_access(
    void* data, std::size_t input_index, iv::AccessRequestSet const& requests)
{
    auto& capture = *static_cast<PropagatedAccessCapture*>(data);
    capture.input_index = input_index;
    capture.requests = requests;
}

void capture_propagated_event_access(
    void* data, std::size_t input_index, iv::EventAccessRequestSet const& requests)
{
    auto& capture = *static_cast<PropagatedEventAccessCapture*>(data);
    capture.input_index = input_index;
    capture.requests = requests;
}

struct DefaultPropagationCapture {
    std::size_t calls = 0;
    std::size_t input_index = 0;
    iv::AccessRequest request {};
};

struct DefaultEventPropagationCapture {
    std::size_t calls = 0;
    std::size_t input_index = 0;
    iv::EventAccessRequest request {};
};

struct NoPropagationCapture {
    std::size_t sample_calls = 0;
    std::size_t event_calls = 0;
};

void capture_default_propagated_access(
    void* data, std::size_t input_index, iv::AccessRequestSet const& requests)
{
    auto& capture = *static_cast<DefaultPropagationCapture*>(data);
    ++capture.calls;
    capture.input_index = input_index;
    if (!requests.requests().empty()) {
        capture.request = requests.requests().front();
    }
}

void capture_default_propagated_event_access(
    void* data, std::size_t input_index, iv::EventAccessRequestSet const& requests)
{
    auto& capture = *static_cast<DefaultEventPropagationCapture*>(data);
    ++capture.calls;
    capture.input_index = input_index;
    if (!requests.requests().empty()) {
        capture.request = requests.requests().front();
    }
}

void capture_unexpected_sample_propagation(
    void* data, std::size_t, iv::AccessRequestSet const&)
{
    ++static_cast<NoPropagationCapture*>(data)->sample_calls;
}

void capture_unexpected_event_propagation(
    void* data, std::size_t, iv::EventAccessRequestSet const&)
{
    ++static_cast<NoPropagationCapture*>(data)->event_calls;
}

TEST(CompiledDspPorts, CompiledInputsAreArbitrarilyAccessibleFromTickBlockWithoutAccessCallback)
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
    std::array<iv::InputPort, 1> realtime_inputs {{
        iv::InputPort(sample_shared, 7),
    }};

    std::array<iv::TimedEvent, 8> realtime_event_storage {};
    realtime_event_storage[0] = {.time = 8, .value = iv::TriggerEvent {}};
    realtime_event_storage[1] = {.time = 13, .value = iv::TriggerEvent {}};
    iv::EventSharedPortData event_shared {
        realtime_event_storage,
        0,
        2,
        iv::EventTypeId::trigger,
    };
    std::array<iv::EventInputPort, 1> realtime_event_inputs {{
        iv::EventInputPort(event_shared),
    }};

    InputData sample_input_data;
    EventInputData event_input_data;
    std::array<iv::CompiledInputPort, 1> compiled_inputs {{
        {.data = &sample_input_data,
         .extent_fn = &input_extent,
         .read_sample_fn = &input_sample},
    }};
    std::array<iv::CompiledEventInputPort, 1> compiled_event_inputs {{
        {.data = &event_input_data,
         .extent_fn = &event_input_extent,
         .read_events_fn = &read_input_events},
    }};

    iv::TickBlockContext<CompiledInputOnly> context {
        iv::TickContext<CompiledInputOnly> {
            .inputs = realtime_inputs,
            .event_inputs = realtime_event_inputs,
            .compiled_inputs = compiled_inputs,
            .compiled_event_inputs = compiled_event_inputs,
        },
        8,
        8,
    };

    auto samples = context.input<"samples">();
    EXPECT_FLOAT_EQ(samples[0], 10.0f);
    EXPECT_EQ(samples.extent().begin, 0u);
    EXPECT_EQ(samples.extent().end, 4u);
    EXPECT_FLOAT_EQ(static_cast<float>(samples.at(2)), 3.0f);

    auto events = context.input<"events">();
    auto current_block = events.events();
    ASSERT_EQ(current_block.size(), 2u);
    EXPECT_EQ(current_block[0].time, 8u);
    EXPECT_EQ(current_block[1].time, 13u);

    auto arbitrary = events.events(10, 20);
    ASSERT_EQ(arbitrary.size(), 2u);
    EXPECT_EQ(arbitrary[0].time, 11u);
    EXPECT_EQ(arbitrary[1].time, 17u);
}

TEST(CompiledDspPorts, AccessContextExposesOnlyDeclaredCompiledPorts)
{
    std::array<iv::AccessRequest, 1> requests {{
        {.begin = 4, .end = 20, .sample_count = 8},
    }};
    InputData input_data;
    OutputData output_data;
    std::array<iv::CompiledInputPort, 1> inputs {{
        {.data = &input_data,
         .extent_fn = &input_extent,
         .read_sample_fn = &input_sample,
         .neutral_value = -0.25f},
    }};
    std::array<iv::CompiledOutputPort, 1> outputs {{
        {.request_set = iv::AccessRequestSet {requests},
         .data = &output_data,
         .write_sample_fn = &write_output_sample},
    }};

    iv::AccessBlockContext<CompiledTransform> context {
        .inputs = inputs,
        .outputs = outputs,
    };

    auto input = context.input<"input">();
    EXPECT_EQ(input.size(), 4u);
    EXPECT_FLOAT_EQ(static_cast<float>(input.at(2)), 3.0f);
    EXPECT_FLOAT_EQ(static_cast<float>(input.at(8)), -0.25f);

    auto output = context.output<"output">();
    ASSERT_EQ(output.requests().requests().size(), 1u);
    EXPECT_EQ(output.requests().requests().front().begin, 4u);
    output.write(2, 0.75f);
    EXPECT_FLOAT_EQ(static_cast<float>(output_data.samples[2]), 0.75f);
}

TEST(CompiledDspPorts, AccessContextExposesLosslessCompiledEventIntervals)
{
    EventInputData input_data;
    EventOutputData output_data;
    std::array<iv::EventAccessRequest, 2> requested {{
        {.begin = 8, .end = 20},
        {.begin = 28, .end = 35},
    }};
    std::array<iv::CompiledEventInputPort, 1> inputs {{
        {.data = &input_data,
         .extent_fn = &event_input_extent,
         .read_events_fn = &read_input_events},
    }};
    std::array<iv::CompiledEventOutputPort, 1> outputs {{
        {.request_set = iv::EventAccessRequestSet {requested},
         .data = &output_data,
         .write_event_fn = &write_output_event},
    }};

    iv::AccessBlockContext<CompiledEvents> context {
        .event_inputs = inputs,
        .event_outputs = outputs,
    };

    auto input = context.input<"events-in">();
    auto events = input.events(8, 20);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].time, 11u);
    EXPECT_EQ(events[1].time, 17u);
    EXPECT_TRUE(input.events(35, 45).empty());

    auto output = context.output<"events-out">();
    ASSERT_EQ(output.requests().requests().size(), 2u);
    EXPECT_EQ(output.requests().requests()[1].begin, 28u);
    output.write({.time = 31, .value = iv::TriggerEvent {}});
    ASSERT_EQ(output_data.count, 1u);
    EXPECT_EQ(output_data.events[0].time, 31u);
}

TEST(CompiledDspPorts, OneAccessContextCanUseCompiledSampleAndEventPortsTogether)
{
    InputData sample_input_data;
    OutputData sample_output_data;
    EventInputData event_input_data;
    EventOutputData event_output_data;
    std::array<iv::CompiledInputPort, 1> sample_inputs {{
        {.data = &sample_input_data,
         .extent_fn = &input_extent,
         .read_sample_fn = &input_sample},
    }};
    std::array<iv::CompiledOutputPort, 1> sample_outputs {{
        {.data = &sample_output_data,
         .write_sample_fn = &write_output_sample},
    }};
    std::array<iv::CompiledEventInputPort, 1> event_inputs {{
        {.data = &event_input_data,
         .extent_fn = &event_input_extent,
         .read_events_fn = &read_input_events},
    }};
    std::array<iv::CompiledEventOutputPort, 1> event_outputs {{
        {.data = &event_output_data,
         .write_event_fn = &write_output_event},
    }};

    iv::AccessBlockContext<MixedCompiledKinds> context {
        .inputs = sample_inputs,
        .outputs = sample_outputs,
        .event_inputs = event_inputs,
        .event_outputs = event_outputs,
    };

    EXPECT_FLOAT_EQ(
        static_cast<float>(context.input<"samples">().at(1)), 2.0f);
    context.output<"samples-out">().write(2, 0.625f);
    EXPECT_FLOAT_EQ(
        static_cast<float>(sample_output_data.samples[2]), 0.625f);

    auto events = context.input<"events">().events(10, 20);
    ASSERT_EQ(events.size(), 2u);
    context.output<"events-out">().write(events.front());
    ASSERT_EQ(event_output_data.count, 1u);
    EXPECT_EQ(event_output_data.events[0].time, 11u);
}

TEST(CompiledDspPorts, PropagateBlockAccessContextForwardsRequestSetDirectlyToCompiledInput)
{
    std::array<iv::AccessRequest, 1> requested {{
        {.begin = 100, .end = 200, .sample_count = 20},
    }};
    std::array<iv::AccessRequestSet, 1> output_requests {{
        iv::AccessRequestSet {requested},
    }};
    PropagatedAccessCapture capture;
    iv::PropagateBlockAccessContext<CompiledTransform> context {
        .output_requests = output_requests,
        .user_data = &capture,
        .propagate_input_access = &capture_propagated_access,
    };

    context.input<"input">(context.output<"output">());

    EXPECT_EQ(capture.input_index, 0u);
    ASSERT_EQ(capture.requests.requests().size(), 1u);
    EXPECT_EQ(capture.requests.requests().front().begin, 100u);
    EXPECT_EQ(capture.requests.requests().front().end, 200u);
    EXPECT_EQ(capture.requests.requests().front().sample_count, 20u);
}

TEST(CompiledDspPorts, PropagateBlockAccessContextKeepsEventIntervalsDistinctFromSampleRequests)
{
    std::array<iv::EventAccessRequest, 2> requested {{
        {.begin = 100, .end = 140},
        {.begin = 200, .end = 220},
    }};
    std::array<iv::EventAccessRequestSet, 1> output_requests {{
        iv::EventAccessRequestSet {requested},
    }};
    PropagatedEventAccessCapture capture;
    iv::PropagateBlockAccessContext<CompiledEvents> context {
        .event_output_requests = output_requests,
        .user_data = &capture,
        .propagate_event_input_access = &capture_propagated_event_access,
    };

    context.input<"events-in">(context.output<"events-out">());

    EXPECT_EQ(capture.input_index, 0u);
    ASSERT_EQ(capture.requests.requests().size(), 2u);
    EXPECT_EQ(capture.requests.requests()[0].begin, 100u);
    EXPECT_EQ(capture.requests.requests()[0].end, 140u);
    EXPECT_EQ(capture.requests.requests()[1].begin, 200u);
    EXPECT_EQ(capture.requests.requests()[1].end, 220u);
}

TEST(CompiledDspPorts, MixedNodesUseCompactCompiledPortOrdinals)
{
    InputData input_data;
    OutputData output_data;
    std::array<iv::CompiledInputPort, 1> inputs {{
        {.data = &input_data, .extent_fn = &input_extent, .read_sample_fn = &input_sample},
    }};
    std::array<iv::CompiledOutputPort, 1> outputs {{
        {.data = &output_data, .write_sample_fn = &write_output_sample},
    }};
    iv::AccessBlockContext<MixedCompiledPorts> access_context {
        .inputs = inputs,
        .outputs = outputs,
    };

    EXPECT_FLOAT_EQ(
        static_cast<float>(access_context.input<"compiled">().at(3)), 4.0f);
    access_context.output<"compiled">().write(1, 0.5f);
    EXPECT_FLOAT_EQ(static_cast<float>(output_data.samples[1]), 0.5f);

    std::array<iv::AccessRequest, 1> requested {{
        {.begin = 50, .end = 70, .sample_count = 4},
    }};
    std::array<iv::AccessRequestSet, 1> output_requests {{
        iv::AccessRequestSet {requested},
    }};
    PropagatedAccessCapture capture;
    iv::PropagateBlockAccessContext<MixedCompiledPorts> propagation_context {
        .output_requests = output_requests,
        .user_data = &capture,
        .propagate_input_access = &capture_propagated_access,
    };
    propagation_context.input<"compiled">(
        propagation_context.output<"compiled">());

    EXPECT_EQ(capture.input_index, 0u);
    ASSERT_EQ(capture.requests.requests().size(), 1u);
    EXPECT_EQ(capture.requests.requests().front().begin, 50u);
}

TEST(CompiledDspPorts, CompiledStateHasASeparateExplicitAccessor)
{
    alignas(CompiledSource::CompiledState)
        std::array<std::byte, sizeof(CompiledSource::CompiledState)> storage {};
    auto* state = std::construct_at(
        reinterpret_cast<CompiledSource::CompiledState*>(storage.data()),
        CompiledSource::CompiledState {.calls = 3});
    iv::AccessBlockContext<CompiledSource> context {
        .compiled_state_storage = storage,
    };

    EXPECT_EQ(context.compiled_state().calls, 3);
    std::destroy_at(state);
}


TEST(CompiledDspPorts, TickAndAccessContextsShareWritableCompiledState)
{
    alignas(CompiledSource::CompiledState)
        std::array<std::byte, sizeof(CompiledSource::CompiledState)> storage {};
    auto* state = std::construct_at(
        reinterpret_cast<CompiledSource::CompiledState*>(storage.data()),
        CompiledSource::CompiledState {.calls = 1});

    iv::TickBlockContext<CompiledSource> tick {
        iv::TickContext<CompiledSource> {.compiled_state_storage = storage},
        0,
        16,
    };
    ++tick.compiled_state().calls;

    iv::AccessBlockContext<CompiledSource> access {
        .compiled_state_storage = storage,
    };
    access.compiled_state().calls += 3;

    EXPECT_EQ(state->calls, 5);
    std::destroy_at(state);
}

TEST(CompiledDspPorts, CompilerTickOperationReceivesCompiledStateStorage)
{
    alignas(TickCompiledStateRecorder::CompiledState)
        std::array<std::byte, sizeof(TickCompiledStateRecorder::CompiledState)> storage {};
    auto* state = std::construct_at(
        reinterpret_cast<TickCompiledStateRecorder::CompiledState*>(storage.data()));

    auto const& record = iv::details::node_compiler_record<TickCompiledStateRecorder>;
    EXPECT_EQ(record.compiled_state_size, sizeof(TickCompiledStateRecorder::CompiledState));
    EXPECT_EQ(record.compiled_state_alignment, alignof(TickCompiledStateRecorder::CompiledState));

    TickCompiledStateRecorder node;
    record.operations.tick_block(
        &node,
        iv::ReflectedNodeTickContext {.compiled_state = storage},
        0,
        16);

    EXPECT_EQ(state->writes, 1);
    std::destroy_at(state);
}

TEST(CompiledDspPorts, BatchedAccessNormalizesAnUnbatchedCallbackPerRequest)
{
    int calls = 0;
    UnbatchedAccessNode node {.calls = &calls};
    std::array<iv::AccessBlockContext<UnbatchedAccessNode>, 3> requests {};
    iv::AccessBlockBatchContext<UnbatchedAccessNode> batch {
        .unbatched_accesses = requests,
    };

    iv::do_access_block_batched(node, batch);

    EXPECT_EQ(calls, 3);
}

TEST(CompiledDspPorts, BatchedPropagationNormalizesAnUnbatchedCallbackPerRequest)
{
    int calls = 0;
    UnbatchedBlockAccessPropagationNode node {.calls = &calls};
    std::array<iv::PropagateBlockAccessContext<UnbatchedBlockAccessPropagationNode>, 2>
        propagations {};
    iv::PropagateBlockAccessBatchContext<UnbatchedBlockAccessPropagationNode> batch {
        .unbatched_propagations = propagations,
    };

    iv::do_propagate_block_access_batched<UnbatchedBlockAccessPropagationNode>()(node, batch);

    EXPECT_EQ(calls, 2);
}

TEST(CompiledDspPorts, MissingPropagationCallbackRequestsEntireCompiledInputExtent)
{
    MissingBlockAccessPropagation node;
    std::array<iv::CompiledSampleExtent, 1> input_extents {{
        {.begin = 25, .end = 89},
    }};
    DefaultPropagationCapture capture;
    iv::PropagateBlockAccessBatchContext<MissingBlockAccessPropagation> batch {
        .batch = {
            .input_extents = input_extents,
            .user_data = &capture,
            .propagate_input_access = &capture_default_propagated_access,
        },
    };

    iv::do_propagate_block_access_batched<MissingBlockAccessPropagation>()(
        node, batch);

    EXPECT_EQ(capture.calls, 1u);
    EXPECT_EQ(capture.input_index, 0u);
    EXPECT_EQ(capture.request.begin, 25u);
    EXPECT_EQ(capture.request.end, 89u);
    EXPECT_EQ(capture.request.sample_count, 64u);
}

TEST(CompiledDspPorts, MissingPropagationCallbackRequestsEntireCompiledEventInputExtent)
{
    MissingEventBlockAccessPropagation node;
    std::array<iv::CompiledEventExtent, 1> input_extents {{
        {.begin = 25, .end = 89},
    }};
    DefaultEventPropagationCapture capture;
    iv::PropagateBlockAccessBatchContext<MissingEventBlockAccessPropagation> batch {
        .batch = {
            .event_input_extents = input_extents,
            .user_data = &capture,
            .propagate_event_input_access = &capture_default_propagated_event_access,
        },
    };

    iv::do_propagate_block_access_batched<MissingEventBlockAccessPropagation>()(
        node, batch);

    EXPECT_EQ(capture.calls, 1u);
    EXPECT_EQ(capture.input_index, 0u);
    EXPECT_EQ(capture.request.begin, 25u);
    EXPECT_EQ(capture.request.end, 89u);
}

TEST(CompiledDspPorts, CompiledOutputWithNoCompiledInputsHasTrivialPropagation)
{
    CompiledEventSource node;
    iv::PropagateBlockAccessBatchContext<CompiledEventSource> batch {};

    EXPECT_NO_THROW(
        iv::do_propagate_block_access_batched<CompiledEventSource>()(node, batch));
}

TEST(CompiledDspPorts, CompiledInputsWithoutCompiledOutputsDoNotPropagateDemand)
{
    CompiledInputOnly node;
    std::array<iv::CompiledSampleExtent, 1> sample_input_extents {{
        {.begin = 0, .end = 64},
    }};
    std::array<iv::CompiledEventExtent, 1> event_input_extents {{
        {.begin = 0, .end = 64},
    }};
    NoPropagationCapture capture;
    iv::PropagateBlockAccessBatchContext<CompiledInputOnly> batch {
        .batch = {
            .input_extents = sample_input_extents,
            .event_input_extents = event_input_extents,
            .user_data = &capture,
            .propagate_input_access = &capture_unexpected_sample_propagation,
            .propagate_event_input_access = &capture_unexpected_event_propagation,
        },
    };

    iv::do_propagate_block_access_batched<CompiledInputOnly>()(node, batch);

    EXPECT_EQ(capture.sample_calls, 0u);
    EXPECT_EQ(capture.event_calls, 0u);
}

TEST(CompiledDspPorts, BatchedCallbacksReceiveOneWholeQueryContext)
{
    int access_calls = 0;
    int propagation_calls = 0;
    BatchedCallbacksNode node {
        .access_calls = &access_calls,
        .propagation_calls = &propagation_calls,
    };

    std::array<iv::AccessBlockContext<BatchedCallbacksNode>, 3> access_requests {};
    iv::AccessBlockBatchContext<BatchedCallbacksNode> access_context {
        .unbatched_accesses = access_requests,
    };
    iv::do_access_block_batched(node, access_context);

    std::array<iv::PropagateBlockAccessContext<BatchedCallbacksNode>, 2>
        propagations {};
    iv::PropagateBlockAccessBatchContext<BatchedCallbacksNode> propagation_context {
        .unbatched_propagations = propagations,
    };
    iv::do_propagate_block_access_batched<BatchedCallbacksNode>()(
        node, propagation_context);

    EXPECT_EQ(access_calls, 1);
    EXPECT_EQ(propagation_calls, 1);
}

TEST(CompiledDspPorts, CompilerRecordAnchorsNormalizedCompiledOperations)
{
    int access_calls = 0;
    int propagation_calls = 0;
    BatchedCallbacksNode node {
        .access_calls = &access_calls,
        .propagation_calls = &propagation_calls,
    };
    auto const operations =
        iv::details::node_compiler_operations<BatchedCallbacksNode>();
    ASSERT_NE(operations.access_block_batched, nullptr);
    ASSERT_NE(operations.propagate_block_access_batched, nullptr);

    iv::AccessBlockBatchContext<BatchedCallbacksNode> access_context {};
    operations.access_block_batched(&node, &access_context);

    iv::PropagateBlockAccessBatchContext<BatchedCallbacksNode>
        propagation_context {};
    operations.propagate_block_access_batched(&node, &propagation_context);

    EXPECT_EQ(access_calls, 1);
    EXPECT_EQ(propagation_calls, 1);
}

} // namespace
