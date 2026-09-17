#include <intravenous/graph/realtime_port_planning.h>
#include <intravenous/ports.h>

#include <gtest/gtest.h>

#include <array>
#include <stdexcept>

namespace {

TEST(EventConversionRegistry, OnlyObjectiveDirectedConversionsAreAvailable)
{
    auto const registry = iv::EventConversionRegistry::instance();

    EXPECT_NO_THROW(registry.plan(iv::EventTypeId::midi, iv::EventTypeId::trigger));
    EXPECT_NO_THROW(registry.plan(iv::EventTypeId::midi, iv::EventTypeId::boundary));
    EXPECT_NO_THROW(registry.plan(iv::EventTypeId::midi, iv::EventTypeId::empty));
    EXPECT_NO_THROW(registry.plan(iv::EventTypeId::boundary, iv::EventTypeId::trigger));
    EXPECT_NO_THROW(registry.plan(iv::EventTypeId::boundary, iv::EventTypeId::empty));
    EXPECT_NO_THROW(registry.plan(iv::EventTypeId::trigger, iv::EventTypeId::empty));

    EXPECT_THROW(
        registry.plan(iv::EventTypeId::trigger, iv::EventTypeId::boundary),
        std::logic_error);
    EXPECT_THROW(
        registry.plan(iv::EventTypeId::trigger, iv::EventTypeId::midi),
        std::logic_error);
    EXPECT_THROW(
        registry.plan(iv::EventTypeId::boundary, iv::EventTypeId::midi),
        std::logic_error);
    EXPECT_THROW(
        registry.plan(iv::EventTypeId::empty, iv::EventTypeId::trigger),
        std::logic_error);
    EXPECT_THROW(
        registry.plan(iv::EventTypeId::empty, iv::EventTypeId::boundary),
        std::logic_error);
    EXPECT_THROW(
        registry.plan(iv::EventTypeId::empty, iv::EventTypeId::midi),
        std::logic_error);
}

TEST(EventConversionRegistry, AllowedConversionsDoNotInventLaterTimestamps)
{
    auto const plan = iv::EventConversionRegistry::instance().plan(
        iv::EventTypeId::boundary,
        iv::EventTypeId::trigger);
    iv::TimedEvent source{
        .time = 123,
        .value = iv::BoundaryEvent{.is_begin = true},
    };

    std::array<iv::TimedEvent, 2> converted{};
    std::size_t count = 0;
    iv::EventConversionRegistry::instance().convert(
        plan, source, [&](iv::TimedEvent const& event) {
            converted[count++] = event;
        });

    ASSERT_EQ(count, 1u);
    EXPECT_EQ(converted[0].time, 123u);
    EXPECT_TRUE(std::holds_alternative<iv::TriggerEvent>(converted[0].value));
}

TEST(RealtimePortWindow, IncludesHistoryAndLatencyWithExclusiveEnd)
{
    auto const window = iv::realtime_port_window(100, 16, 4, 3);
    EXPECT_EQ(window.begin, 96u);
    EXPECT_EQ(window.end, 119u);
    EXPECT_TRUE(window.contains(96));
    EXPECT_TRUE(window.contains(118));
    EXPECT_FALSE(window.contains(95));
    EXPECT_FALSE(window.contains(119));

    auto const clamped = iv::realtime_port_window(2, 16, 8, 0);
    EXPECT_EQ(clamped.begin, 0u);
    EXPECT_EQ(clamped.end, 18u);
}

TEST(EventOutputPort, RejectsRealtimeEventsOutsideActiveWindow)
{
    std::array<iv::TimedEvent, 8> storage{};
    iv::EventSharedPortData shared(
        storage, 0, 0, iv::EventTypeId::trigger);
    iv::EventOutputPort output(shared, iv::EventTypeId::trigger, 4, 3);

    EXPECT_THROW(
        output.push(iv::TimedEvent{
            .time = 100,
            .value = iv::TriggerEvent{},
        }),
        std::logic_error);

    output.begin_block(100, 16);
    EXPECT_NO_THROW(output.push(iv::TimedEvent{
        .time = 96,
        .value = iv::TriggerEvent{},
    }));
    EXPECT_NO_THROW(output.push(iv::TimedEvent{
        .time = 118,
        .value = iv::TriggerEvent{},
    }));
    EXPECT_THROW(output.push(iv::TimedEvent{
        .time = 95,
        .value = iv::TriggerEvent{},
    }), std::logic_error);
    EXPECT_THROW(output.push(iv::TimedEvent{
        .time = 119,
        .value = iv::TriggerEvent{},
    }), std::logic_error);
    output.end_block();

    EXPECT_EQ(shared.write_index, 2u);
}

TEST(EventOutputPort, ExplicitBlockPushUsesSameWindow)
{
    std::array<iv::TimedEvent, 8> storage{};
    iv::EventSharedPortData shared(
        storage, 0, 0, iv::EventTypeId::trigger);
    iv::EventOutputPort output(shared, iv::EventTypeId::trigger, 2, 1);

    EXPECT_NO_THROW(output.push(
        iv::TriggerEvent{}, 8, 100, 8)); // time 108; latency permits it
    EXPECT_THROW(output.push(
        iv::TriggerEvent{}, 9, 100, 8), std::logic_error); // end is 109
}

TEST(SampleConnectionImplementationChooser, UsesSimpleConservativePolicy)
{
    using Kind = iv::SampleConnectionImplementationKind;

    EXPECT_EQ(iv::choose_sample_connection_implementation({
        .direct_implementation_legal = true,
    }), Kind::direct);

    EXPECT_EQ(iv::choose_sample_connection_implementation({
        .direct_implementation_legal = true,
        .requires_materialization = true,
    }), Kind::transient_materialization);

    EXPECT_EQ(iv::choose_sample_connection_implementation({
        .retained_frames = 32,
        .channel_count = 2,
        .value_size_bytes = sizeof(iv::Sample),
    }), Kind::compact_persistent_carry);

    EXPECT_EQ(iv::choose_sample_connection_implementation({
        .retained_frames = 4096,
        .channel_count = 2,
        .value_size_bytes = sizeof(iv::Sample),
    }), Kind::persistent_ring);

    EXPECT_EQ(iv::choose_sample_connection_implementation({
        .feedback = true,
    }), Kind::feedback_ring);
}

TEST(SampleConnectionImplementationChooser, CrossoverIsExplicitlyTunable)
{
    auto requirements = iv::SampleConnectionImplementationRequirements{
        .retained_frames = 128,
        .channel_count = 1,
        .value_size_bytes = sizeof(iv::Sample),
    };

    EXPECT_EQ(iv::choose_sample_connection_implementation(
        requirements,
        {.compact_carry_max_bytes = 1024}),
        iv::SampleConnectionImplementationKind::compact_persistent_carry);
    EXPECT_EQ(iv::choose_sample_connection_implementation(
        requirements,
        {.compact_carry_max_bytes = 128}),
        iv::SampleConnectionImplementationKind::persistent_ring);
}

TEST(EventConnectionImplementationChooser, UsesUnknownDensityConservatively)
{
    using Kind = iv::EventConnectionImplementationKind;

    EXPECT_EQ(iv::choose_event_connection_implementation({
        .direct_implementation_legal = true,
    }), Kind::direct);

    EXPECT_EQ(iv::choose_event_connection_implementation({
        .requires_materialization = true,
    }), Kind::transient_sequence);

    EXPECT_EQ(iv::choose_event_connection_implementation({
        .retained_window_samples = 64,
        .estimated_retained_events = 8,
    }), Kind::compact_persistent_carry);

    EXPECT_EQ(iv::choose_event_connection_implementation({
        .retained_window_samples = 64,
    }), Kind::persistent_ring);

    EXPECT_EQ(iv::choose_event_connection_implementation({
        .feedback = true,
    }), Kind::feedback_ring);
}

TEST(EventConnectionImplementationChooser, CrossoverIsExplicitlyTunable)
{
    auto requirements = iv::EventConnectionImplementationRequirements{
        .retained_window_samples = 64,
        .estimated_retained_events = 12,
    };

    EXPECT_EQ(iv::choose_event_connection_implementation(
        requirements,
        {.compact_carry_max_events = 16}),
        iv::EventConnectionImplementationKind::compact_persistent_carry);
    EXPECT_EQ(iv::choose_event_connection_implementation(
        requirements,
        {.compact_carry_max_events = 8}),
        iv::EventConnectionImplementationKind::persistent_ring);
}

} // namespace
