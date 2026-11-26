#include <intravenous/global_index.h>
#include <intravenous/sample_positions.h>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>

namespace {

iv::GlobalIndex index(std::uint64_t bits)
{
    return iv::GlobalIndex::from_bits(bits);
}

TEST(FractionalGlobalIndex, AdvancesAndMeasuresAcrossTheCircularQ64Point64Domain)
{
    auto const begin = iv::FractionalGlobalIndex::from_parts(
        index(std::numeric_limits<std::uint64_t>::max()),
        std::numeric_limits<std::uint64_t>::max());
    auto const end = begin.advanced_by({.fraction = 2});

    EXPECT_EQ(end.whole(), index(0));
    EXPECT_EQ(end.fraction(), 1u);
    EXPECT_EQ(
        begin.forward_distance_to(end),
        (iv::FractionalGlobalExtent{.fraction = 2}));
}

TEST(FractionalGlobalIndexRange, ProjectsOneSharedIntervalOntoFasterAndSlowerLattices)
{
    auto const range = iv::FractionalGlobalIndexRange::from_global_range({
        .begin = index(10),
        .extent = 3,
    });

    auto const project_rate = range.try_project_onto_lattice(0);
    ASSERT_TRUE(project_rate.has_value());
    EXPECT_EQ(
        project_rate->begin,
        iv::FractionalGlobalIndex::from_global(index(10)));
    EXPECT_EQ(project_rate->count, 3u);

    auto const double_rate = range.try_project_onto_lattice(1);
    ASSERT_TRUE(double_rate.has_value());
    EXPECT_EQ(
        double_rate->begin,
        iv::FractionalGlobalIndex::from_global(index(10)));
    EXPECT_EQ(double_rate->count, 6u);

    auto const half_rate = range.try_project_onto_lattice(-1);
    ASSERT_TRUE(half_rate.has_value());
    EXPECT_EQ(
        half_rate->begin,
        iv::FractionalGlobalIndex::from_global(index(10)));
    EXPECT_EQ(half_rate->count, 2u);
}

TEST(FractionalGlobalIndex, InterpolationUsesExactGridCoordinatesAndWrapsSlowLattices)
{
    auto const three_quarters = std::uint64_t{3} << 62;
    auto const fast = iv::interpolate_global_lattice(
        iv::FractionalGlobalIndex::from_parts(index(12), three_quarters), 1);
    ASSERT_TRUE(fast);
    EXPECT_EQ(fast->lower, index(25));
    EXPECT_EQ(fast->upper, index(26));
    EXPECT_EQ(fast->fraction, std::uint64_t{1} << 63);

    auto const last = std::numeric_limits<std::uint64_t>::max();
    auto const half_before_zero = iv::FractionalGlobalIndex::from_parts(
        index(last), std::uint64_t{1} << 63);
    auto const slow = iv::interpolate_global_lattice(half_before_zero, -1);
    ASSERT_TRUE(slow);
    EXPECT_EQ(slow->lower, index(last >> 1));
    EXPECT_EQ(slow->upper, index(0));
    EXPECT_EQ(slow->fraction, three_quarters);

    auto const one = iv::interpolate_global_lattice(half_before_zero, -64);
    ASSERT_TRUE(one);
    EXPECT_EQ(one->lower, index(0));
    EXPECT_EQ(one->upper, index(0));
    EXPECT_FALSE(iv::interpolate_global_lattice(half_before_zero, 65));
}

TEST(FractionalGlobalIndexRange, SlowLatticeCanProduceZeroValuesForAnInvocation)
{
    auto const no_position = iv::FractionalGlobalIndexRange::from_global_range({
        .begin = index(11),
        .extent = 1,
    }).try_project_onto_lattice(-1);
    ASSERT_TRUE(no_position.has_value());
    EXPECT_EQ(no_position->count, 0u);

    auto const next_position = iv::FractionalGlobalIndexRange::from_global_range({
        .begin = index(11),
        .extent = 3,
    }).try_project_onto_lattice(-1);
    ASSERT_TRUE(next_position.has_value());
    EXPECT_EQ(
        next_position->begin,
        iv::FractionalGlobalIndex::from_global(index(12)));
    EXPECT_EQ(next_position->count, 1u);
}

TEST(FractionalGlobalIndexRange, EmptyProjectionKeepsItsLatticeInsertionIndex)
{
    auto const projected =
        iv::FractionalGlobalIndexRange::from_global_range({
            .begin = index(97),
            .extent = 1,
        }).try_project_onto_lattice(-2);

    ASSERT_TRUE(projected.has_value());
    EXPECT_EQ(
        projected->begin,
        iv::FractionalGlobalIndex::from_global(index(100)));
    EXPECT_EQ(projected->begin_index_bits, 25u);
    EXPECT_EQ(projected->count, 0u);
}

TEST(FractionalGlobalIndexRange, ProjectionPreservesFractionalAndWrappingIntervals)
{
    iv::FractionalGlobalIndexRange const fractional{
        .begin = iv::FractionalGlobalIndex::from_parts(
            index(10), std::uint64_t{1} << 62),
        .extent = {.whole = 1},
    };
    auto const halves = fractional.try_project_onto_lattice(1);
    ASSERT_TRUE(halves.has_value());
    EXPECT_EQ(
        halves->begin,
        iv::FractionalGlobalIndex::from_parts(
            index(10), std::uint64_t{1} << 63));
    EXPECT_EQ(halves->count, 2u);

    auto const wrapping = iv::FractionalGlobalIndexRange::from_global_range({
        .begin = index(std::numeric_limits<std::uint64_t>::max()),
        .extent = 2,
    });
    auto const project_rate = wrapping.try_project_onto_lattice(0);
    ASSERT_TRUE(project_rate.has_value());
    EXPECT_EQ(project_rate->count, 2u);

    auto const one_per_cycle = wrapping.try_project_onto_lattice(-64);
    ASSERT_TRUE(one_per_cycle.has_value());
    EXPECT_EQ(
        one_per_cycle->begin,
        iv::FractionalGlobalIndex::from_global(index(0)));
    EXPECT_EQ(one_per_cycle->count, 1u);
}

TEST(FractionalGlobalIndexRange, RejectsUnsupportedOrUnrepresentableProjectionCounts)
{
    auto const one_project_sample =
        iv::FractionalGlobalIndexRange::from_global_range({
            .begin = index(0),
            .extent = 1,
        });
    EXPECT_FALSE(one_project_sample.try_project_onto_lattice(65).has_value());
    EXPECT_FALSE(one_project_sample.try_project_onto_lattice(-65).has_value());
    EXPECT_FALSE(one_project_sample.try_project_onto_lattice(64).has_value());
}

TEST(SamplePositionRun, ClipsALargeRunWithoutChangingItsPhase)
{
    iv::SamplePositionRun const run{index(100), 100'000'000, 8};
    std::size_t intersections = 0;
    iv::for_each_sample_position_intersection(run, {index(109), 16}, [&](auto result) {
        ++intersections;
        EXPECT_EQ(result, (iv::SamplePositionRun{index(116), 2, 8}));
        EXPECT_EQ(result.position(1), index(124));
    });
    EXPECT_EQ(intersections, 1u);
    iv::for_each_sample_position_intersection(run, {index(109), 7}, [&](auto) {
        ADD_FAILURE() << "The range contains no selected positions";
    });
}

TEST(SamplePositionRun, ClipsACircularIntersectionInRunOrder)
{
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    iv::SamplePositionRun const run{index(100), 4, 8};
    std::array<iv::SamplePositionRun, 2> parts;
    std::size_t part_count = 0;
    iv::for_each_sample_position_intersection(run, {index(124), max - 15}, [&](auto part) {
        ASSERT_LT(part_count, parts.size());
        parts[part_count++] = part;
    });
    ASSERT_EQ(part_count, parts.size());
    EXPECT_EQ(parts[0], (iv::SamplePositionRun{index(100), 1, 8}));
    EXPECT_EQ(parts[1], (iv::SamplePositionRun{index(124), 1, 8}));
}

TEST(SamplePositionRun, ValidatesExtentAndSupportsWrapAndSinglePositions)
{
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    EXPECT_FALSE((iv::SamplePositionRun{index(0), 3, max}).valid());
    EXPECT_FALSE((iv::SamplePositionRun{index(0), 1, 0}).valid());
    iv::SamplePositionRun const run{index(max - 3), 2, 4};
    ASSERT_TRUE(run.valid());
    EXPECT_EQ(run.position(1), index(0));
    iv::for_each_sample_position_intersection(run, {index(max), 2}, [&](auto part) {
        EXPECT_EQ(part, (iv::SamplePositionRun{index(0), 1, 4}));
    });

    std::array const empty_runs{iv::SamplePositionRun{index(0), 0, 8}};
    std::array const single{index(71)};
    EXPECT_TRUE((iv::SamplePositionsView{empty_runs}).empty());
    EXPECT_FALSE((iv::SamplePositionsView{empty_runs, single}).empty());
}

TEST(SamplePositionRun, IntersectsDifferentStridesAcrossTheSignedSeam)
{
    auto const origin = iv::GlobalIndex::from_signed(INT64_MAX - 5);
    iv::SamplePositionRun const source{origin, 20, 6};
    iv::SamplePositionRun const requested{origin.advanced_by(3), 12, 9};
    auto const common = iv::intersect_clipped_sample_run(source, requested);
    ASSERT_TRUE(common);
    EXPECT_EQ(*common, (iv::SamplePositionRun{origin.advanced_by(12), 6, 18}));
    EXPECT_FALSE(iv::intersect_clipped_sample_run(
        source, {origin.advanced_by(1), 12, 9}));
}

} // namespace
