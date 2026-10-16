#include <intravenous/global_index.h>

#include <gtest/gtest.h>

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

} // namespace
