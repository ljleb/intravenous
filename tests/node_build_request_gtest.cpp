#include <intravenous/graph/reflected_node_description.h>
#include <intravenous/node/coverage_port_context.h>
#include <intravenous/node/tick.h>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace {

iv::GlobalIndex index(std::int64_t value)
{
    return iv::GlobalIndex::from_signed(value);
}

iv::GlobalIndexRange range(std::int64_t begin, std::int64_t end)
{
    return iv::GlobalIndexRange::from_begin_and_end(index(begin), index(end));
}

iv::IndexCoverage index_coverage(
    std::initializer_list<std::pair<std::int64_t, std::int64_t>> ranges)
{
    iv::IndexCoverage result;
    for (auto const [begin, end] : ranges) result.include(range(begin, end));
    return result;
}

iv::ReflectedContinuousPortStorageBinding sample_storage_binding(
    std::span<iv::Sample> samples)
{
    auto geometry = iv::continuous_value_storage_geometry(
        iv::ContinuousValueTypeId::sample, std::nullopt);
    IV_ASSERT(geometry.has_value(),
        "Sample must have a registered continuous storage geometry");
    iv::ReflectedContinuousPortStorageBinding result{
        .geometry = *geometry,
        .value_capacity = samples.size(),
        .channel_layout = iv::mono_planar_channel_layout,
    };
    result.channels[0] = {
        .storage = reinterpret_cast<std::byte*>(samples.data()),
        .value_capacity = samples.size(),
        .value_stride_bytes = sizeof(iv::Sample),
    };
    return result;
}

iv::ReflectedContinuousInputPortBinding sample_input_binding(
    std::span<iv::Sample> samples)
{
    return {
        .storage = sample_storage_binding(samples),
    };
}

iv::ReflectedContinuousOutputPortBinding sample_output_binding(
    std::span<iv::Sample> samples)
{
    return {
        .storage = sample_storage_binding(samples),
    };
}

struct PointwisePhysicalOriginNode {
    static constexpr auto inputs()
    {
        return std::array{
            iv::input<iv::Sample>("input", iv::InputAccess::sequential)};
    }

    static constexpr auto outputs()
    {
        return std::array{
            iv::output<iv::Sample>("output", iv::OutputProduction::tick)};
    }

    void tick(iv::TickSampleContext<PointwisePhysicalOriginNode> const& ctx) const
    {
        ctx.output<"output">().push(ctx.input<"input">().get());
    }
};

TEST(TickPointwiseExecution, AppliesValueOffsetToEachPortPhysicalOrigin)
{
    std::array<iv::Sample, 16> input_storage{};
    std::array<iv::Sample, 16> output_storage{};
    input_storage[5] = 10.0f;
    input_storage[6] = 11.0f;
    input_storage[7] = 12.0f;
    input_storage[8] = 13.0f;

    std::array inputs{sample_input_binding(std::span{input_storage})};
    std::array outputs{sample_output_binding(std::span{output_storage})};
    inputs[0].sequential_begin_ordinal = 5;
    outputs[0].sequential_begin_ordinal = 13;

    iv::do_tick_block(
        PointwisePhysicalOriginNode{},
        iv::TickBlockContext<PointwisePhysicalOriginNode>{
            iv::TickContext<PointwisePhysicalOriginNode>{
                iv::details::TickPortBindings<PointwisePhysicalOriginNode>{
                    .continuous_inputs = inputs,
                    .continuous_outputs = outputs,
                },
            },
            iv::FractionalGlobalIndex::from_global(index(1000)),
            4,
        });

    EXPECT_FLOAT_EQ(output_storage[13], 10.0f);
    EXPECT_FLOAT_EQ(output_storage[14], 11.0f);
    EXPECT_FLOAT_EQ(output_storage[15], 12.0f);
    EXPECT_FLOAT_EQ(output_storage[0], 13.0f);
    EXPECT_FLOAT_EQ(output_storage[5], 0.0f);
}

struct RequestNode {
    std::size_t latency = 0;

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>("level", iv::InputAccess::sequential)};
    }
    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("signal", iv::OutputProduction::tick, iv::OutputRetention::persisted)};
    }
    std::size_t internal_latency() const { return latency; }
    std::optional<std::size_t> ttl_samples() const { return 64; }
    bool can_skip_block() const { return true; }
    void tick_block(auto const&) const {}
};

struct ReductionNode {
    inline static std::size_t fft_neutral_initializations = 0;

    static constexpr iv::Sample multiply_samples(
        iv::Sample left, iv::Sample right) noexcept
    {
        return left * right;
    }

    static constexpr void multiply_fft_bins(
        std::span<iv::FFTBin> destination,
        std::span<iv::FFTBin const> source) noexcept
    {
        for (std::size_t index = 0; index < destination.size(); ++index) {
            destination[index] *= source[index];
        }
    }

    static void initialize_fft_multiply_neutral(
        std::span<iv::FFTBin> destination) noexcept
    {
        ++fft_neutral_initializations;
        for (auto& bin : destination) bin = iv::FFTBin{1.0f, 0.0f};
    }

    static constexpr auto inputs()
    {
        return std::tuple{
            iv::event_input(
                "event", iv::EventTypeId::trigger, iv::InputAccess::sequential),
            iv::input<iv::Sample>("region", iv::InputAccess::region),
            iv::input<iv::Sample, iv::FFTBlock>(
                "product",
                iv::InputAccess::sequential,
                {
                    .neutral_value = 1.0f,
                    .combine_values = &multiply_samples,
                },
                {
                    .neutral_value = &initialize_fft_multiply_neutral,
                    .combine_values = &multiply_fft_bins,
                }),
        };
    }

    static constexpr auto outputs() { return std::tuple{}; }
    void tick_block(auto const&) const {}
};

struct OrderingConstraintShape {
    static constexpr auto inputs()
    {
        return std::tuple{
            iv::input<iv::Sample>("region-in", iv::InputAccess::region),
            iv::input<iv::Sample>("coverage-in", iv::InputAccess::coverage),
            iv::event_input(
                "event-region-in", iv::EventTypeId::trigger,
                iv::InputAccess::region),
            iv::event_input(
                "event-coverage-in", iv::EventTypeId::trigger,
                iv::InputAccess::coverage),
        };
    }

    static constexpr auto outputs()
    {
        return std::tuple{
            iv::output<iv::Sample>(
                "region-out", iv::OutputProduction::region),
            iv::output<iv::Sample>(
                "coverage-out", iv::OutputProduction::coverage),
            iv::event_output(
                "event-region-out", iv::EventTypeId::trigger,
                iv::OutputProduction::region),
            iv::event_output(
                "event-coverage-out", iv::EventTypeId::trigger,
                iv::OutputProduction::coverage),
        };
    }
};

template<class Context>
concept RegionInputHasOrdering = requires(Context const& context) {
    context.template input<"region-in">().ordering();
};

template<class Context>
concept CoverageInputHasOrdering = requires(Context const& context) {
    context.template input<"coverage-in">().ordering();
};

template<class Context>
concept EventRegionInputHasOrdering = requires(Context const& context) {
    context.template input<"event-region-in">().ordering();
};

template<class Context>
concept EventCoverageInputHasOrdering = requires(Context const& context) {
    context.template input<"event-coverage-in">().ordering();
};

template<class Context>
concept RegionOutputHasOrdering = requires(Context const& context) {
    context.template output<"region-out">().ordering();
};

template<class Context>
concept CoverageOutputHasOrdering = requires(Context const& context) {
    context.template output<"coverage-out">().ordering();
};

template<class Context>
concept EventRegionOutputHasOrdering = requires(Context const& context) {
    context.template output<"event-region-out">().ordering();
};

template<class Context>
concept EventCoverageOutputHasOrdering = requires(Context const& context) {
    context.template output<"event-coverage-out">().ordering();
};

using OrderingConstraintContext =
    iv::ConstrainPortsContext<OrderingConstraintShape>;

static_assert(!RegionInputHasOrdering<OrderingConstraintContext>);
static_assert(CoverageInputHasOrdering<OrderingConstraintContext>);
static_assert(!EventRegionInputHasOrdering<OrderingConstraintContext>);
static_assert(EventCoverageInputHasOrdering<OrderingConstraintContext>);
static_assert(!RegionOutputHasOrdering<OrderingConstraintContext>);
static_assert(CoverageOutputHasOrdering<OrderingConstraintContext>);
static_assert(!EventRegionOutputHasOrdering<OrderingConstraintContext>);
static_assert(EventCoverageOutputHasOrdering<OrderingConstraintContext>);

struct ReplayableNode {
    static constexpr bool intrinsically_replayable = true;
    std::size_t latency = 0;

    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>("input", iv::InputAccess::sequential)};
    }
    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("output", iv::OutputProduction::tick)};
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
        return std::array{iv::input<iv::Sample>("input", iv::InputAccess::sequential)};
    }
    void constrain_ports(
        iv::ConstrainPortsContext<ReplayWithInputHistory>& ctx) const
    {
        ctx.input<"input">().history() = 1;
    }
    void tick(iv::TickSampleContext<ReplayWithInputHistory> const&) const {}
};

struct ReplayWithOutputLatency {
    static constexpr bool intrinsically_replayable = true;
    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("output", iv::OutputProduction::tick)};
    }
    void constrain_ports(
        iv::ConstrainPortsContext<ReplayWithOutputLatency>& ctx) const
    {
        ctx.output<"output">().latency() = 1;
    }
    void tick(iv::TickSampleContext<ReplayWithOutputLatency> const&) const {}
};

static_assert(iv::details::replay_declaration_is_valid_v<ReplayableNode>);
static_assert(!iv::details::replay_declaration_is_valid_v<ReplayWithState>);
static_assert(iv::details::replay_declaration_is_valid_v<ReplayWithInputHistory>);
static_assert(iv::details::replay_declaration_is_valid_v<ReplayWithOutputLatency>);

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
concept HasDefaultValue = requires(Config const& config) {
    config.default_value;
};
template<class Config>
concept HasNeutralValue = requires(Config const& config) {
    config.neutral_value;
};
template<class Config>
concept HasCombineValues = requires(Config const& config) {
    config.combine_values;
};
template<class Config>
concept HasHistory = requires(Config const& config) { config.history; };
template<class Config>
concept HasLatency = requires(Config const& config) { config.latency; };
template<class Config>
concept HasRetention = requires(Config const& config) { config.retention; };

static_assert(std::same_as<
    decltype(iv::input<iv::Sample>("", iv::InputAccess::sequential)),
    iv::SequentialInputConfig<iv::Sample>>);
static_assert(std::same_as<
    decltype(iv::input<iv::Sample>("", iv::InputAccess::coverage)),
    iv::CoverageInputConfig<iv::Sample>>);
static_assert(std::same_as<
    decltype(iv::event_input({}, iv::EventTypeId::empty, iv::InputAccess::sequential)),
    iv::SequentialEventInputConfig>);
static_assert(std::same_as<
    decltype(iv::event_input({}, iv::EventTypeId::empty, iv::InputAccess::coverage)),
    iv::CoverageEventInputConfig>);
static_assert(std::same_as<
    decltype(iv::output<iv::Sample>("", iv::OutputProduction::tick)),
    iv::TickOutputConfig<iv::Sample>>);
static_assert(std::same_as<
    decltype(iv::output<iv::Sample>("", iv::OutputProduction::coverage)),
    iv::CoverageOutputConfig<iv::Sample>>);
static_assert(std::same_as<
    decltype(iv::event_output({}, iv::EventTypeId::empty, iv::OutputProduction::tick)),
    iv::TickEventOutputConfig>);
static_assert(std::same_as<
    decltype(iv::event_output({}, iv::EventTypeId::empty, iv::OutputProduction::coverage)),
    iv::CoverageEventOutputConfig>);
static_assert(HasSampleRange<
    iv::SampleContinuousValueExtension::SequentialInputOptions>);
static_assert(HasDefaultValue<
    iv::SampleContinuousValueExtension::SequentialInputOptions>);
static_assert(HasDefaultValue<
    iv::FractionalGlobalIndexContinuousValueExtension::SequentialInputOptions>);
static_assert(!HasDefaultValue<
    iv::FFTBlockContinuousValueExtension::SequentialInputOptions>);
static_assert(!HasSampleRange<
    iv::FFTBlockContinuousValueExtension::SequentialInputOptions>);
static_assert(!HasDefaultValue<
    iv::FFTBlockContinuousValueExtension::SequentialInputMetadata>);
static_assert(!HasSampleRange<
    iv::FFTBlockContinuousValueExtension::SequentialInputMetadata>);
static_assert(HasNeutralValue<
    iv::SampleContinuousValueExtension::SequentialInputOptions>);
static_assert(HasNeutralValue<
    iv::FractionalGlobalIndexContinuousValueExtension::SequentialInputOptions>);
static_assert(HasNeutralValue<
    iv::FFTBlockContinuousValueExtension::SequentialInputOptions>);
static_assert(!HasNeutralValue<
    iv::FFTBlockContinuousValueExtension::SequentialInputMetadata>);
static_assert(std::same_as<
    decltype(iv::FFTBlockContinuousValueExtension::SequentialInputOptions{}
        .neutral_value),
    iv::FFTBlockContinuousValueExtension::neutral_value_type>);
static_assert(HasCombineValues<
    iv::SampleContinuousValueExtension::SequentialInputOptions>);
static_assert(HasCombineValues<
    iv::FractionalGlobalIndexContinuousValueExtension::SequentialInputOptions>);
static_assert(HasCombineValues<
    iv::FFTBlockContinuousValueExtension::SequentialInputOptions>);
static_assert(!HasSampleRange<iv::SequentialEventInputConfig>);
static_assert(!HasHistory<iv::SequentialInputConfig<iv::Sample>>);
static_assert(!HasHistory<iv::TickOutputConfig<iv::Sample>>);
static_assert(!HasLatency<iv::TickOutputConfig<iv::Sample>>);
static_assert(!HasHistory<iv::CoverageInputConfig<iv::Sample>>);
static_assert(!HasLatency<iv::CoverageOutputConfig<iv::Sample>>);
static_assert(HasRetention<iv::TickOutputConfig<iv::Sample>>);
static_assert(HasRetention<iv::TickEventOutputConfig>);
static_assert(iv::is_random_access(iv::input<iv::Sample>("background", iv::InputAccess::coverage)));
static_assert(iv::is_tock(iv::event_output("background", iv::EventTypeId::trigger, iv::OutputProduction::coverage)));
static_assert(!iv::is_persisted(iv::output<iv::Sample>("ephemeral", iv::OutputProduction::coverage)));
static_assert(iv::is_persisted(iv::output<iv::Sample>("persisted", iv::OutputProduction::coverage, iv::OutputRetention::persisted)));
static_assert(iv::is_persisted(iv::output<iv::Sample>("persisted", iv::OutputProduction::tick, iv::OutputRetention::persisted)));

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

TEST(NodeBuildRequest, RetainsTypedSequentialReductionOperationsInCompilerRecord)
{
    auto const operations =
        iv::details::node_compiler_operations<ReductionNode>();
    ASSERT_NE(operations.initialize_sequential_input, nullptr);
    ASSERT_NE(operations.combine_sequential_input_values, nullptr);

    std::array<iv::Sample, 3> sample_destination{};
    std::array<iv::Sample, 3> sample_source{2.0f, 3.0f, 4.0f};
    EXPECT_TRUE(operations.initialize_sequential_input(
        1,
        iv::ContinuousValueTypeId::sample,
        iv::details::SequentialInputInitialization::neutral,
        reinterpret_cast<std::byte*>(sample_destination.data()),
        sizeof(sample_destination),
        1));
    EXPECT_EQ(sample_destination,
        (std::array<iv::Sample, 3>{1.0f, 1.0f, 1.0f}));
    EXPECT_TRUE(operations.combine_sequential_input_values(
        1,
        iv::ContinuousValueTypeId::sample,
        reinterpret_cast<std::byte*>(sample_destination.data()),
        reinterpret_cast<std::byte const*>(sample_source.data()),
        sizeof(sample_destination),
        1));
    EXPECT_EQ(sample_destination, sample_source);

    std::array<iv::FFTBin, 6> fft_destination{};
    std::array<iv::FFTBin, 6> fft_source{
        iv::FFTBin{2.0f, 0.0f},
        iv::FFTBin{3.0f, 0.0f},
        iv::FFTBin{4.0f, 0.0f},
        iv::FFTBin{5.0f, 0.0f},
        iv::FFTBin{6.0f, 0.0f},
        iv::FFTBin{7.0f, 0.0f},
    };
    ReductionNode::fft_neutral_initializations = 0;
    EXPECT_TRUE(operations.initialize_sequential_input(
        1,
        iv::ContinuousValueTypeId::fft_block,
        iv::details::SequentialInputInitialization::neutral,
        reinterpret_cast<std::byte*>(fft_destination.data()),
        sizeof(fft_destination),
        3));
    EXPECT_EQ(ReductionNode::fft_neutral_initializations, 2u);
    EXPECT_EQ(fft_destination,
        (std::array<iv::FFTBin, 6>{
            iv::FFTBin{1.0f, 0.0f},
            iv::FFTBin{1.0f, 0.0f},
            iv::FFTBin{1.0f, 0.0f},
            iv::FFTBin{1.0f, 0.0f},
            iv::FFTBin{1.0f, 0.0f},
            iv::FFTBin{1.0f, 0.0f},
        }));
    EXPECT_TRUE(operations.combine_sequential_input_values(
        1,
        iv::ContinuousValueTypeId::fft_block,
        reinterpret_cast<std::byte*>(fft_destination.data()),
        reinterpret_cast<std::byte const*>(fft_source.data()),
        sizeof(fft_destination),
        3));
    EXPECT_EQ(fft_destination, fft_source);

    auto const default_fft_options =
        iv::FFTBlockContinuousValueExtension::SequentialInputOptions{};
    fft_destination.fill(iv::FFTBin{9.0f, -4.0f});
    EXPECT_TRUE(
        iv::FFTBlockContinuousValueExtension::initialize_sequential_neutral(
            default_fft_options, fft_destination, 3));
    EXPECT_EQ(fft_destination, (std::array<iv::FFTBin, 6>{}));

    EXPECT_FALSE(operations.initialize_sequential_input(
        0,
        iv::ContinuousValueTypeId::sample,
        iv::details::SequentialInputInitialization::neutral,
        reinterpret_cast<std::byte*>(sample_destination.data()),
        sizeof(sample_destination),
        1));
}

TEST(IndexCoverage, CanonicalizesAndCoalescesRanges)
{
    auto selected = index_coverage({
        {40, 50}, {10, 20}, {18, 30}, {30, 35}, {70, 70},
    });

    ASSERT_EQ(selected.size(), 2u);
    EXPECT_EQ(selected, index_coverage({{10, 35}, {40, 50}}));
    EXPECT_TRUE(selected.contains(index(12)));
    EXPECT_FALSE(selected.contains(index(35)));
    EXPECT_TRUE(selected.contains(range(12, 34)));
    EXPECT_FALSE(selected.contains(range(25, 45)));
}

TEST(IndexCoverage, IncludeExcludeIntersectionAndDifferenceStayCanonical)
{
    auto selected = index_coverage({{10, 20}, {30, 40}});
    selected.include(range(20, 30));
    ASSERT_EQ(selected.size(), 1u);
    EXPECT_EQ(selected, index_coverage({{10, 40}}));

    selected.exclude(range(15, 35));
    ASSERT_EQ(selected.size(), 2u);
    EXPECT_EQ(selected, index_coverage({{10, 15}, {35, 40}}));

    auto const other = index_coverage({{12, 38}, {50, 60}});
    EXPECT_EQ(selected.intersection(other),
        index_coverage({{12, 15}, {35, 38}}));
    EXPECT_EQ(other.difference(selected),
        index_coverage({{15, 35}, {50, 60}}));
}

TEST(IndexCoverage, CopiesShareAnImmutableValueUntilMutation)
{
    auto const original = index_coverage({{-20, -10}, {10, 20}});
    auto changed = original;

    changed.include(range(-10, 10));
    ASSERT_EQ(changed.size(), 1u);
    EXPECT_EQ(changed, index_coverage({{-20, 20}}));

    ASSERT_EQ(original.size(), 2u);
    EXPECT_EQ(original, index_coverage({{-20, -10}, {10, 20}}));

    changed.exclude(range(-5, 5));
    EXPECT_EQ(changed, index_coverage({{-20, -5}, {5, 20}}));
    EXPECT_EQ(original, index_coverage({{-20, -10}, {10, 20}}));
}

TEST(IndexCoverage, CircularRangesSplitOnlyAtTheSignedOrderingSeam)
{
    auto const begin = index(std::numeric_limits<std::int64_t>::max() - 1);
    iv::IndexCoverage selected{iv::GlobalIndexRange{begin, 4}};

    ASSERT_EQ(selected.size(), 2u);
    EXPECT_EQ(selected, (iv::IndexCoverage{
        iv::GlobalIndexRange{
            index(std::numeric_limits<std::int64_t>::min()), 2},
        iv::GlobalIndexRange{begin, 2},
    }));
    for (std::uint64_t offset = 0; offset < 4; ++offset) {
        EXPECT_TRUE(selected.contains(begin.advanced_by(offset)));
    }

    selected.exclude(iv::GlobalIndexRange{begin.advanced_by(1), 2});
    EXPECT_TRUE(selected.contains(begin));
    EXPECT_FALSE(selected.contains(begin.advanced_by(1)));
    EXPECT_FALSE(selected.contains(begin.advanced_by(2)));
    EXPECT_TRUE(selected.contains(begin.advanced_by(3)));
}

TEST(IndexCoverage, RepresentsTheWholeDomainWithoutAFullDomainRangeSentinel)
{
    constexpr auto half = iv::GlobalIndex::extent_type{1} << 63;
    iv::IndexCoverage full;
    full.include(iv::GlobalIndexRange{
        iv::GlobalIndex::from_signed(
            std::numeric_limits<std::int64_t>::min()),
        half,
    });
    full.include(iv::GlobalIndexRange{
        iv::GlobalIndex::from_signed(0), half});

    EXPECT_EQ(full.size(), 2u);
    EXPECT_TRUE(full.contains(iv::GlobalIndex::from_signed(
        std::numeric_limits<std::int64_t>::min())));
    EXPECT_TRUE(full.contains(iv::GlobalIndex::from_signed(-1)));
    EXPECT_TRUE(full.contains(iv::GlobalIndex::from_signed(0)));
    EXPECT_TRUE(full.contains(iv::GlobalIndex::from_signed(
        std::numeric_limits<std::int64_t>::max())));
    EXPECT_TRUE(full.contains(iv::GlobalIndexRange{}));
    EXPECT_FALSE(full.intersects(iv::GlobalIndexRange{}));

    full.exclude(iv::GlobalIndexRange{
        iv::GlobalIndex::from_signed(-2), 4});
    EXPECT_TRUE(full.contains(iv::GlobalIndex::from_signed(-3)));
    EXPECT_FALSE(full.contains(iv::GlobalIndex::from_signed(-2)));
    EXPECT_FALSE(full.contains(iv::GlobalIndex::from_signed(-1)));
    EXPECT_FALSE(full.contains(iv::GlobalIndex::from_signed(0)));
    EXPECT_FALSE(full.contains(iv::GlobalIndex::from_signed(1)));
    EXPECT_TRUE(full.contains(iv::GlobalIndex::from_signed(2)));
}

struct BackgroundSource {
    struct TockState {
        int calls = 0;
        std::size_t sample_rate = 0;
    };

    static constexpr auto outputs()
    {
        return std::array {iv::output<iv::Sample>("signal", iv::OutputProduction::coverage)};
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
        ctx.output<"signal">().publish_coverage(index_coverage({{10, 20}}));
    }
};

struct BackgroundTransform {
    mutable std::size_t tock_sample_rate = 0;
    mutable std::size_t forward_sample_rate = 0;
    mutable std::size_t reverse_sample_rate = 0;

    static constexpr auto inputs()
    {
        return std::tuple {
            iv::input<iv::Sample>("samples", iv::InputAccess::coverage),
            iv::event_input("events", iv::EventTypeId::trigger, iv::InputAccess::coverage),
        };
    }

    static constexpr auto outputs()
    {
        return std::tuple {
            iv::output<iv::Sample>("samples-out", iv::OutputProduction::coverage, iv::OutputRetention::ephemeral),
            iv::event_output(
                "events-out", iv::EventTypeId::trigger,
                iv::OutputProduction::coverage,
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
        return std::tuple {
            iv::input<iv::Sample>("samples", iv::InputAccess::coverage),
            iv::event_input("events", iv::EventTypeId::trigger, iv::InputAccess::coverage),
        };
    }
    void tick_block(iv::TickBlockContext<BackgroundInputOnly> const&) const {}
};

struct MissingBackgroundTock {
    static constexpr auto outputs()
    {
        return std::array {iv::output<iv::Sample>("output", iv::OutputProduction::coverage)};
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
        return std::array {iv::output<iv::Sample>("output", iv::OutputProduction::coverage)};
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
        return std::tuple {
            iv::output<iv::Sample>("samples", iv::OutputProduction::tick, iv::OutputRetention::persisted),
            iv::event_output(
                "events", iv::EventTypeId::trigger,
                iv::OutputProduction::tick,
                iv::OutputRetention::persisted),
        };
    }

    void tick_block(
        iv::TickBlockContext<PersistedRealtimeNode> const& context) const
    {
        auto samples = context.output<0>();
        for (std::size_t i = 0; i < context.block_size; ++i) {
            samples[i] = static_cast<float>(i);
        }
        context.output<1>().push(iv::TriggerEvent{}, 2);
    }
};

struct DispositionNode {
    bool void_outputs = false;

    static constexpr auto outputs()
    {
        return std::tuple {
            iv::output<iv::Sample>("samples", iv::OutputProduction::tick, iv::OutputRetention::persisted),
            iv::event_output(
                "events", iv::EventTypeId::trigger,
                iv::OutputProduction::tick,
                iv::OutputRetention::persisted),
        };
    }

    void tick_block(
        iv::TickBlockContext<DispositionNode> const& context) const
    {
        if (!void_outputs) return;
        context.output<"samples">().write_void();
        context.output<"events">().write_void();
    }
};

struct EmptyPersistedEventBlockNode {
    static constexpr auto outputs()
    {
        return std::array {iv::event_output(
            "events", iv::EventTypeId::trigger,
            iv::OutputProduction::tick,
            iv::OutputRetention::persisted)};
    }

    void tick_block(
        iv::TickBlockContext<EmptyPersistedEventBlockNode> const& context) const
    {
        context.output<"events">().push_block({});
    }
};

struct DefaultSkippedPersistedNode {
    static constexpr auto outputs()
    {
        return std::tuple {
            iv::output<iv::Sample>("samples", iv::OutputProduction::tick, iv::OutputRetention::persisted),
            iv::event_output(
                "events", iv::EventTypeId::trigger,
                iv::OutputProduction::tick,
                iv::OutputRetention::persisted),
        };
    }

    void tick_block(
        iv::TickBlockContext<DefaultSkippedPersistedNode> const&) const
    {}
};

struct EphemeralOutputNode {
    static constexpr auto outputs()
    {
        return std::tuple {
            iv::output<iv::Sample>("samples", iv::OutputProduction::tick),
            iv::event_output("events", iv::EventTypeId::trigger, iv::OutputProduction::tick),
        };
    }

    void tick_block(
        iv::TickBlockContext<EphemeralOutputNode> const& context) const
    {
        context.output<"samples">()[0] = 1.0f;
        context.output<"events">().push(iv::TriggerEvent{}, 0);
    }
};

struct MixedAccessIndexNode {
    static constexpr auto inputs()
    {
        return std::tuple{};
    }

    static constexpr auto outputs()
    {
        return std::tuple {
            iv::output<iv::Sample>("background-sample", iv::OutputProduction::coverage),
            iv::output<iv::Sample>("realtime-sample", iv::OutputProduction::tick),
            iv::event_output("background-event", iv::EventTypeId::trigger, iv::OutputProduction::coverage),
            iv::event_output("realtime-event", iv::EventTypeId::trigger, iv::OutputProduction::tick),
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
    iv::FractionalGlobalIndex>);
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
static_assert(iv::is_persisted(
    iv::port_config_at<0>(PersistedRealtimeNode::outputs())));
static_assert(iv::is_persisted(
    iv::port_config_at<1>(PersistedRealtimeNode::outputs())));
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

iv::Sample read_sample(void const* data, iv::GlobalIndex index, std::size_t)
{
    return static_cast<SampleData const*>(data)->values[
        static_cast<std::size_t>(index.bits())];
}

struct SampleWrites { std::array<iv::Sample, 8> values {}; };
void write_sample(void* data, iv::GlobalIndex index, std::size_t, iv::Sample value)
{
    static_cast<SampleWrites*>(data)->values[
        static_cast<std::size_t>(index.bits())] = value;
}

struct EventData {
    std::array<iv::TimedEvent, 4> events {{
        {.time = index(2), .value = iv::TriggerEvent{}},
        {.time = index(3), .value = iv::TriggerEvent{}},
        {.time = index(10), .value = iv::TriggerEvent{}},
        {.time = index(12), .value = iv::TriggerEvent{}},
    }};
};

void for_each_event(
    void const* data,
    iv::GlobalIndexRange selected,
    void* visitor_data,
    iv::RandomAccessEventInputPort::VisitEvent visitor)
{
    for (auto const& event : static_cast<EventData const*>(data)->events) {
        if (selected.contains(event.time)) visitor(visitor_data, event);
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
    iv::OutputDisposition sample_disposition{};
    std::array sample_outputs{
        sample_output_binding(std::span{sample_storage})};
    sample_outputs[0].sequential_begin_ordinal = 128;
    std::array<iv::TimedEvent, 16> event_storage {};
    iv::EventSharedPortData event_shared {
        event_storage, 0, 0, iv::EventTypeId::trigger,
    };
    iv::OutputDisposition event_disposition{};
    std::array event_outputs {iv::EventOutputPort(
        event_shared,
        iv::EventTypeId::trigger,
        0,
        0,
        nullptr)};

    iv::details::TickRecordingBindings<PersistedRealtimeNode> recording{};
    recording.sample_outputs[0] = &sample_disposition;
    recording.event_outputs[0] = &event_disposition;

    iv::do_tick_block(
        PersistedRealtimeNode{},
        iv::TickBlockContext<PersistedRealtimeNode>{
            iv::TickContext<PersistedRealtimeNode>{
                iv::details::TickPortBindings<PersistedRealtimeNode>{
                    .continuous_outputs = sample_outputs,
                    .event_outputs = event_outputs,
                    .recording = recording,
                },
            },
            iv::FractionalGlobalIndex::from_global(index(128)),
            4,
        });

    EXPECT_FLOAT_EQ(sample_storage[128], 0.0f);
    EXPECT_FLOAT_EQ(sample_storage[129], 1.0f);
    EXPECT_FLOAT_EQ(sample_storage[130], 2.0f);
    EXPECT_FLOAT_EQ(sample_storage[131], 3.0f);
    EXPECT_EQ(sample_disposition, iv::OutputDisposition::written);
    ASSERT_EQ(event_shared.write_index, 1u);
    EXPECT_EQ(event_storage[0].time, index(130));
    EXPECT_EQ(event_disposition, iv::OutputDisposition::written);
}

TEST(BackgroundDspPorts, ReflectedOutputsExposeFinalInvocationDisposition)
{
    std::array<iv::Sample, 8> sample_storage{};
    std::array sample_bindings{
        sample_output_binding(std::span{sample_storage})};
    auto& sample = sample_bindings[0];

    struct EventStorage {
        std::size_t count = 0;
        std::array<iv::TimedEvent, 8> events{};
    } event_storage;
    std::array<iv::ReflectedEventOutputPortBinding, 1> event_bindings{{{
        .storage = {
            .storage = reinterpret_cast<std::byte*>(&event_storage),
            .count_offset = offsetof(EventStorage, count),
            .events_offset = offsetof(EventStorage, events),
            .event_capacity = event_storage.events.size(),
            .type = iv::EventTypeId::trigger,
        },
        .source_type = iv::EventTypeId::trigger,
    }}};
    auto context = iv::ReflectedNodeTickContext{
        .continuous_output_bindings = sample_bindings,
        .event_output_bindings = event_bindings,
    };
    auto const operations =
        iv::details::node_compiler_operations<DispositionNode>();

    sample.disposition = iv::OutputDisposition::written;
    event_bindings[0].disposition = iv::OutputDisposition::written;
    DispositionNode untouched;
    operations.tick_block(&untouched, context, index(0), 4);
    EXPECT_EQ(sample.disposition, iv::OutputDisposition::untouched);
    EXPECT_EQ(event_bindings[0].disposition,
        iv::OutputDisposition::untouched);

    DispositionNode voiding{.void_outputs = true};
    operations.tick_block(&voiding, context, index(4), 4);
    EXPECT_EQ(sample.disposition, iv::OutputDisposition::voided);
    EXPECT_EQ(event_bindings[0].disposition, iv::OutputDisposition::voided);
}

TEST(BackgroundDspPorts, ExplicitEmptyEventBlockIsAnOrdinaryWrite)
{
    struct EventStorage {
        std::size_t count = 0;
        std::array<iv::TimedEvent, 8> events{};
    } storage;
    std::array<iv::ReflectedEventOutputPortBinding, 1> bindings{{{
        .storage = {
            .storage = reinterpret_cast<std::byte*>(&storage),
            .count_offset = offsetof(EventStorage, count),
            .events_offset = offsetof(EventStorage, events),
            .event_capacity = storage.events.size(),
            .type = iv::EventTypeId::trigger,
        },
        .source_type = iv::EventTypeId::trigger,
    }}};
    auto const operations =
        iv::details::node_compiler_operations<EmptyPersistedEventBlockNode>();
    EmptyPersistedEventBlockNode node;

    operations.tick_block(
        &node,
        iv::ReflectedNodeTickContext{.event_output_bindings = bindings},
        index(16),
        4);

    EXPECT_EQ(bindings[0].disposition, iv::OutputDisposition::written);
    EXPECT_EQ(storage.count, 0u);
}

TEST(BackgroundDspPorts, GeneratedPersistedSkipOutputsAreOrdinaryWrites)
{
    std::array<iv::Sample, 8> samples{};
    std::array bindings{
        sample_output_binding(std::span{samples})};
    struct EventStorage {
        std::size_t count = 0;
        std::array<iv::TimedEvent, 8> events{};
    } event_storage;
    std::array<iv::ReflectedEventOutputPortBinding, 1> event_bindings{{{
        .storage = {
            .storage = reinterpret_cast<std::byte*>(&event_storage),
            .count_offset = offsetof(EventStorage, count),
            .events_offset = offsetof(EventStorage, events),
            .event_capacity = event_storage.events.size(),
            .type = iv::EventTypeId::trigger,
        },
        .source_type = iv::EventTypeId::trigger,
    }}};
    auto const operations =
        iv::details::node_compiler_operations<
            DefaultSkippedPersistedNode>();
    DefaultSkippedPersistedNode node;

    operations.skip_block(
        &node,
        iv::ReflectedNodeTickContext{
            .continuous_output_bindings = bindings,
            .event_output_bindings = event_bindings,
        },
        index(0),
        4);

    EXPECT_EQ(bindings[0].disposition, iv::OutputDisposition::written);
    EXPECT_EQ(
        event_bindings[0].disposition, iv::OutputDisposition::written);
    EXPECT_EQ(event_storage.count, 0u);
}

TEST(BackgroundDspPorts, EphemeralWritesDoNotObserveOutputDisposition)
{
    std::array<iv::Sample, 8> samples{};
    std::array sample_bindings{
        sample_output_binding(std::span{samples})};
    sample_bindings[0].disposition = iv::OutputDisposition::voided;

    struct EventStorage {
        std::size_t count = 0;
        std::array<iv::TimedEvent, 8> events{};
    } events;
    std::array<iv::ReflectedEventOutputPortBinding, 1> event_bindings{{{
        .storage = {
            .storage = reinterpret_cast<std::byte*>(&events),
            .count_offset = offsetof(EventStorage, count),
            .events_offset = offsetof(EventStorage, events),
            .event_capacity = events.events.size(),
            .type = iv::EventTypeId::trigger,
        },
        .source_type = iv::EventTypeId::trigger,
        .disposition = iv::OutputDisposition::voided,
    }}};
    auto const operations =
        iv::details::node_compiler_operations<EphemeralOutputNode>();
    EphemeralOutputNode node;

    operations.tick_block(
        &node,
        iv::ReflectedNodeTickContext{
            .continuous_output_bindings = sample_bindings,
            .event_output_bindings = event_bindings,
        },
        index(0),
        4);

    EXPECT_EQ(
        sample_bindings[0].disposition, iv::OutputDisposition::voided);
    EXPECT_EQ(
        event_bindings[0].disposition, iv::OutputDisposition::voided);
    EXPECT_FLOAT_EQ(samples[0], 1.0f);
    EXPECT_EQ(events.count, 1u);

    operations.skip_block(
        &node,
        iv::ReflectedNodeTickContext{
            .continuous_output_bindings = sample_bindings,
            .event_output_bindings = event_bindings,
        },
        index(4),
        4);
    EXPECT_EQ(
        sample_bindings[0].disposition, iv::OutputDisposition::voided);
    EXPECT_EQ(
        event_bindings[0].disposition, iv::OutputDisposition::voided);
    EXPECT_EQ(events.count, 0u);
}

TEST(BackgroundDspPorts, BackgroundInputsRemainAvailableFromTickBlock)
{
    std::array<iv::Sample, 8> realtime_samples {
        10.0f, 11.0f, 12.0f, 13.0f, 14.0f, 15.0f, 16.0f, 17.0f,
    };
    std::array realtime_inputs{
        sample_input_binding(std::span{realtime_samples})};

    std::array<iv::TimedEvent, 8> realtime_event_storage {};
    realtime_event_storage[0] = {
        .time = index(8), .value = iv::TriggerEvent {}};
    realtime_event_storage[1] = {
        .time = index(13), .value = iv::TriggerEvent {}};
    iv::EventSharedPortData event_shared {
        realtime_event_storage,
        0,
        2,
        iv::EventTypeId::trigger,
    };
    std::array realtime_event_inputs {iv::EventInputPort(event_shared)};

    auto const sample_coverage = index_coverage({{0, 8}});
    auto const event_coverage = index_coverage({{0, 4}, {10, 14}});
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
            iv::details::TickPortBindings<BackgroundInputOnly>{
                .continuous_inputs = realtime_inputs,
                .event_inputs = realtime_event_inputs,
                .random_access_inputs = random_access_inputs,
                .random_access_event_inputs = random_access_event_inputs,
            },
        },
        iv::FractionalGlobalIndex::from_global(index(8)),
        8,
    };

    auto samples = context.input<0>();
    EXPECT_FLOAT_EQ(static_cast<float>(samples[0]), 10.0f);
    EXPECT_EQ(samples.coverage(), sample_coverage);
    EXPECT_FLOAT_EQ(static_cast<float>(samples.at(index(2))), 2.0f);

    auto events = context.input<1>();
    auto const current_block = events.events();
    ASSERT_EQ(current_block.size(), 2u);
    EXPECT_EQ(current_block[0].time, index(8));
    EXPECT_EQ(current_block[1].time, index(13));

    std::vector<iv::GlobalIndex> arbitrary;
    events.for_each(range(10, 14), [&](iv::TimedEvent const& event) {
        arbitrary.push_back(event.time);
    });
    EXPECT_EQ(arbitrary,
        (std::vector<iv::GlobalIndex>{index(10), index(12)}));
}

TEST(BackgroundDspPorts, TockContextExposesCoverageAndSampleEventBindings)
{
    auto const input_coverage = index_coverage({{0, 8}});
    auto const event_coverage = index_coverage({{0, 4}, {10, 14}});
    auto const sample_request = index_coverage({{2, 5}});
    auto const event_request = index_coverage({{10, 14}});
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
    EXPECT_FLOAT_EQ(
        static_cast<float>(context.input<"samples">().at(index(3))), 3.0f);
    context.output<"samples-out">().write(index(3), 0.75f);
    EXPECT_FLOAT_EQ(static_cast<float>(sample_writes.values[3]), 0.75f);

    std::vector<iv::GlobalIndex> visited;
    context.input<"events">().for_each(
        range(10, 14), [&](iv::TimedEvent const& event) {
        visited.push_back(event.time);
    });
    EXPECT_EQ(visited,
        (std::vector<iv::GlobalIndex>{index(10), index(12)}));
    context.output<"events-out">().write(
        {.time = index(12), .value = iv::TriggerEvent{}});
    ASSERT_EQ(event_writes.events.size(), 1u);
    EXPECT_EQ(event_writes.events.front().time, index(12));
}

struct PublishedCoverage {
    iv::IndexCoverage coverage;
    iv::IndexCoverage changed;
};
void publish_coverage(void* data, iv::IndexCoverage const& coverage)
{
    static_cast<PublishedCoverage*>(data)->coverage = coverage;
}
void publish_changed(void* data, iv::IndexCoverage const& changed)
{
    static_cast<PublishedCoverage*>(data)->changed.include(changed);
}

struct RequiredCoverage { iv::IndexCoverage required; };
void publish_required(void* data, iv::IndexCoverage const& required)
{
    static_cast<RequiredCoverage*>(data)->required.include(required);
}

TEST(BackgroundDspPorts, ForwardAndReverseContextsKeepExactDisjointCoverage)
{
    auto const sample_coverage = index_coverage({{10, 20}, {40, 50}});
    auto const sample_changed = index_coverage({{12, 14}, {45, 47}});
    auto const event_coverage = index_coverage({{100, 120}});
    auto const event_changed = index_coverage({{108, 109}});
    PublishedCoverage sample_output;
    PublishedCoverage event_output;
    iv::IndexCoverage const empty_output_coverage;
    std::array input_changes {iv::InputCoverageChange{
        .coverage_value = &sample_coverage, .changed_value = &sample_changed,
    }};
    std::array event_input_changes {iv::InputCoverageChange{
        .coverage_value = &event_coverage, .changed_value = &event_changed,
    }};
    std::array output_changes {iv::OutputCoverageChange{
        .data = &sample_output,
        .previous_coverage_value = &empty_output_coverage,
        .publish_coverage_value = &publish_coverage,
        .publish_changed_value = &publish_changed,
    }};
    std::array event_output_changes {iv::OutputCoverageChange{
        .data = &event_output,
        .previous_coverage_value = &empty_output_coverage,
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

    auto const sample_demand = index_coverage({{13, 15}, {42, 44}});
    auto const event_demand = index_coverage({{110, 112}});
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
        return std::array {iv::input<iv::Sample>("input", iv::InputAccess::coverage)};
    }
    static constexpr auto outputs()
    {
        return std::array {iv::output<iv::Sample>("output", iv::OutputProduction::coverage)};
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
    auto const input_coverage = index_coverage({{10, 20}, {50, 60}});
    auto const input_changed = index_coverage({{12, 13}});
    auto const previous_output = index_coverage({{100, 120}, {200, 220}});
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
    auto const input_coverage = index_coverage({{10, 20}, {50, 60}});
    auto const output_demand = index_coverage({{100, 101}});
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

    operations.tick_block(
        &node, iv::ReflectedNodeTickContext {}, index(0), 16);
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
