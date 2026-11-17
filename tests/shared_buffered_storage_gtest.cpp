#include <intravenous/runtime/persisted_data_store.h>
#include <intravenous/runtime/tick_materialization_snapshot.h>
#include <intravenous/runtime/graph_jit.h>

#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <vector>
#if defined(__linux__)
#include <sys/resource.h>
#endif

namespace {
using namespace iv;
GlobalIndex at(std::int64_t value) { return GlobalIndex::from_signed(value); }
GlobalIndexRange span(std::int64_t begin, std::uint64_t length) { return {at(begin), length}; }
PersistedOutputHandle output(PersistedDataStore& store, std::size_t port = 0, PortKind kind = PortKind::sample)
{
    return store.resolve_output(GenerationLocalPersistedOutputId{.generation = 1, .port = port, .kind = kind});
}
PersistedSampleRegion samples(PersistedOutputHandle id, GlobalIndexRange range, float first = 0,
    ChannelLayout layout = mono_interleaved_channel_layout)
{
    PersistedSampleRegion result{.output = id, .range = range, .layout = layout};
    result.values.resize(static_cast<std::size_t>(range.extent) * channel_count(layout));
    for (std::size_t index = 0; index < result.values.size(); ++index) result.values[index] = first + static_cast<float>(index);
    return result;
}
void replace(PersistedDataStore::Candidate& candidate, PersistedSampleRegion region)
{
    auto const range = region.range;
    candidate.replace_sample_regions(region.output, region.layout, region.geometry,
        IndexCoverage{range}, std::array{range}, {std::move(region)});
}

TEST(SharedBufferedStorage, RecordingAndTockReuseOneBoundedBackingAfterSnapshotsRetire)
{
    PersistedDataStore store;
    auto const recording = output(store, 0), tock = output(store, 1);
    auto const page = store.buffered_storage_usage().page_size;
    auto const frames = page / sizeof(Sample);
    for (std::uint64_t revision = 1; revision <= 50; ++revision) {
        auto candidate = store.begin_candidate(revision);
        candidate.overwrite(samples(recording, span(0, frames), static_cast<float>(revision)));
        replace(candidate, samples(tock, span(0, frames), static_cast<float>(revision), mono_planar_channel_layout));
        ASSERT_EQ(store.publish(std::move(candidate)), PersistedDataPublishResult::published);
        (void)store.reclaim_retired();
        auto const usage = store.buffered_storage_usage();
        EXPECT_EQ(usage.live_page_bytes, 2 * page);
        EXPECT_LE(usage.backing_bytes, 4 * page);
    }
    EXPECT_GT(store.buffered_storage_usage().reused_pages, 0u);
    auto reader = store.register_reader();
    auto pinned = reader.pin();
    EXPECT_FLOAT_EQ(pinned->find_sample_region(recording, at(0))->at(at(0), 0).value, 50);
    EXPECT_FLOAT_EQ(pinned->find_sample_region(tock, at(0))->at(at(0), 0).value, 50);
}

TEST(SharedBufferedStorage, CopiedMappingPinsItsPagesAfterItsSnapshotAndOutputRetire)
{
    PersistedDataStore store;
    auto const id = output(store);
    auto const page = store.buffered_storage_usage().page_size;
    auto seed = store.begin_candidate(1);
    seed.overwrite(samples(id, span(0, page / sizeof(Sample)), 42));
    ASSERT_EQ(store.publish(std::move(seed)), PersistedDataPublishResult::published);
    auto reader = store.register_reader();
    auto pin = reader.pin();
    auto held = pin->find_sample_region(id, at(0))->continuous_values;
    pin = {};
    for (std::uint64_t revision = 2; revision <= 30; ++revision) {
        auto candidate = store.begin_candidate(revision);
        candidate.overwrite(samples(id, span(0, page / sizeof(Sample)), static_cast<float>(revision)));
        ASSERT_EQ(store.publish(std::move(candidate)), PersistedDataPublishResult::published);
        (void)store.reclaim_retired();
        EXPECT_FLOAT_EQ(reinterpret_cast<Sample const*>(held.data())[0].value, 42);
    }
    EXPECT_EQ(store.buffered_storage_usage().live_page_bytes, 2 * page);
    held = {};
    EXPECT_EQ(store.buffered_storage_usage().live_page_bytes, page);
}

TEST(SharedBufferedStorage, TockReplacementSharesUnchangedPagesAndErasureDropsOnlyUnneededPages)
{
    PersistedDataStore store;
    auto const id = output(store);
    auto const page = store.buffered_storage_usage().page_size, frames = page / sizeof(Sample);
    auto const logical = span(0, 2 * frames);
    auto seed = store.begin_candidate(1);
    replace(seed, samples(id, logical, 1, mono_planar_channel_layout));
    ASSERT_EQ(store.publish(std::move(seed)), PersistedDataPublishResult::published);
    auto old_reader = store.register_reader();
    auto old = old_reader.pin();
    auto next = samples(id, logical, 1, mono_planar_channel_layout);
    next.values[frames] = 9999;
    auto update = store.begin_candidate(2);
    replace(update, std::move(next));
    ASSERT_EQ(store.publish(std::move(update)), PersistedDataPublishResult::published);
    EXPECT_EQ(store.buffered_storage_usage().live_page_bytes, 3 * page);
    EXPECT_FLOAT_EQ(old->find_sample_region(id, at(frames))->at(at(frames), 0).value, 1 + frames);
    old = {};
    (void)store.reclaim_retired();
    EXPECT_EQ(store.buffered_storage_usage().live_page_bytes, 2 * page);
    auto erase = store.begin_candidate(3);
    erase.erase_sample(id, span(frames, frames), mono_planar_channel_layout);
    ASSERT_EQ(store.publish(std::move(erase)), PersistedDataPublishResult::published);
    (void)store.reclaim_retired();
    EXPECT_EQ(store.buffered_storage_usage().live_page_bytes, page);
    auto reader = store.register_reader();
    auto current = reader.pin();
    EXPECT_EQ(*current->find_sample_logical_coverage(id), IndexCoverage{logical});
    EXPECT_EQ(*current->find_sample_coverage(id, mono_planar_channel_layout), IndexCoverage{span(0, frames)});
    EXPECT_EQ(current->find_sample_region(id, at(frames)), nullptr);
}

TEST(SharedBufferedStorage, RecordingVoidPrunesPagesWithoutErasingValidNeighboursInTheSamePage)
{
    PersistedDataStore store;
    auto const id = output(store);
    auto const page = store.buffered_storage_usage().page_size, frames = page / sizeof(Sample);
    auto seed = store.begin_candidate(1);
    seed.overwrite(samples(id, span(0, 3 * frames), 1));
    ASSERT_EQ(store.publish(std::move(seed)), PersistedDataPublishResult::published);
    auto erase = store.begin_candidate(2);
    erase.erase_sample(id, span(frames / 2, 2 * frames), mono_interleaved_channel_layout);
    ASSERT_EQ(store.publish(std::move(erase)), PersistedDataPublishResult::published);
    (void)store.reclaim_retired();
    EXPECT_EQ(store.buffered_storage_usage().live_page_bytes, 2 * page);
    auto reader = store.register_reader();
    auto pin = reader.pin();
    EXPECT_EQ(pin->find_sample_region(id, at(frames)), nullptr);
    EXPECT_FLOAT_EQ(pin->find_sample_region(id, at(0))->at(at(0), 0).value, 1);
    EXPECT_FLOAT_EQ(pin->find_sample_region(id, at(3 * frames - 1))->at(at(3 * frames - 1), 0).value, 3 * frames);
}

TEST(SharedBufferedStorage, BoundedMappedWindowsPreserveOneLogicalRegionAndRejectOversizedCompleteViews)
{
    PersistedDataStore probe;
    auto const page = probe.buffered_storage_usage().page_size;
    BufferedStorageLimits limits;
    limits.max_contiguous_mapping_bytes = 2 * page;
    PersistedDataStore store{limits};
    auto const id = output(store);
    auto const frames = page / sizeof(Sample);
    auto seed = store.begin_candidate(1);
    seed.overwrite(samples(id, span(0, 5 * frames), 10));
    ASSERT_EQ(store.publish(std::move(seed)), PersistedDataPublishResult::published);
    auto reader = store.register_reader();
    auto pin = reader.pin();
    auto regions = pin->sample_regions(id, mono_interleaved_channel_layout);
    ASSERT_EQ(regions.size(), 1u);
    ASSERT_NE(regions[0].windows, nullptr);
    EXPECT_EQ(regions[0].windows->size(), 3u);
    EXPECT_THROW((void)regions[0].contiguous_view(), std::length_error);
    auto selected = regions[0].selected_view(span(3 * frames, frames));
    EXPECT_FLOAT_EQ(selected.at(at(3 * frames), 0).value, 10 + 3 * frames);
    pin = {};
    auto erase = store.begin_candidate(2);
    erase.erase_output(id);
    ASSERT_EQ(store.publish(std::move(erase)), PersistedDataPublishResult::published);
    (void)store.reclaim_retired();
    EXPECT_EQ(store.buffered_storage_usage().live_page_bytes, page);
    selected = {};
    EXPECT_EQ(store.buffered_storage_usage().live_page_bytes, 0u);
    EXPECT_EQ(store.buffered_storage_usage().reserved_mapping_bytes, 0u);
}

TEST(SharedBufferedStorage, PhysicalAndVirtualLimitsCountPinnedOldVersionsAndRollbackFailedViews)
{
    PersistedDataStore probe;
    auto const page = probe.buffered_storage_usage().page_size;
    for (bool physical : {false, true}) {
        BufferedStorageLimits limits;
        if (physical) limits.max_backing_bytes = 2 * page;
        else limits.max_total_mapping_bytes = 2 * page;
        PersistedDataStore store{limits};
        auto const id = output(store);
        auto seed = store.begin_candidate(1);
        seed.overwrite(samples(id, span(0, 1), 1));
        ASSERT_EQ(store.publish(std::move(seed)), PersistedDataPublishResult::published);
        auto reader = store.register_reader();
        auto old = reader.pin();
        auto next = store.begin_candidate(2);
        next.overwrite(samples(id, span(0, 1), 2));
        ASSERT_EQ(store.publish(std::move(next)), PersistedDataPublishResult::published);
        {
            auto rejected = store.begin_candidate(3);
            EXPECT_THROW(rejected.overwrite(samples(id, span(0, 1), 3)), std::length_error);
        }
        EXPECT_FLOAT_EQ(old->find_sample_region(id, at(0))->at(at(0), 0).value, 1);
        EXPECT_EQ(store.buffered_storage_usage().reserved_mapping_bytes, 2 * page);
        old = {};
        (void)store.reclaim_retired();
        auto retry = store.begin_candidate(3);
        EXPECT_NO_THROW(retry.overwrite(samples(id, span(0, 1), 3)));
    }
}

TEST(SharedBufferedStorage, MappingRunBudgetCountsRunsRatherThanPagesAndChecksProcessHeadroom)
{
    BufferedStorageLimits limits;
    limits.max_mapping_runs = 1;
    PersistedDataStore store{limits};
    auto const page = store.buffered_storage_usage().page_size, frames = page / sizeof(Sample);
    auto const id = output(store);
    auto seed = store.begin_candidate(1);
    EXPECT_NO_THROW(seed.overwrite(samples(id, span(0, 8 * frames), 1)));
    ASSERT_EQ(store.publish(std::move(seed)), PersistedDataPublishResult::published);
    EXPECT_EQ(store.buffered_storage_usage().mapped_runs, 1u);
    auto fragmented = store.begin_candidate(2);
    EXPECT_THROW(fragmented.overwrite(samples(id, span(frames, 1), 9000)), std::length_error);
    limits.process_vma_headroom = std::numeric_limits<std::size_t>::max();
    PersistedDataStore no_headroom{limits};
    auto blocked = no_headroom.begin_candidate(1);
    EXPECT_THROW(blocked.overwrite(samples(output(no_headroom), span(0, 1), 1)), std::length_error);
    EXPECT_EQ(no_headroom.buffered_storage_usage().reserved_mapping_bytes, 0u);
}

TEST(SharedBufferedStorage, EventChunksShareTheBackingBudgetAndRemainAuthoritative)
{
    BufferedStorageLimits limits;
    limits.max_backing_bytes = 32 * sizeof(TimedEvent);
    PersistedDataStore store{limits};
    auto const id = output(store, 0, PortKind::event);
    PersistedEventChunk chunk{.output = id, .range = span(0, 100), .type = EventTypeId::trigger};
    chunk.events.push_back({.time = at(10), .value = TriggerEvent{}});
    auto seed = store.begin_candidate(1);
    seed.overwrite(std::move(chunk));
    ASSERT_EQ(store.publish(std::move(seed)), PersistedDataPublishResult::published);
    EXPECT_GT(store.buffered_storage_usage().event_bytes, 0u);
    auto rejected = store.begin_candidate(2);
    PersistedEventChunk large{.output = id, .range = span(0, 100), .type = EventTypeId::trigger};
    for (int index = 0; index < 100; ++index) large.events.push_back({.time = at(index), .value = TriggerEvent{}});
    EXPECT_THROW(rejected.overwrite(std::move(large)), std::length_error);
    auto reader = store.register_reader();
    auto pin = reader.pin();
    EXPECT_EQ(pin->event_timeline(id, EventTypeId::trigger).size(), 1u);
}

TEST(SharedBufferedStorage, EventPressureReclaimsUnusedSampleBackingWithoutLosingReusableOffsets)
{
    PersistedDataStore probe;
    auto const page = probe.buffered_storage_usage().page_size;
    BufferedStorageLimits limits;
    limits.max_backing_bytes = page;
    PersistedDataStore store{limits};
    auto const sample_id = output(store), event_id = output(store, 1, PortKind::event);
    auto seed = store.begin_candidate(1);
    seed.overwrite(samples(sample_id, span(0, 1), 7));
    ASSERT_EQ(store.publish(std::move(seed)), PersistedDataPublishResult::published);
    auto erase = store.begin_candidate(2);
    erase.erase_output(sample_id);
    ASSERT_EQ(store.publish(std::move(erase)), PersistedDataPublishResult::published);
    (void)store.reclaim_retired();
    EXPECT_EQ(store.buffered_storage_usage().cached_page_bytes, page);

    auto events = store.begin_candidate(3);
    PersistedEventChunk chunk{.output = event_id, .range = span(0, 1), .type = EventTypeId::trigger};
    chunk.events.push_back({.time = at(0), .value = TriggerEvent{}});
    events.overwrite(std::move(chunk));
    ASSERT_EQ(store.publish(std::move(events)), PersistedDataPublishResult::published);
    EXPECT_EQ(store.buffered_storage_usage().backing_bytes, 0u);
    EXPECT_EQ(store.buffered_storage_usage().file_extent_bytes, page);
    EXPECT_EQ(store.buffered_storage_usage().reusable_page_bytes, page);
    auto remove = store.begin_candidate(4);
    remove.erase_output(event_id);
    ASSERT_EQ(store.publish(std::move(remove)), PersistedDataPublishResult::published);
    (void)store.reclaim_retired();
    EXPECT_EQ(store.buffered_storage_usage().event_bytes, 0u);
    auto refill = store.begin_candidate(5);
    refill.overwrite(samples(sample_id, span(0, 1), 99));
    ASSERT_EQ(store.publish(std::move(refill)), PersistedDataPublishResult::published);
    EXPECT_EQ(store.buffered_storage_usage().backing_bytes, page);
    EXPECT_EQ(store.buffered_storage_usage().file_extent_bytes, page);
    EXPECT_GT(store.buffered_storage_usage().reused_pages, 0u);
}

TEST(SharedBufferedStorage, TockViewsPreservePlanarChannelsAndSlicesAcrossTheSignedSeam)
{
    for (bool stereo : {false, true}) {
        PersistedDataStore store;
        auto const id = output(store);
        auto const begin = std::numeric_limits<std::int64_t>::max() - 2;
        ChannelLayout const layout{.channel_type = stereo ? ChannelTypeId::stereo : ChannelTypeId::mono,
            .sample_layout = SampleStreamLayout::planar};
        auto candidate = store.begin_candidate(1);
        replace(candidate, samples(id, span(begin, 6), 10, layout));
        ASSERT_EQ(store.publish(std::move(candidate)), PersistedDataPublishResult::published);
        auto reader = store.register_reader();
        auto pin = reader.pin();
        auto regions = pin->sample_regions(id, layout);
        ASSERT_EQ(regions.size(), 1u);
        GlobalIndexRange const selected{at(begin).advanced_by(2), 3};
        auto view = regions[0].selected_view(selected);
        for (std::size_t frame = 0; frame < 3; ++frame)
            for (std::size_t channel = 0; channel < channel_count(layout); ++channel)
                EXPECT_FLOAT_EQ(view.at(selected.begin.advanced_by(frame), channel).value,
                    12 + frame + 6 * channel);
        auto erase = store.begin_candidate(2);
        erase.erase_output(id);
        ASSERT_EQ(store.publish(std::move(erase)), PersistedDataPublishResult::published);
        pin = {};
        (void)store.reclaim_retired();
        EXPECT_FLOAT_EQ(view.at(selected.begin, 0).value, 12);
    }
}

TEST(SharedBufferedStorage, ResidentSnapshotFailureDoesNotAcquireAPartialLease)
{
    BufferedStorageLimits limits;
    limits.max_resident_bytes = 1;
    PersistedDataStore store{limits};
    auto input = make_tick_materialized_sample_input(1, mono_interleaved_channel_layout,
        {0}, std::vector{span(0, 4)}, {1.0f, 2.0f, 3.0f, 4.0f});
    TickMaterializationSnapshot snapshot{1, 1, {}, {std::move(input)}, {}};
    EXPECT_THROW(snapshot.prepare_resident(store), std::length_error);
    EXPECT_EQ(store.buffered_storage_usage().resident_bytes, 0u);
}

TEST(SharedBufferedStorage, OverlappingRealtimeSelectionsKeepResidencyUntilTheirLastLeaseRetires)
{
#if defined(__linux__)
    rlimit limit{};
    ASSERT_EQ(::getrlimit(RLIMIT_MEMLOCK, &limit), 0);
    PersistedDataStore probe;
    auto const page = probe.buffered_storage_usage().page_size;
    if (limit.rlim_cur < 2 * page) GTEST_SKIP() << "host cannot lock two pages";
    BufferedStorageLimits limits;
    limits.max_resident_bytes = page;
    PersistedDataStore store{limits};
    PersistedContinuousStorage payload{2 * page, page};
    std::array first{BufferedResidentRange{payload.data(), page}};
    auto one = store.prepare_resident(first);
    auto two = store.prepare_resident(first);
    EXPECT_EQ(store.buffered_storage_usage().resident_bytes, page);
    std::array too_large{BufferedResidentRange{payload.data(), 2 * page}};
    EXPECT_THROW((void)store.prepare_resident(too_large), std::length_error);
    EXPECT_EQ(store.buffered_storage_usage().resident_bytes, page);
    one.reset();
    EXPECT_EQ(store.buffered_storage_usage().resident_bytes, page);
    two.reset();
    EXPECT_EQ(store.buffered_storage_usage().resident_bytes, 0u);
#else
    GTEST_SKIP() << "Linux buffered storage backend";
#endif
}
TEST(SharedBufferedStorage, DenseReuseKeepsFileRunsContiguousWithAndWithoutPinnedVersions)
{
    for (bool tock : {false, true}) for (std::size_t pinned : {0u, 4u}) {
        PersistedDataStore store;
        auto const id = output(store);
        auto const page = store.buffered_storage_usage().page_size;
        auto const logical = span(0, 64 * page / sizeof(Sample));
        std::vector<PersistedDataStore::ReaderSlot> readers;
        std::vector<PersistedDataStore::ReaderPin> pins(pinned);
        for (std::size_t index = 0; index < pinned; ++index) readers.push_back(store.register_reader());
        for (std::uint64_t revision = 1; revision <= 24; ++revision) {
            if (pinned && revision > 1) {
                auto const slot = (revision - 2) % pinned;
                pins[slot] = {};
                pins[slot] = readers[slot].pin();
            }
            auto candidate = store.begin_candidate(revision);
            auto value = samples(id, logical, static_cast<float>(revision),
                tock ? mono_planar_channel_layout : mono_interleaved_channel_layout);
            if (tock) replace(candidate, std::move(value));
            else candidate.overwrite(std::move(value));
            EXPECT_LE(store.buffered_storage_usage().mapped_runs, pinned + 2);
            ASSERT_EQ(store.publish(std::move(candidate)), PersistedDataPublishResult::published);
            (void)store.reclaim_retired();
        }
        EXPECT_GT(store.buffered_storage_usage().reused_pages, 0u);
    }
}

TEST(SharedBufferedStorage, TockSamePageAndCrossPagePatchesKeepOnlyCanonicalLivePages)
{
    for (bool stereo : {false, true}) {
        PersistedDataStore store;
        auto const id = output(store);
        auto const page = store.buffered_storage_usage().page_size;
        auto const page_frames = page / sizeof(Sample), frames = 2 * page_frames;
        ChannelLayout const layout{.channel_type = stereo ? ChannelTypeId::stereo : ChannelTypeId::mono,
            .sample_layout = SampleStreamLayout::planar};
        auto const logical = span(-23, frames);
        auto initial = samples(id, logical, 1, layout);
        auto expected = initial.values;
        auto seed = store.begin_candidate(1);
        replace(seed, std::move(initial));
        ASSERT_EQ(store.publish(std::move(seed)), PersistedDataPublishResult::published);
        auto reader = store.register_reader();
        for (std::uint64_t revision = 2; revision <= 90; ++revision) {
            auto const offset = revision % 3 == 0 ? page_frames - 2 : (revision * 7) % (page_frames - 4);
            auto const count = revision % 3 == 0 ? 4u : 1u;
            GlobalIndexRange const changed{logical.begin.advanced_by(offset), count};
            auto window = samples(id, changed, static_cast<float>(10000 + revision * 10), layout);
            for (std::size_t channel = 0; channel < channel_count(layout); ++channel)
                for (std::size_t frame = 0; frame < count; ++frame)
                    expected[channel * frames + offset + frame] = window.values[channel * count + frame];
            PersistedSampleRegion incoming{.output = id, .range = logical, .layout = layout};
            incoming.set_prepared_windows({std::move(window)});
            auto update = store.begin_candidate(revision);
            update.replace_sample_regions(id, layout, incoming.geometry, IndexCoverage{changed},
                std::array{logical}, {std::move(incoming)});
            ASSERT_EQ(store.publish(std::move(update)), PersistedDataPublishResult::published);
            (void)store.reclaim_retired();
            EXPECT_EQ(store.buffered_storage_usage().live_page_bytes, 2 * page * channel_count(layout));
            auto pin = reader.pin();
            auto regions = pin->sample_regions(id, layout);
            ASSERT_EQ(regions.size(), 1u);
            for (std::size_t channel = 0; channel < channel_count(layout); ++channel)
                for (std::size_t frame = 0; frame < frames; ++frame)
                    ASSERT_EQ(regions[0].at(logical.begin.advanced_by(frame), channel), expected[channel * frames + frame]);
        }
    }
}

TEST(SharedBufferedStorage, MultipleIncomingWindowsCopyOnePageOnceAndKeepSparseValidity)
{
    PersistedDataStore store;
    auto const id = output(store);
    auto const page = store.buffered_storage_usage().page_size;
    auto const logical = span(0, page / sizeof(Sample));
    auto seed = store.begin_candidate(1);
    replace(seed, samples(id, logical, 1, mono_planar_channel_layout));
    ASSERT_EQ(store.publish(std::move(seed)), PersistedDataPublishResult::published);
    auto reader = store.register_reader();
    auto old = reader.pin();
    PersistedSampleRegion incoming{.output = id, .range = logical, .layout = mono_planar_channel_layout};
    incoming.set_prepared_windows({samples(id, span(5, 1), 9000, incoming.layout),
        samples(id, span(7, 2), 8000, incoming.layout)});
    auto update = store.begin_candidate(2);
    update.replace_sample_regions(id, incoming.layout, incoming.geometry,
        IndexCoverage{span(5, 1), span(7, 2)}, std::array{logical}, {std::move(incoming)});
    ASSERT_EQ(store.publish(std::move(update)), PersistedDataPublishResult::published);
    EXPECT_EQ(store.buffered_storage_usage().live_page_bytes, 2 * page);
    EXPECT_FLOAT_EQ(old->sample_regions(id, mono_planar_channel_layout)[0].at(at(5), 0).value, 6);
    old = {};
    (void)store.reclaim_retired();
    EXPECT_EQ(store.buffered_storage_usage().live_page_bytes, page);
    auto erase = store.begin_candidate(3);
    erase.erase_sample(id, span(6, 1), mono_planar_channel_layout);
    ASSERT_EQ(store.publish(std::move(erase)), PersistedDataPublishResult::published);
    (void)store.reclaim_retired();
    auto current = reader.pin();
    EXPECT_EQ(current->find_sample_region(id, at(6)), nullptr);
    auto regions = current->sample_regions(id, mono_planar_channel_layout);
    EXPECT_FALSE(regions[0].materialized_coverage().contains(at(6)));
    EXPECT_FLOAT_EQ(regions[0].at(at(5), 0).value, 9000);
    EXPECT_FLOAT_EQ(regions[0].at(at(7), 0).value, 8000);
    EXPECT_EQ(store.buffered_storage_usage().live_page_bytes, page);
}

TEST(SharedBufferedStorage, PlanarWindowsMapSharedPagesAndRetainOnlyTheirSelectedChannels)
{
    PersistedDataStore probe;
    auto const page = probe.buffered_storage_usage().page_size;
    BufferedStorageLimits limits;
    limits.max_contiguous_mapping_bytes = 2 * page;
    PersistedDataStore store{limits};
    auto const id = output(store);
    ChannelLayout const layout{.channel_type = ChannelTypeId::stereo, .sample_layout = SampleStreamLayout::planar};
    auto const frames = page / sizeof(Sample);
    auto const logical = span(0, 2 * frames);
    auto seed = store.begin_candidate(1);
    replace(seed, samples(id, logical, 1, layout));
    ASSERT_EQ(store.publish(std::move(seed)), PersistedDataPublishResult::published);
    auto reader = store.register_reader();
    auto old = reader.pin();
    auto const regions = old->sample_regions(id, layout);
    ASSERT_EQ(regions.size(), 1u);
    ASSERT_NE(regions[0].windows, nullptr);
    ASSERT_EQ(regions[0].windows->size(), 2u);
    EXPECT_EQ(store.buffered_storage_usage().live_page_bytes, 4 * page);
    auto const* second_data = (*regions[0].windows)[1].storage_data();
    auto first_mapping = (*regions[0].windows)[0].continuous_values;
    PersistedSampleRegion patch{.output = id, .range = logical, .layout = layout};
    patch.set_prepared_windows({samples(id, span(0, 1), 9000, layout)});
    auto update = store.begin_candidate(2);
    update.replace_sample_regions(id, layout, patch.geometry, IndexCoverage{span(0, 1)},
        std::array{logical}, {std::move(patch)});
    ASSERT_EQ(store.publish(std::move(update)), PersistedDataPublishResult::published);
    auto another = store.register_reader();
    {
        auto current = another.pin();
        auto now = current->sample_regions(id, layout);
        EXPECT_EQ((*now[0].windows)[1].storage_data(), second_data);
        EXPECT_FLOAT_EQ(now[0].at(at(0), 0).value, 9000);
        EXPECT_FLOAT_EQ(now[0].at(at(0), 1).value, 9001);
    }
    old = {};
    (void)store.reclaim_retired();
    auto erase = store.begin_candidate(3);
    erase.erase_output(id);
    ASSERT_EQ(store.publish(std::move(erase)), PersistedDataPublishResult::published);
    (void)store.reclaim_retired();
    EXPECT_EQ(store.buffered_storage_usage().live_page_bytes, 2 * page);
    auto const* held = reinterpret_cast<Sample const*>(first_mapping.data());
    EXPECT_FLOAT_EQ(held[0].value, 1);
    EXPECT_FLOAT_EQ(held[frames].value, 1 + 2 * frames);
    first_mapping = {};
    EXPECT_EQ(store.buffered_storage_usage().live_page_bytes, 0u);
}

TEST(SharedBufferedStorage, DuplicateAndOverlappingRegionsKeepIndependentCanonicalPages)
{
    PersistedDataStore store;
    auto const id = output(store);
    auto const page = store.buffered_storage_usage().page_size;
    auto const layout = mono_planar_channel_layout;
    std::array order{span(0, 8), span(0, 8), span(4, 8)};
    auto seed = store.begin_candidate(1);
    auto geometry = samples(id, order[0], 1, layout).geometry;
    seed.replace_sample_regions(id, layout, geometry, IndexCoverage{span(0, 12)}, order,
        {samples(id, order[0], 1, layout), samples(id, order[1], 100, layout), samples(id, order[2], 200, layout)});
    ASSERT_EQ(store.publish(std::move(seed)), PersistedDataPublishResult::published);
    auto a = samples(id, order[0], 0, layout), b = samples(id, order[1], 0, layout);
    a.values.clear(); b.values.clear();
    a.set_prepared_windows({samples(id, span(5, 1), 9000, layout)});
    b.set_prepared_windows({samples(id, span(5, 1), 8000, layout)});
    auto update = store.begin_candidate(2);
    update.replace_sample_regions(id, layout, geometry, IndexCoverage{span(5, 1)}, order, {std::move(a), std::move(b)});
    ASSERT_EQ(store.publish(std::move(update)), PersistedDataPublishResult::published);
    (void)store.reclaim_retired();
    auto reader = store.register_reader();
    auto pin = reader.pin();
    auto regions = pin->sample_regions(id, layout);
    ASSERT_EQ(regions.size(), 3u);
    EXPECT_FLOAT_EQ(regions[0].at(at(5), 0).value, 9000);
    EXPECT_FLOAT_EQ(regions[1].at(at(5), 0).value, 8000);
    EXPECT_FALSE(regions[2].materialized_coverage().contains(at(5)));
    EXPECT_FLOAT_EQ(regions[2].at(at(6), 0).value, 202);
    EXPECT_EQ(store.buffered_storage_usage().live_page_bytes, 3 * page);
}

TEST(SharedBufferedStorage, SparseWindowsShareOnePageAndLeaveUnpreparedPositionsInvalid)
{
    PersistedDataStore store;
    auto const id = output(store);
    auto const page = store.buffered_storage_usage().page_size;
    auto const logical = span(0, page / sizeof(Sample));
    PersistedSampleRegion initial{.output = id, .range = logical, .layout = mono_planar_channel_layout};
    initial.set_prepared_windows({samples(id, span(10, 1), 10, initial.layout),
        samples(id, span(20, 1), 20, initial.layout)});
    auto seed = store.begin_candidate(1);
    seed.replace_sample_regions(id, initial.layout, initial.geometry, IndexCoverage{span(10, 1), span(20, 1)},
        std::array{logical}, {std::move(initial)});
    ASSERT_EQ(store.publish(std::move(seed)), PersistedDataPublishResult::published);
    EXPECT_EQ(store.buffered_storage_usage().live_page_bytes, page);
    auto reader = store.register_reader();
    auto old = reader.pin();
    PersistedSampleRegion incoming{.output = id, .range = logical, .layout = mono_planar_channel_layout};
    incoming.set_prepared_windows({samples(id, span(10, 1), 90, incoming.layout)});
    auto update = store.begin_candidate(2);
    update.replace_sample_regions(id, incoming.layout, incoming.geometry, IndexCoverage{span(10, 1)},
        std::array{logical}, {std::move(incoming)});
    ASSERT_EQ(store.publish(std::move(update)), PersistedDataPublishResult::published);
    EXPECT_FLOAT_EQ(old->find_sample_region(id, at(10))->at(at(10), 0).value, 10);
    old = {};
    (void)store.reclaim_retired();
    EXPECT_EQ(store.buffered_storage_usage().live_page_bytes, page);
    auto current = reader.pin();
    EXPECT_EQ(current->find_sample_region(id, at(15)), nullptr);
    EXPECT_FLOAT_EQ(current->find_sample_region(id, at(10))->at(at(10), 0).value, 90);
    EXPECT_FLOAT_EQ(current->find_sample_region(id, at(20))->at(at(20), 0).value, 20);
}

TEST(SharedBufferedStorage, UnalignedPlanarSelectionRepacksOnlyItsBoundedViewAndPinsItsSourceSubset)
{
    PersistedDataStore probe;
    auto const page = probe.buffered_storage_usage().page_size;
    BufferedStorageLimits limits;
    limits.max_contiguous_mapping_bytes = 2 * page;
    PersistedDataStore store{limits};
    auto const id = output(store);
    auto const frames = 2 * page / sizeof(Sample) + 7;
    ChannelLayout const layout{.channel_type = ChannelTypeId::stereo, .sample_layout = SampleStreamLayout::planar};
    auto const logical = span(-19, frames);
    auto seed = store.begin_candidate(1);
    replace(seed, samples(id, logical, 1, layout));
    ASSERT_EQ(store.publish(std::move(seed)), PersistedDataPublishResult::published);
    auto reader = store.register_reader();
    auto pin = reader.pin();
    auto regions = pin->sample_regions(id, layout);
    auto selected = regions[0].selected_view(span(-14, 3));
    EXPECT_THROW((void)regions[0].contiguous_view(), std::length_error);
    auto erase = store.begin_candidate(2);
    erase.erase_output(id);
    ASSERT_EQ(store.publish(std::move(erase)), PersistedDataPublishResult::published);
    pin = {};
    (void)store.reclaim_retired();
    EXPECT_EQ(store.buffered_storage_usage().live_page_bytes, 3 * page);
    for (std::size_t frame = 0; frame < 3; ++frame) {
        EXPECT_FLOAT_EQ(selected.at(at(-14).advanced_by(frame), 0).value, 6 + frame);
        EXPECT_FLOAT_EQ(selected.at(at(-14).advanced_by(frame), 1).value, 6 + frames + frame);
    }
    selected = {};
    EXPECT_EQ(store.buffered_storage_usage().live_page_bytes, 0u);
}

TEST(SharedBufferedStorage, EvictionPreservesLogicalVersionsAndAuthoritativeRecordings)
{
    PersistedDataStore store;
    auto recording = output(store, 0), cached = output(store, 1);
    auto seed = store.begin_candidate(1);
    seed.overwrite(samples(recording, span(0, 4), 12));
    replace(seed, samples(cached, span(-4, 8), 20, mono_planar_channel_layout));
    ASSERT_EQ(store.publish(std::move(seed)), PersistedDataPublishResult::published);
    auto reader = store.register_reader();
    auto old = reader.pin();
    auto version = old->version();
    store.reclaim_cached_payload({});
    auto other = store.register_reader();
    auto current = other.pin();
    EXPECT_EQ(current->version(), version);
    EXPECT_TRUE(store.is_current(old.snapshot()));
    EXPECT_TRUE(current->shares_output_with(old.snapshot(), cached));
    EXPECT_EQ(*current->find_sample_logical_coverage(cached), IndexCoverage{span(-4, 8)});
    EXPECT_TRUE(current->find_sample_coverage(cached, mono_planar_channel_layout)->empty());
    ASSERT_EQ(current->sample_regions(cached, mono_planar_channel_layout).size(), 1u);
    EXPECT_EQ(current->sample_regions(cached, mono_planar_channel_layout)[0].range, span(-4, 8));
    EXPECT_FLOAT_EQ(current->find_sample_region(recording, at(0))->at(at(0), 0).value, 12);
    EXPECT_FLOAT_EQ(old->find_sample_region(cached, at(-4))->at(at(-4), 0).value, 20);
}

TEST(SharedBufferedStorage, DetachedPreparedReaderPinsOnlyItsSelectedPages)
{
    PersistedDataStore store;
    auto id = output(store);
    auto page = store.buffered_storage_usage().page_size, frames = page / sizeof(Sample);
    auto seed = store.begin_candidate(1);
    replace(seed, samples(id, span(0, 2 * frames), 0, mono_planar_channel_layout));
    ASSERT_EQ(store.publish(std::move(seed)), PersistedDataPublishResult::published);
    auto reader = store.register_reader();
    auto lease = reader.pin();
    lease.retain_payload(std::array{PersistedPayloadSelection{id, IndexCoverage{span(0, frames)}}});
    store.reclaim_cached_payload({});
    EXPECT_EQ(store.buffered_storage_usage().live_page_bytes, page);
    EXPECT_FLOAT_EQ(lease->find_sample_region(id, at(3))->at(at(3), 0).value, 3);
    EXPECT_EQ(*lease->find_sample_logical_coverage(id), IndexCoverage{span(0, 2 * frames)});
    lease = {};
    EXPECT_EQ(store.buffered_storage_usage().live_page_bytes, 0u);
}

TEST(SharedBufferedStorage, CachePressureEvictsComputedBeforeBufferedAndKeepsRecordings)
{
    PersistedDataStore probe;
    auto page = probe.buffered_storage_usage().page_size, frames = page / sizeof(Sample);
    BufferedStorageLimits limits;
    limits.max_backing_bytes = 3 * page;
    PersistedDataStore store(limits);
    auto recording = output(store, 0), buffered = output(store, 1), computed = output(store, 2);
    auto seed = store.begin_candidate(1);
    seed.overwrite(samples(recording, span(0, frames), 1));
    replace(seed, samples(buffered, span(0, frames), 2, mono_planar_channel_layout));
    replace(seed, samples(computed, span(0, frames), 3, mono_planar_channel_layout));
    ASSERT_EQ(store.publish(std::move(seed)), PersistedDataPublishResult::published);
    (void)store.reclaim_retired();
    store.set_cache_realization(computed, OutputRealization::computed);
    store.prepare_cache(page, {}, [](auto) {});
    auto reader = store.register_reader();
    auto pin = reader.pin();
    EXPECT_NE(pin->find_sample_region(recording, at(0)), nullptr);
    EXPECT_NE(pin->find_sample_region(buffered, at(0)), nullptr);
    EXPECT_EQ(pin->find_sample_region(computed, at(0)), nullptr);
    EXPECT_EQ(store.buffered_storage_usage().live_page_bytes, 2 * page);
    EXPECT_GT(store.buffered_storage_usage().cache_evictions, 0u);
}

TEST(SharedBufferedStorage, CachePressureRetainsRequiredWindowsWithoutFabricatingCoverage)
{
    PersistedDataStore probe;
    auto page = probe.buffered_storage_usage().page_size, frames = page / sizeof(Sample);
    BufferedStorageLimits limits;
    limits.max_backing_bytes = 2 * page;
    PersistedDataStore store(limits);
    auto id = output(store);
    auto seed = store.begin_candidate(1);
    replace(seed, samples(id, span(0, 2 * frames), 7, mono_planar_channel_layout));
    ASSERT_EQ(store.publish(std::move(seed)), PersistedDataPublishResult::published);
    (void)store.reclaim_retired();
    auto required = std::array{PersistedPayloadSelection{id, IndexCoverage{span(0, frames)}}};
    store.prepare_cache(page, required, [](auto) {});
    auto reader = store.register_reader();
    auto pin = reader.pin();
    EXPECT_EQ(*pin->find_sample_coverage(id, mono_planar_channel_layout), IndexCoverage{span(0, frames)});
    EXPECT_EQ(*pin->find_sample_logical_coverage(id), IndexCoverage{span(0, 2 * frames)});
    EXPECT_EQ(store.buffered_storage_usage().live_page_bytes, page);
}

TEST(SharedBufferedStorage, CacheReclamationDoesNotMakeAnUnchangedSemanticCandidateStale)
{
    PersistedDataStore store;
    auto cached = output(store, 0), recording = output(store, 1);
    auto seed = store.begin_candidate(1);
    replace(seed, samples(cached, span(0, 4), 4, mono_planar_channel_layout));
    ASSERT_EQ(store.publish(std::move(seed)), PersistedDataPublishResult::published);
    auto successor = store.begin_candidate(2);
    successor.retain_payload({});
    store.reclaim_cached_payload({});
    successor.overwrite(samples(recording, span(0, 4), 9));
    ASSERT_EQ(store.publish(std::move(successor)), PersistedDataPublishResult::published);
    auto stale = store.begin_candidate(3);
    auto winner = store.begin_candidate(3);
    ASSERT_EQ(store.publish(std::move(winner)), PersistedDataPublishResult::published);
    EXPECT_EQ(store.publish(std::move(stale)), PersistedDataPublishResult::stale_base);
}

TEST(SharedBufferedStorage, ReconstructionKeepsExactRecordedInputsWithoutKeepingResultPages)
{
    PersistedDataStore store;
    auto recording = output(store, 0), derived = output(store, 1);
    auto code = std::make_shared<CompiledGraph>();
    std::weak_ptr<CompiledGraph> code_lifetime = code;
    auto reader = store.register_reader();
    auto previous = reader.pin();
    auto seed = store.begin_candidate(1);
    seed.overwrite(samples(recording, span(0, 4), 10));
    replace(seed, samples(derived, span(0, 4), 20, mono_planar_channel_layout));
    auto inputs = std::make_shared<PersistedDataStore::Snapshot>(seed.working_snapshot());
    inputs->select_outputs(std::array{recording});
    auto context = std::make_shared<PersistedDataStore::Reconstruction>();
    context->executable = code;
    context->inputs = std::move(inputs);
    context->dependencies = {recording};
    seed.set_reconstruction(derived, std::move(context), previous.snapshot());
    ASSERT_EQ(store.publish(std::move(seed)), PersistedDataPublishResult::published);
    previous = {};
    code.reset();
    auto changed = store.begin_candidate(2);
    changed.overwrite(samples(recording, span(0, 4), 50));
    ASSERT_EQ(store.publish(std::move(changed)), PersistedDataPublishResult::published);
    store.reclaim_cached_payload({});
    {
        auto pin = reader.pin();
        auto selected = pin->reconstruction(derived);
        ASSERT_NE(selected, nullptr);
        EXPECT_FLOAT_EQ(selected->inputs->find_sample_region(recording, at(0))->at(at(0), 0).value, 10);
        EXPECT_FLOAT_EQ(pin->find_sample_region(recording, at(0))->at(at(0), 0).value, 50);
        EXPECT_EQ(pin->find_sample_region(derived, at(0)), nullptr);
        EXPECT_FALSE(code_lifetime.expired());
    }
    auto remove = store.begin_candidate(3);
    remove.erase_output(derived);
    ASSERT_EQ(store.publish(std::move(remove)), PersistedDataPublishResult::published);
    (void)store.reclaim_retired();
    EXPECT_TRUE(code_lifetime.expired());
}

TEST(SharedBufferedStorage, EventEvictionPreservesDuplicateRegionsAndKnownEmptyResults)
{
    PersistedDataStore store;
    auto events = output(store, 0, PortKind::event);
    auto recording = output(store, 1, PortKind::event);
    PersistedEventChunk active{.output = events, .range = span(10, 8), .type = EventTypeId::trigger};
    active.events.push_back({.time = at(12), .value = TriggerEvent{}});
    PersistedEventChunk empty{.output = events, .range = span(10, 8), .type = EventTypeId::trigger};
    PersistedEventChunk recorded{.output = recording, .range = span(10, 8), .type = EventTypeId::trigger};
    recorded.events.push_back({.time = at(13), .value = TriggerEvent{}});
    auto seed = store.begin_candidate(1);
    seed.replace_event_regions(events, EventTypeId::trigger, IndexCoverage{span(10, 8)},
        std::array{span(10, 8), span(10, 8)}, {std::move(active), std::move(empty)});
    seed.overwrite(std::move(recorded));
    ASSERT_EQ(store.publish(std::move(seed)), PersistedDataPublishResult::published);
    auto reader = store.register_reader();
    auto old = reader.pin();
    auto old_count = store.buffered_storage_usage().event_bytes;
    store.reclaim_cached_payload({});
    auto other = store.register_reader();
    auto pin = other.pin();
    EXPECT_EQ(pin->event_region_ranges(events, EventTypeId::trigger).size(), 2u);
    EXPECT_EQ(*pin->find_event_logical_coverage(events), IndexCoverage{span(10, 8)});
    EXPECT_TRUE(pin->find_event_coverage(events, EventTypeId::trigger)->empty());
    EXPECT_EQ(old->event_timeline(events, EventTypeId::trigger).size(), 1u);
    EXPECT_EQ(pin->event_timeline(recording, EventTypeId::trigger).size(), 1u);
    EXPECT_TRUE(pin->shares_output_with(old.snapshot(), events));
    old = {};
    (void)store.reclaim_retired();
    EXPECT_LT(store.buffered_storage_usage().event_bytes, old_count);
}

TEST(SharedBufferedStorage, PressureCannotDiscardPinnedPagesOrTheOnlyRecordingCopy)
{
    PersistedDataStore probe;
    auto page = probe.buffered_storage_usage().page_size, frames = page / sizeof(Sample);
    BufferedStorageLimits limits;
    limits.max_backing_bytes = 2 * page;
    PersistedDataStore store(limits);
    auto recording = output(store, 0), cached = output(store, 1);
    auto seed = store.begin_candidate(1);
    seed.overwrite(samples(recording, span(0, frames), 1));
    replace(seed, samples(cached, span(0, frames), 2, mono_planar_channel_layout));
    ASSERT_EQ(store.publish(std::move(seed)), PersistedDataPublishResult::published);
    auto reader = store.register_reader();
    auto pin = reader.pin();
    store.prepare_cache(page, {}, [](auto) {});
    EXPECT_EQ(store.buffered_storage_usage().live_page_bytes, 2 * page);
    auto failed = store.begin_candidate(2);
    EXPECT_THROW(failed.overwrite(samples(recording, span(frames, frames), 3)), std::length_error);
    EXPECT_FLOAT_EQ(pin->find_sample_region(cached, at(0))->at(at(0), 0).value, 2);
    pin = {};
    (void)store.reclaim_retired();
    EXPECT_EQ(store.buffered_storage_usage().live_page_bytes, page);
    auto accepted = store.begin_candidate(2);
    accepted.overwrite(samples(recording, span(frames, frames), 3));
    ASSERT_EQ(store.publish(std::move(accepted)), PersistedDataPublishResult::published);
    auto final = reader.pin();
    EXPECT_FLOAT_EQ(final->find_sample_region(recording, at(0))->at(at(0), 0).value, 1);
    EXPECT_FLOAT_EQ(final->find_sample_region(recording, at(frames))->at(at(frames), 0).value, 3);
}

} // namespace
