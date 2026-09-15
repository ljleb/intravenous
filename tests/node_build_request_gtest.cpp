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
        return std::array{iv::sample_input(input_name)};
    }

    auto outputs() const
    {
        return std::array{iv::sample_output(output_name)};
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

static_assert(std::same_as<decltype(iv::sample_input()), iv::InputConfig>);
static_assert(std::same_as<
    decltype(iv::event_input({}, iv::EventTypeId::empty)), iv::InputConfig>);
static_assert(std::same_as<decltype(iv::sample_output()), iv::OutputConfig>);
static_assert(std::same_as<
    decltype(iv::event_output({}, iv::EventTypeId::empty)), iv::OutputConfig>);
static_assert(HasSampleRange<iv::SampleInputProperties>);
static_assert(!HasSampleRange<iv::EventInputProperties>);
static_assert(iv::sample_properties(iv::InputConfig {}).neutral_value.value == 0.0f);

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
        return std::array {iv::sample_output("signal", {}, true)};
    }

    void access_block(iv::AccessBlockContext<CompiledSource>&) const {}
};

struct CompiledTransform {
    static constexpr auto inputs()
    {
        return std::array {iv::sample_input("input", {}, true)};
    }

    static constexpr auto outputs()
    {
        return std::array {iv::sample_output("output", {}, true)};
    }

    void access_block_batch(iv::AccessBlockBatchContext<CompiledTransform>&) const {}
    void propagate_block_access_batch(
        iv::PropagateBlockAccessBatchContext<CompiledTransform>&) const {}
};


struct CompiledEvents {
    static constexpr auto inputs()
    {
        return std::array {iv::event_input("events-in", iv::EventTypeId::trigger, true)};
    }

    static constexpr auto outputs()
    {
        return std::array {iv::event_output("events-out", iv::EventTypeId::trigger, true)};
    }
};

struct MissingCompiledAccess {
    static constexpr auto outputs()
    {
        return std::array {iv::sample_output("output", {}, true)};
    }
};

struct ConflictingCompiledAccess {
    static constexpr auto outputs()
    {
        return std::array {iv::sample_output("output", {}, true)};
    }

    void access_block(iv::AccessBlockContext<ConflictingCompiledAccess>&) const {}
    void access_block_batch(
        iv::AccessBlockBatchContext<ConflictingCompiledAccess>&) const {}
};

struct MissingBlockAccessPropagation {
    static constexpr auto inputs()
    {
        return std::array {iv::sample_input("input", {}, true)};
    }

    static constexpr auto outputs()
    {
        return std::array {iv::sample_output("output", {}, true)};
    }

    void access_block(iv::AccessBlockContext<MissingBlockAccessPropagation>&) const {}
};

struct MixedCompiledPorts {
    static constexpr auto inputs()
    {
        return std::array {
            iv::sample_input("realtime"),
            iv::sample_input("compiled", {}, true),
        };
    }

    static constexpr auto outputs()
    {
        return std::array {
            iv::sample_output("realtime"),
            iv::sample_output("compiled", {}, true),
        };
    }

    void access_block(iv::AccessBlockContext<MixedCompiledPorts>&) const {}
    void propagate_block_access(
        iv::PropagateBlockAccessContext<MixedCompiledPorts>&) const {}
};

struct UnbatchedAccessNode {
    int* calls = nullptr;

    static constexpr auto outputs()
    {
        return std::array {iv::sample_output("output", {}, true)};
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
        return std::array {iv::sample_input("input", {}, true)};
    }

    static constexpr auto outputs()
    {
        return std::array {iv::sample_output("output", {}, true)};
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
        return std::array {iv::sample_input("input", {}, true)};
    }

    static constexpr auto outputs()
    {
        return std::array {iv::sample_output("output", {}, true)};
    }

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
static_assert(std::same_as<
    decltype(iv::TickBlockContext<CompiledSource>::index),
    iv::SampleIndex>);
static_assert(iv::details::declares_compiled_sample_outputs_v<CompiledSource>);
static_assert(!iv::details::declares_compiled_sample_inputs_v<CompiledSource>);
static_assert(iv::details::declares_compiled_inputs_v<CompiledEvents>);
static_assert(iv::details::declares_compiled_outputs_v<CompiledEvents>);
static_assert(!iv::details::declares_compiled_sample_inputs_v<CompiledEvents>);
static_assert(!iv::details::declares_compiled_sample_outputs_v<CompiledEvents>);
static_assert(iv::details::compiled_dsp_node_declaration_is_valid_v<CompiledEvents>);
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
static_assert(std::same_as<
    decltype(iv::do_propagate_block_access_batched<UnbatchedBlockAccessPropagationNode>()),
    iv::PropagateBlockAccessBatchedOperation<UnbatchedBlockAccessPropagationNode>>);

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

struct PropagatedAccessCapture {
    std::size_t input_index = 0;
    iv::AccessRequestSet requests {};
};

void capture_propagated_access(
    void* data, std::size_t input_index, iv::AccessRequestSet const& requests)
{
    auto& capture = *static_cast<PropagatedAccessCapture*>(data);
    capture.input_index = input_index;
    capture.requests = requests;
}

struct DefaultPropagationCapture {
    std::size_t calls = 0;
    std::size_t input_index = 0;
    iv::AccessRequest request {};
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

} // namespace
