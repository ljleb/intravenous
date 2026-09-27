#include <intravenous/runtime/persisted_page_store.h>
#include <intravenous/runtime/tick_invocation_frame.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace {

iv::PersistedOutputId stable_output(
    iv::PortKind kind,
    char const* name)
{
    return iv::graph_jit::StableOutputPortId{
        .node = {
            .graph = "project",
            .virtual_node = "source",
            .direct_member = 0,
        },
        .kind = kind,
        .port_name = name,
        .port_index = 0,
    };
}

iv::PersistedOutputId local_output(
    iv::PortKind kind,
    iv::graph_jit::BackgroundPortIndex port)
{
    return iv::GenerationLocalPersistedOutputId{
        .generation = 3,
        .port = port,
        .kind = kind,
    };
}

iv::PersistedSamplePage sample_page(
    iv::PersistedOutputId output,
    float first)
{
    return {
        .output = std::move(output),
        .page_index = 0,
        .domain = iv::Coverage{{{0, 4}}},
        .layout = {
            .channel_type = iv::ChannelTypeId::mono,
            .sample_layout = iv::SampleStreamLayout::planar,
        },
        .packing = iv::PersistedSamplePacking::dense,
        .values = {first, first + 1, first + 2, first + 3},
    };
}

iv::PersistedEventPage empty_event_page(iv::PersistedOutputId output)
{
    return {
        .output = std::move(output),
        .page_index = 0,
        .domain = iv::Coverage{{{0, 4}}},
        .type = iv::EventTypeId::trigger,
        .events = {},
    };
}

static_assert(noexcept(
    std::declval<iv::PersistedPageStore::ReaderSlot&>().pin()));
static_assert(std::is_nothrow_destructible_v<
    iv::PersistedPageStore::ReaderPin>);
static_assert(std::is_nothrow_constructible_v<
    iv::TickInvocationFrame,
    iv::PersistedPageStore::ReaderSlot&>);

TEST(PersistedPageStore, TickInvocationFramePinsOnePublishedRootForItsLifetime)
{
    iv::PersistedPageStore store;
    auto slot = store.register_reader();

    {
        iv::TickInvocationFrame frame{slot};
        EXPECT_EQ(frame.published_pages().version(),
            (iv::PersistedPageSnapshotVersion{}));
        EXPECT_TRUE(frame.call().sequential_sample_inputs.empty());
        EXPECT_TRUE(frame.call().sequential_event_inputs.empty());
        EXPECT_TRUE(frame.call().random_access_sample_inputs.empty());
        EXPECT_TRUE(frame.call().random_access_event_inputs.empty());

        auto candidate = store.begin_candidate(1, 4);
        ASSERT_EQ(
            store.publish(std::move(candidate)),
            iv::PersistedPagePublishResult::published);
        EXPECT_EQ(store.retired_snapshot_count(), 1u);
        EXPECT_EQ(store.reclaim_retired(), 0u);
        EXPECT_EQ(frame.published_pages().version(),
            (iv::PersistedPageSnapshotVersion{}));
    }

    EXPECT_EQ(store.reclaim_retired(), 1u);
}

TEST(PersistedPageStore, PublishesSampleAndEventPagesAsOneImmutableRoot)
{
    iv::PersistedPageStore store;
    auto old_slot = store.register_reader();
    auto current_slot = store.register_reader();
    auto old = old_slot.pin();
    EXPECT_TRUE(store.is_current(old.snapshot()));

    auto const sample = stable_output(iv::PortKind::sample, "samples");
    auto const events = local_output(iv::PortKind::event, 8);
    auto candidate = store.begin_candidate(12, 4);
    candidate.put(sample_page(sample, 1.0f));
    candidate.put(empty_event_page(events));

    EXPECT_EQ(
        store.publish(std::move(candidate)),
        iv::PersistedPagePublishResult::published);

    auto current = current_slot.pin();
    EXPECT_FALSE(store.is_current(old.snapshot()));
    EXPECT_TRUE(store.is_current(current.snapshot()));
    EXPECT_EQ(old->version(), (iv::PersistedPageSnapshotVersion{}));
    EXPECT_EQ(old->sample_page_count(), 0);
    EXPECT_EQ(old->event_page_count(), 0);
    EXPECT_EQ(
        current->version(),
        (iv::PersistedPageSnapshotVersion{.semantic = 12, .page = 1}));
    ASSERT_NE(current->find_sample_page(sample, 0), nullptr);
    EXPECT_FLOAT_EQ(
        current->find_sample_page(sample, 0)->values.front().value,
        1.0f);
    ASSERT_NE(current->find_event_page(events, 0), nullptr);
    EXPECT_TRUE(current->find_event_page(events, 0)->events.empty());

    EXPECT_EQ(store.retired_snapshot_count(), 1);
    EXPECT_EQ(store.reclaim_retired(), 0);

    old = {};
    EXPECT_EQ(store.reclaim_retired(), 1);
    EXPECT_EQ(store.retired_snapshot_count(), 0);
}

TEST(PersistedPageStore, RejectsASecondCandidateFromTheSameBaseAsStale)
{
    iv::PersistedPageStore store;
    auto slot = store.register_reader();
    auto const output = stable_output(iv::PortKind::sample, "samples");
    auto first = store.begin_candidate(4, 4);
    auto stale = store.begin_candidate(4, 4);
    first.put(sample_page(output, 10.0f));
    stale.put(sample_page(output, 20.0f));

    EXPECT_EQ(
        store.publish(std::move(first)),
        iv::PersistedPagePublishResult::published);
    EXPECT_EQ(
        store.publish(std::move(stale)),
        iv::PersistedPagePublishResult::stale_base);

    auto published = slot.pin();
    EXPECT_EQ(
        published->version(),
        (iv::PersistedPageSnapshotVersion{.semantic = 4, .page = 1}));
    ASSERT_NE(published->find_sample_page(output, 0), nullptr);
    EXPECT_FLOAT_EQ(
        published->find_sample_page(output, 0)->values.front().value,
        10.0f);
}

TEST(PersistedPageStore, StructurallySharesUnchangedPagesAcrossRoots)
{
    iv::PersistedPageStore store;
    auto old_slot = store.register_reader();
    auto new_slot = store.register_reader();
    auto const samples = stable_output(iv::PortKind::sample, "samples");
    auto const events = stable_output(iv::PortKind::event, "events");

    auto first = store.begin_candidate(7, 4);
    first.put(sample_page(samples, 2.0f));
    ASSERT_EQ(
        store.publish(std::move(first)),
        iv::PersistedPagePublishResult::published);
    auto old = old_slot.pin();
    auto const* shared_page = old->find_sample_page(samples, 0);

    auto second = store.begin_candidate(7, 4);
    second.put(empty_event_page(events));
    ASSERT_EQ(
        store.publish(std::move(second)),
        iv::PersistedPagePublishResult::published);
    auto current = new_slot.pin();

    EXPECT_EQ(current->version().page, 2);
    EXPECT_EQ(current->find_sample_page(samples, 0), shared_page);
    EXPECT_NE(current->find_event_page(events, 0), nullptr);
    EXPECT_EQ(old->find_event_page(events, 0), nullptr);
}

TEST(PersistedPageStore, ValidatesTypedPayloadsAgainstTheCanonicalPageDomain)
{
    iv::PersistedPageStore store;
    auto candidate = store.begin_candidate(1, 4);

    auto packed = sample_page(
        stable_output(iv::PortKind::sample, "samples"), 1.0f);
    packed.domain = iv::Coverage{{{1, 3}}};
    packed.packing = iv::PersistedSamplePacking::coverage_packed;
    packed.values = {1.0f, 2.0f};
    EXPECT_NO_THROW(candidate.put(std::move(packed)));

    auto wrong_kind = empty_event_page(
        stable_output(iv::PortKind::sample, "not-events"));
    EXPECT_THROW(candidate.put(std::move(wrong_kind)), std::invalid_argument);

    auto unordered = empty_event_page(
        stable_output(iv::PortKind::event, "events"));
    unordered.events = {
        {.time = 2, .value = iv::TriggerEvent{}},
        {.time = 1, .value = iv::TriggerEvent{}},
    };
    EXPECT_THROW(candidate.put(std::move(unordered)), std::invalid_argument);
}

} // namespace
