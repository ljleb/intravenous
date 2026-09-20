#include <intravenous/graph/realtime_port_planning.h>
#include <intravenous/ports.h>

#include <gtest/gtest.h>

#include <array>
#include <limits>
#include <optional>
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

    EXPECT_TRUE(iv::EventConversionRegistry::is_nonexpanding(
        registry.plan(iv::EventTypeId::midi, iv::EventTypeId::trigger)));
    EXPECT_TRUE(iv::EventConversionRegistry::is_nonexpanding(
        registry.plan(iv::EventTypeId::midi, iv::EventTypeId::boundary)));
    EXPECT_TRUE(iv::EventConversionRegistry::is_nonexpanding(
        registry.plan(iv::EventTypeId::boundary, iv::EventTypeId::trigger)));

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

TEST(EventBufferSizing, FractionalRatesProduceDeterministicStaticCapacities)
{
    EXPECT_TRUE(iv::is_valid_event_buffer_rate(0.0));
    EXPECT_TRUE(iv::is_valid_event_buffer_rate(0.24));
    EXPECT_FALSE(iv::is_valid_event_buffer_rate(-0.01));
    EXPECT_FALSE(iv::is_valid_event_buffer_rate(
        std::numeric_limits<double>::infinity()));

    ASSERT_EQ(iv::event_count_for_sample_span(0.25, 64),
        std::optional<std::size_t>{16});
    ASSERT_EQ(iv::event_count_for_sample_span(0.24, 64),
        std::optional<std::size_t>{16});
    ASSERT_EQ(iv::event_count_for_sample_span(0.5, 3),
        std::optional<std::size_t>{2});
    ASSERT_EQ(iv::event_count_for_sample_span(0.0, 4096),
        std::optional<std::size_t>{0});

    EXPECT_EQ(iv::event_sequence_capacity_for_sample_span(0.25, 64),
        std::optional<std::size_t>{16});
    EXPECT_EQ(iv::event_sequence_capacity_for_sample_span(0.24, 64),
        std::optional<std::size_t>{16});
    EXPECT_EQ(iv::event_sequence_capacity_for_sample_span(0.5, 3),
        std::optional<std::size_t>{2});
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

TEST(EventOutputPort, CountsProducerSequenceOverflowWithoutAllocating)
{
    std::array<iv::TimedEvent, 16> storage{};
    iv::EventSharedPortData shared(
        storage, 0, 0, iv::EventTypeId::trigger);
    std::uint64_t overflow_count = 0;
    iv::EventOutputPort output(
        shared,
        iv::EventTypeId::trigger,
        0,
        0,
        &overflow_count);

    output.begin_block(0, 64);
    for (std::size_t i = 0; i < 17; ++i) {
        output.push(iv::TimedEvent{
            .time = 0,
            .value = iv::TriggerEvent{},
        });
    }
    output.end_block();

    EXPECT_EQ(shared.write_index, 16u);
    EXPECT_EQ(overflow_count, 1u);
}

TEST(SampleConnectionStorageChooser, UsesBlockRelativeRetention)
{
    using Kind = iv::RealtimeBufferStorageKind;

    EXPECT_EQ(iv::choose_sample_connection_storage_plan({
        .current_block_frames = 256,
    }).kind, Kind::transient_stack);

    EXPECT_EQ(iv::choose_sample_connection_storage_plan({
        .current_block_frames = 256,
        .retained_frames = 32,
        .channel_count = 2,
    }).kind, Kind::stack_with_persistent_carry);

    EXPECT_EQ(iv::choose_sample_connection_storage_plan({
        .current_block_frames = 256,
        .retained_frames = 256,
        .channel_count = 2,
    }).kind, Kind::full_node_storage);
}

TEST(EventConnectionStorageChooser, UsesRateDerivedCapacities)
{
    using Kind = iv::RealtimeBufferStorageKind;

    EXPECT_EQ(iv::choose_event_connection_storage_plan({
        .current_event_capacity = 1024,
    }).kind, Kind::transient_stack);

    EXPECT_EQ(iv::choose_event_connection_storage_plan({
        .current_event_capacity = 1000,
        .retained_event_capacity = 999,
    }).kind, Kind::stack_with_persistent_carry);

    EXPECT_EQ(iv::choose_event_connection_storage_plan({
        .current_event_capacity = 1000,
        .retained_event_capacity = 1000,
    }).kind, Kind::full_node_storage);
}

} // namespace
