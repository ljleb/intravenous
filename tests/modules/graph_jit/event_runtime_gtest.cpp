#include "test_support.h"
#include <intravenous/graph/names.h>

namespace iv::test::graph_jit_tests {

TEST(GraphJitEventFeedbackRuntime, AppendsEachSourceSuffixExactlyOnce)
{
    std::array<iv::TimedEvent, 4> const source{
        iv::TimedEvent{.time = index(0), .value = iv::TriggerEvent{}},
        iv::TimedEvent{.time = index(1), .value = iv::TriggerEvent{}},
        iv::TimedEvent{.time = index(4), .value = iv::TriggerEvent{}},
        iv::TimedEvent{.time = index(5), .value = iv::TriggerEvent{}},
    };
    std::array<iv::TimedEvent, 8> ring{};
    std::size_t read_index = 0;
    std::size_t write_index = 0;

    iv::graph_jit::detail::iv_graph_jit_append_event_feedback(
        source.data(), nullptr, 0, 0, 0, 2, index(0), 5,
        ring.data(), ring.size(), &read_index, &write_index);
    iv::graph_jit::detail::iv_graph_jit_append_event_feedback(
        source.data(), nullptr, 0, 0, 2, source.size(), index(4), 5,
        ring.data(), ring.size(), &read_index, &write_index);

    EXPECT_EQ(read_index, 0u);
    ASSERT_EQ(write_index, source.size());
    EXPECT_EQ(ring[0].time, index(5));
    EXPECT_EQ(ring[1].time, index(6));
    EXPECT_EQ(ring[2].time, index(9));
    EXPECT_EQ(ring[3].time, index(10));
}

TEST(GraphJitEventFeedbackRuntime, AppendsWrappedPersistentSourceSuffixExactlyOnce)
{
    std::array<iv::TimedEvent, 4> source{};
    // Monotonic source indices [3, 7) wrap once through this four-index ring.
    source[3] = iv::TimedEvent{.time = index(3), .value = iv::TriggerEvent{}};
    source[0] = iv::TimedEvent{.time = index(4), .value = iv::TriggerEvent{}};
    source[1] = iv::TimedEvent{.time = index(5), .value = iv::TriggerEvent{}};
    source[2] = iv::TimedEvent{.time = index(6), .value = iv::TriggerEvent{}};
    std::array<iv::TimedEvent, 8> ring{};
    std::size_t read_index = 0;
    std::size_t write_index = 0;

    iv::graph_jit::detail::iv_graph_jit_append_event_feedback_ring_source(
        source.data(), nullptr, 0, 0, source.size(), 3, 5, index(3), 5,
        ring.data(), ring.size(), &read_index, &write_index);
    iv::graph_jit::detail::iv_graph_jit_append_event_feedback_ring_source(
        source.data(), nullptr, 0, 0, source.size(), 5, 7, index(5), 5,
        ring.data(), ring.size(), &read_index, &write_index);

    EXPECT_EQ(read_index, 0u);
    ASSERT_EQ(write_index, 4u);
    EXPECT_EQ(ring[0].time, index(8));
    EXPECT_EQ(ring[1].time, index(9));
    EXPECT_EQ(ring[2].time, index(10));
    EXPECT_EQ(ring[3].time, index(11));
}

TEST(GraphJitEventFeedbackRuntime, AppendsLinearFeedbackSuffixExactlyOnce)
{
    std::array<iv::TimedEvent, 4> const source{
        iv::TimedEvent{.time = index(0), .value = iv::TriggerEvent{}},
        iv::TimedEvent{.time = index(1), .value = iv::TriggerEvent{}},
        iv::TimedEvent{.time = index(4), .value = iv::TriggerEvent{}},
        iv::TimedEvent{.time = index(5), .value = iv::TriggerEvent{}},
    };
    std::array<iv::TimedEvent, 8> sequence{};
    std::size_t count = 0;

    iv::graph_jit::detail::iv_graph_jit_append_event_feedback_sequence(
        source.data(), nullptr, 0, 0, 0, 2, index(0), 5,
        sequence.data(), sequence.size(), &count);
    iv::graph_jit::detail::iv_graph_jit_append_event_feedback_sequence(
        source.data(), nullptr, 0, 0, 2, source.size(), index(4), 5,
        sequence.data(), sequence.size(), &count);

    ASSERT_EQ(count, source.size());
    EXPECT_EQ(sequence[0].time, index(5));
    EXPECT_EQ(sequence[1].time, index(6));
    EXPECT_EQ(sequence[2].time, index(9));
    EXPECT_EQ(sequence[3].time, index(10));
}

TEST(GraphJitEventFeedbackRuntime, AppendsWrappedRingSourceIntoLinearFeedback)
{
    std::array<iv::TimedEvent, 4> source{};
    source[3] = iv::TimedEvent{.time = index(3), .value = iv::TriggerEvent{}};
    source[0] = iv::TimedEvent{.time = index(4), .value = iv::TriggerEvent{}};
    source[1] = iv::TimedEvent{.time = index(5), .value = iv::TriggerEvent{}};
    source[2] = iv::TimedEvent{.time = index(6), .value = iv::TriggerEvent{}};
    std::array<iv::TimedEvent, 8> sequence{};
    std::size_t count = 0;

    iv::graph_jit::detail::iv_graph_jit_append_event_feedback_sequence_ring_source(
        source.data(), nullptr, 0, 0, source.size(), 3, 5, index(3), 5,
        sequence.data(), sequence.size(), &count);
    iv::graph_jit::detail::iv_graph_jit_append_event_feedback_sequence_ring_source(
        source.data(), nullptr, 0, 0, source.size(), 5, 7, index(5), 5,
        sequence.data(), sequence.size(), &count);

    ASSERT_EQ(count, 4u);
    EXPECT_EQ(sequence[0].time, index(8));
    EXPECT_EQ(sequence[1].time, index(9));
    EXPECT_EQ(sequence[2].time, index(10));
    EXPECT_EQ(sequence[3].time, index(11));
}

TEST(GraphJitEventFeedbackRuntime, FiltersLinearSourceIntoRingFeedback)
{
    std::array<iv::TimedEvent, 5> const source{
        iv::TimedEvent{.time = index(0), .value = iv::TriggerEvent{}},
        iv::TimedEvent{.time = index(1), .value = iv::TriggerEvent{}},
        iv::TimedEvent{.time = index(2), .value = iv::TriggerEvent{}},
        iv::TimedEvent{.time = index(3), .value = iv::TriggerEvent{}},
        iv::TimedEvent{.time = index(4), .value = iv::TriggerEvent{}},
    };
    std::array<std::size_t, 5> const source_indices{0, 1, 0, 1, 0};
    std::array<iv::TimedEvent, 8> ring{};
    std::size_t read_index = 0;
    std::size_t write_index = 0;

    iv::graph_jit::detail::iv_graph_jit_append_event_feedback(
        source.data(), source_indices.data(), 1, 1,
        0, source.size(), index(0), 5,
        ring.data(), ring.size(), &read_index, &write_index);

    EXPECT_EQ(read_index, 0u);
    ASSERT_EQ(write_index, 2u);
    EXPECT_EQ(ring[0].time, index(6));
    EXPECT_EQ(ring[1].time, index(8));
}

TEST(GraphJitEventFeedbackRuntime, FiltersLinearSourceIntoSequenceFeedback)
{
    std::array<iv::TimedEvent, 5> const source{
        iv::TimedEvent{.time = index(0), .value = iv::TriggerEvent{}},
        iv::TimedEvent{.time = index(1), .value = iv::TriggerEvent{}},
        iv::TimedEvent{.time = index(2), .value = iv::TriggerEvent{}},
        iv::TimedEvent{.time = index(3), .value = iv::TriggerEvent{}},
        iv::TimedEvent{.time = index(4), .value = iv::TriggerEvent{}},
    };
    std::array<std::size_t, 5> const source_indices{0, 1, 0, 1, 0};
    std::array<iv::TimedEvent, 8> sequence{};
    std::size_t count = 0;

    iv::graph_jit::detail::iv_graph_jit_append_event_feedback_sequence(
        source.data(), source_indices.data(), 1, 1,
        0, source.size(), index(0), 5,
        sequence.data(), sequence.size(), &count);

    ASSERT_EQ(count, 2u);
    EXPECT_EQ(sequence[0].time, index(6));
    EXPECT_EQ(sequence[1].time, index(8));
}

TEST(GraphJitEventFeedbackRuntime, FiltersRingSourceIntoRingFeedback)
{
    std::array<iv::TimedEvent, 4> source{};
    std::array<std::size_t, 4> source_indices{};
    source[3] = iv::TimedEvent{.time = index(3), .value = iv::TriggerEvent{}};
    source_indices[3] = 0;
    source[0] = iv::TimedEvent{.time = index(4), .value = iv::TriggerEvent{}};
    source_indices[0] = 1;
    source[1] = iv::TimedEvent{.time = index(5), .value = iv::TriggerEvent{}};
    source_indices[1] = 0;
    source[2] = iv::TimedEvent{.time = index(6), .value = iv::TriggerEvent{}};
    source_indices[2] = 1;
    std::array<iv::TimedEvent, 8> ring{};
    std::size_t read_index = 0;
    std::size_t write_index = 0;

    iv::graph_jit::detail::iv_graph_jit_append_event_feedback_ring_source(
        source.data(), source_indices.data(), 1, 1,
        source.size(), 3, 7, index(3), 5,
        ring.data(), ring.size(), &read_index, &write_index);

    EXPECT_EQ(read_index, 0u);
    ASSERT_EQ(write_index, 2u);
    EXPECT_EQ(ring[0].time, index(9));
    EXPECT_EQ(ring[1].time, index(11));
}

TEST(GraphJitEventFeedbackRuntime, FiltersRingSourceIntoSequenceFeedback)
{
    std::array<iv::TimedEvent, 4> source{};
    std::array<std::size_t, 4> source_indices{};
    source[3] = iv::TimedEvent{.time = index(3), .value = iv::TriggerEvent{}};
    source_indices[3] = 0;
    source[0] = iv::TimedEvent{.time = index(4), .value = iv::TriggerEvent{}};
    source_indices[0] = 1;
    source[1] = iv::TimedEvent{.time = index(5), .value = iv::TriggerEvent{}};
    source_indices[1] = 0;
    source[2] = iv::TimedEvent{.time = index(6), .value = iv::TriggerEvent{}};
    source_indices[2] = 1;
    std::array<iv::TimedEvent, 8> sequence{};
    std::size_t count = 0;

    iv::graph_jit::detail::iv_graph_jit_append_event_feedback_sequence_ring_source(
        source.data(), source_indices.data(), 1, 1,
        source.size(), 3, 7, index(3), 5,
        sequence.data(), sequence.size(), &count);

    ASSERT_EQ(count, 2u);
    EXPECT_EQ(sequence[0].time, index(9));
    EXPECT_EQ(sequence[1].time, index(11));
}

TEST(GraphJitEventFeedbackRuntime, MaterializesOneSelectedSource)
{
    std::array<iv::TimedEvent, 4> const source{
        iv::TimedEvent{.time = index(0), .value = iv::TriggerEvent{}},
        iv::TimedEvent{.time = index(1), .value = iv::TriggerEvent{}},
        iv::TimedEvent{.time = index(2), .value = iv::TriggerEvent{}},
        iv::TimedEvent{.time = index(3), .value = iv::TriggerEvent{}},
    };
    std::array<std::size_t, 4> const source_indices{0, 1, 0, 1};
    std::array<iv::TimedEvent, 4> target{};

    auto const count =
        iv::graph_jit::detail::iv_graph_jit_materialize_event_sequence(
            static_cast<std::uint32_t>(iv::EventTypeId::trigger),
            static_cast<std::uint32_t>(iv::EventTypeId::trigger),
            0, 0, 0, 0,
            source.data(), source_indices.data(), 1, 1,
            source.size(), 0, source.size(), index(0), 4, 0,
            target.data(), target.size());

    ASSERT_EQ(count, 2u);
    EXPECT_EQ(target[0].time, index(1));
    EXPECT_EQ(target[1].time, index(3));
}

TEST(GraphJitEventMergeRuntime, KWayMergePreservesSemanticSourceOrder)
{
    auto midi = [](std::uint8_t note) {
        iv::MidiEvent event{};
        event.bytes = {0x90, note, 100};
        event.size = 3;
        return event;
    };
    auto timed = [&](std::uint64_t time, std::uint8_t note) {
        return iv::TimedEvent{
            .time = iv::GlobalIndex::from_bits(time), .value = midi(note)};
    };

    std::array<iv::TimedEvent, 16> target{};
    target[0] = timed(2, 10);
    target[1] = timed(5, 11);
    target[2] = timed(9, 12);
    std::array source_1{
        timed(1, 20),
        timed(5, 21),
        timed(9, 22),
    };
    std::array source_2{
        timed(5, 31),
        timed(7, 32),
        timed(9, 33),
    };
    std::array<void const*, 2> source_events{
        source_1.data(), source_2.data()};
    std::array<std::size_t, 2> remaining{
        source_1.size(), source_2.size()};

    auto const count =
        iv::graph_jit::detail::iv_graph_jit_merge_event_sequences_into_home(
            target.data(),
            target.size(),
            3,
            source_events.data(),
            remaining.data(),
            source_events.size(),
            index(0));

    ASSERT_EQ(count, 9u);
    EXPECT_EQ(remaining[0], 0u);
    EXPECT_EQ(remaining[1], 0u);
    std::array<iv::GlobalIndex, 9> const expected_times{
        index(1), index(2), index(5), index(5), index(5),
        index(7), index(9), index(9), index(9)};
    std::array<std::uint8_t, 9> const expected_notes{
        20, 10, 11, 21, 31, 32, 12, 22, 33};
    for (std::size_t i = 0; i < count; ++i) {
        EXPECT_EQ(target[i].time, expected_times[i]);
        auto const* event = std::get_if<iv::MidiEvent>(&target[i].value);
        ASSERT_NE(event, nullptr);
        ASSERT_GE(event->size, 2u);
        EXPECT_EQ(event->bytes[1], expected_notes[i]);
    }
}

TEST(GraphJitEventMergeRuntime, KWayMergeUsesRangeRelativeGlobalOrder)
{
    auto midi = [](std::uint8_t note) {
        iv::MidiEvent event{};
        event.bytes = {0x90, note, 100};
        event.size = 3;
        return event;
    };
    auto timed = [&](std::int64_t time, std::uint8_t note) {
        return iv::TimedEvent{.time = index(time), .value = midi(note)};
    };

    std::array<iv::TimedEvent, 8> target{};
    target[0] = timed(-2, 10);
    target[1] = timed(1, 11);
    std::array source{timed(-1, 20), timed(0, 21)};
    std::array<void const*, 1> source_events{source.data()};
    std::array<std::size_t, 1> remaining{source.size()};

    auto const count =
        iv::graph_jit::detail::iv_graph_jit_merge_event_sequences_into_home(
            target.data(),
            target.size(),
            2,
            source_events.data(),
            remaining.data(),
            source_events.size(),
            index(-2));

    ASSERT_EQ(count, 4u);
    EXPECT_EQ(remaining[0], 0u);
    std::array<iv::GlobalIndex, 4> const expected_times{
        index(-2), index(-1), index(0), index(1)};
    std::array<std::uint8_t, 4> const expected_notes{10, 20, 21, 11};
    for (std::size_t i = 0; i < count; ++i) {
        EXPECT_EQ(target[i].time, expected_times[i]);
        auto const* event = std::get_if<iv::MidiEvent>(&target[i].value);
        ASSERT_NE(event, nullptr);
        ASSERT_GE(event->size, 2u);
        EXPECT_EQ(event->bytes[1], expected_notes[i]);
    }
}

TEST(GraphJitEventMergeRuntime, StagedMergePreservesSemanticSourceOrder)
{
    auto midi = [](std::uint8_t note) {
        iv::MidiEvent event{};
        event.bytes = {0x90, note, 100};
        event.size = 3;
        return event;
    };
    auto timed = [&](std::uint64_t time, std::uint8_t note) {
        return iv::TimedEvent{
            .time = iv::GlobalIndex::from_bits(time), .value = midi(note)};
    };

    std::array<iv::TimedEvent, 8> target{};
    std::array<std::size_t, 8> source_indices{};
    target[0] = timed(5, 21);
    source_indices[0] = 1;
    std::array semantic_source_0{timed(5, 10)};
    std::array semantic_source_2{timed(5, 32)};

    auto count =
        iv::graph_jit::detail::iv_graph_jit_merge_ordered_event_sequence(
            target.data(),
            source_indices.data(),
            target.size(),
            0,
            1,
            semantic_source_0.data(),
            semantic_source_0.size(),
            semantic_source_0.size(),
            0,
            index(0));
    count = iv::graph_jit::detail::iv_graph_jit_merge_ordered_event_sequence(
        target.data(),
        source_indices.data(),
        target.size(),
        0,
        count,
        semantic_source_2.data(),
        semantic_source_2.size(),
        semantic_source_2.size(),
        2,
        index(0));

    ASSERT_EQ(count, 3u);
    std::array<std::uint8_t, 3> const expected_notes{10, 21, 32};
    std::array<std::size_t, 3> const expected_indices{0, 1, 2};
    for (std::size_t i = 0; i < count; ++i) {
        auto const* event = std::get_if<iv::MidiEvent>(&target[i].value);
        ASSERT_NE(event, nullptr);
        ASSERT_GE(event->size, 2u);
        EXPECT_EQ(event->bytes[1], expected_notes[i]);
        EXPECT_EQ(source_indices[i], expected_indices[i]);
    }
}

TEST(GraphJitEventMergeRuntime, StagedMergePreservesWrappedRingOrder)
{
    auto midi = [](std::uint8_t note) {
        iv::MidiEvent event{};
        event.bytes = {0x90, note, 100};
        event.size = 3;
        return event;
    };
    auto timed = [&](std::uint64_t time, std::uint8_t note) {
        return iv::TimedEvent{
            .time = iv::GlobalIndex::from_bits(time), .value = midi(note)};
    };

    std::array<iv::TimedEvent, 8> ring{};
    std::array<std::size_t, 8> source_indices{};
    constexpr std::size_t read_index = 6;
    ring[6] = timed(3, 30);
    ring[7] = timed(5, 51);
    ring[0] = timed(7, 70);
    source_indices[6] = 2;
    source_indices[7] = 1;
    source_indices[0] = 0;
    std::array source{
        timed(2, 20),
        timed(5, 50),
        timed(8, 80),
    };

    auto const write_index =
        iv::graph_jit::detail::iv_graph_jit_merge_ordered_event_sequence(
            ring.data(),
            source_indices.data(),
            ring.size(),
            read_index,
            read_index + 3,
            source.data(),
            source.size(),
            source.size(),
            0,
            index(0));

    ASSERT_EQ(write_index, read_index + 6);
    std::array<std::uint8_t, 6> const expected_notes{20, 30, 50, 51, 70, 80};
    std::array<std::size_t, 6> const expected_indices{0, 2, 0, 1, 0, 0};
    for (std::size_t i = 0; i < expected_notes.size(); ++i) {
        auto const index = (read_index + i) & (ring.size() - 1);
        auto const* event = std::get_if<iv::MidiEvent>(&ring[index].value);
        ASSERT_NE(event, nullptr);
        ASSERT_GE(event->size, 2u);
        EXPECT_EQ(event->bytes[1], expected_notes[i]);
        EXPECT_EQ(source_indices[index], expected_indices[i]);
    }
}

} // namespace iv::test::graph_jit_tests

namespace iv::test::graph_jit_tests {
TEST(GraphJitEventSearch, FindsSurroundingPointsWhenRequestedRangeContainsNoEvents)
{
    std::array events{
        iv::TimedEvent{.time = index(0), .value = iv::SampleEvent{iv::Sample{0}}},
        iv::TimedEvent{.time = index(100), .value = iv::SampleEvent{iv::Sample{1}}},
    };
    EXPECT_TRUE(iv::sorted_event_range(events, range(0, 101), range(40, 50)).empty());
    auto neighbors = iv::sorted_event_neighbors(events, index(0), index(40));
    ASSERT_TRUE(neighbors.at_or_before);
    ASSERT_TRUE(neighbors.after);
    EXPECT_EQ(neighbors.at_or_before->time, index(0));
    EXPECT_EQ(neighbors.after->time, index(100));
    EXPECT_EQ(std::get<iv::SampleEvent>(neighbors.after->value).value, iv::Sample{1});
}

TEST(GraphJitEventSearch, UsesLastEqualPositionAndKeepsCircularRegionOrder)
{
    auto const begin = iv::GlobalIndex::from_bits(std::uint64_t{1} << 63).retreated_by(2);
    std::array events{
        iv::TimedEvent{.time = begin, .value = iv::SampleEvent{iv::Sample{1}}},
        iv::TimedEvent{.time = begin.advanced_by(2), .value = iv::SampleEvent{iv::Sample{2}}},
        iv::TimedEvent{.time = begin.advanced_by(2), .value = iv::SampleEvent{iv::Sample{3}}},
        iv::TimedEvent{.time = begin.advanced_by(5), .value = iv::SampleEvent{iv::Sample{4}}},
    };
    auto neighbors = iv::sorted_event_neighbors(events, begin, begin.advanced_by(2));
    ASSERT_TRUE(neighbors.at_or_before);
    ASSERT_TRUE(neighbors.after);
    EXPECT_EQ(std::get<iv::SampleEvent>(neighbors.at_or_before->value).value, iv::Sample{3});
    EXPECT_EQ(neighbors.after->time, begin.advanced_by(5));
    auto selected = iv::sorted_event_range(events, {begin, 6}, {begin.advanced_by(2), 3});
    ASSERT_EQ(selected.size(), 2u);
    EXPECT_EQ(std::get<iv::SampleEvent>(selected[0].value).value, iv::Sample{2});
    EXPECT_EQ(std::get<iv::SampleEvent>(selected[1].value).value, iv::Sample{3});
}

TEST(GraphJitEventSearch, EmptySequenceAndEdgesHaveNoInventedNeighbor)
{
    EXPECT_FALSE(iv::sorted_event_neighbors({}, index(0), index(1)).at_or_before);
    std::array events{iv::TimedEvent{.time = index(10), .value = iv::SampleEvent{iv::Sample{1}}}};
    EXPECT_FALSE(iv::sorted_event_neighbors(events, index(0), index(5)).at_or_before);
    EXPECT_FALSE(iv::sorted_event_neighbors(events, index(0), index(15)).after);
    EXPECT_EQ(iv::details::event_type_name(iv::EventTypeId::sample), "sample");
    EXPECT_THROW(iv::EventConversionRegistry::plan(iv::EventTypeId::sample, iv::EventTypeId::midi), std::logic_error);
}
TEST(GraphJitEventSearch, MaterializedNeighborsPreserveOverlappingRegionMembershipAcrossWrap)
{
    auto const begin = iv::GlobalIndex::from_bits(~std::uint64_t{0} - 2);
    auto event = [&](std::size_t offset, float value) {
        return iv::TimedEvent{.time = begin.advanced_by(offset), .value = iv::SampleEvent{iv::Sample{value}}};
    };
    auto input = iv::make_tick_materialized_event_input(0, iv::EventTypeId::sample,
        std::vector<iv::GlobalIndexRange>{{begin, 7}, {begin, 7}},
        std::vector<std::vector<iv::TimedEvent>>{{event(0, 1), event(6, 2)}, {event(1, 10), event(5, 20)}},
        std::vector<std::vector<std::size_t>>{{0, 0}, {1, 1}});
    auto const first = input.neighbors(0, begin.advanced_by(3));
    auto const second = input.neighbors(1, begin.advanced_by(3));
    ASSERT_TRUE(first.at_or_before);
    ASSERT_TRUE(first.after);
    ASSERT_TRUE(second.at_or_before);
    ASSERT_TRUE(second.after);
    EXPECT_FLOAT_EQ(static_cast<float>(std::get<iv::SampleEvent>(first.at_or_before->value).value), 1);
    EXPECT_FLOAT_EQ(static_cast<float>(std::get<iv::SampleEvent>(first.after->value).value), 2);
    EXPECT_FLOAT_EQ(static_cast<float>(std::get<iv::SampleEvent>(second.at_or_before->value).value), 10);
    EXPECT_FLOAT_EQ(static_cast<float>(std::get<iv::SampleEvent>(second.after->value).value), 20);
}

} // namespace iv::test::graph_jit_tests
