#include <intravenous/runtime/persisted_page_store.h>
#include <intravenous/runtime/tick_invocation_frame.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

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

iv::graph_jit::BackgroundEvaluationPlan direct_tick_sample_plan()
{
    using namespace iv::graph_jit;
    BackgroundEvaluationPlan plan;
    plan.nodes = {
        BackgroundNodePlan{.outputs = {0}},
        BackgroundNodePlan{.inputs = {1}},
    };
    plan.ports = {
        BackgroundPortPlan{
            .node = 0,
            .configured_port = {0, iv::PortKind::sample, 0},
            .kind = iv::PortKind::sample,
            .direction = PortDirection::output,
            .persisted_tick_output = true,
            .retention = iv::OutputRetention::persisted,
            .sample_layout = {
                .channel_type = iv::ChannelTypeId::mono,
                .sample_layout = iv::SampleStreamLayout::planar,
            },
        },
        BackgroundPortPlan{
            .node = 1,
            .configured_port = {1, iv::PortKind::sample, 0},
            .kind = iv::PortKind::sample,
            .direction = PortDirection::input,
            .random_access_input = true,
            .sample_layout = {
                .channel_type = iv::ChannelTypeId::mono,
                .sample_layout = iv::SampleStreamLayout::planar,
            },
        },
    };
    plan.sample_target_subsets = {{
        .port = {1, iv::PortKind::sample, 0},
        .target_layout = {
            .channel_type = iv::ChannelTypeId::mono,
            .sample_layout = iv::SampleStreamLayout::planar,
        },
        .access = PlannedDestinationAccess::random_access,
        .channels = {{1, 0, 0}},
    }};
    plan.storage.ports = {{
        .kind = iv::PortKind::sample,
        .storage = PortStorageKind::persisted_pages,
        .source_subsets = {0},
        .target_subsets = {0},
        .source_port = iv::NodeBundlePortId{0, iv::PortKind::sample, 0},
        .output_port = 0,
        .sample_layout = {
            .channel_type = iv::ChannelTypeId::mono,
            .sample_layout = iv::SampleStreamLayout::planar,
        },
        .sample_channels = {0},
    }};
    plan.storage.sample_target_storage = {{0}};
    plan.storage.direct_samples = {{
        .target_subset = 0,
        .target_channel = 0,
        .source_subset = 0,
        .source_channel = 0,
        .storage = 0,
        .delivery = PlannedDeliveryMechanism::persisted_tick_to_random_access,
    }};
    plan.tick_runtime.random_access_sample_inputs = {{
        .port = 1,
        .storage = {0},
    }};
    plan.tick_runtime.nodes = {
        TickNodeInvocationPlan{},
        TickNodeInvocationPlan{
            .random_access_sample_begin = 0,
            .random_access_sample_count = 1,
        },
    };
    return plan;
}

iv::graph_jit::BackgroundEvaluationPlan direct_tick_event_plan()
{
    using namespace iv::graph_jit;
    BackgroundEvaluationPlan plan;
    plan.nodes = {
        BackgroundNodePlan{.outputs = {0}},
        BackgroundNodePlan{.inputs = {1}},
    };
    plan.ports = {
        BackgroundPortPlan{
            .node = 0,
            .configured_port = {0, iv::PortKind::event, 0},
            .kind = iv::PortKind::event,
            .direction = PortDirection::output,
            .event_type = iv::EventTypeId::trigger,
        },
        BackgroundPortPlan{
            .node = 1,
            .configured_port = {1, iv::PortKind::event, 0},
            .kind = iv::PortKind::event,
            .direction = PortDirection::input,
            .random_access_input = true,
            .event_type = iv::EventTypeId::trigger,
        },
    };
    plan.event_target_subsets = {{
        .port = {1, 0},
        .type = iv::EventTypeId::trigger,
        .access = PlannedDestinationAccess::random_access,
    }};
    plan.storage.ports = {{
        .kind = iv::PortKind::event,
        .storage = PortStorageKind::persisted_pages,
        .output_port = 0,
        .event_type = iv::EventTypeId::trigger,
    }};
    plan.storage.direct_events = {{
        .target_subset = 0,
        .storage = 0,
    }};
    plan.tick_runtime.random_access_event_inputs = {{
        .port = 1,
        .storage = {0},
    }};
    plan.tick_runtime.nodes = {
        TickNodeInvocationPlan{},
        TickNodeInvocationPlan{
            .random_access_event_begin = 0,
            .random_access_event_count = 1,
        },
    };
    return plan;
}

static_assert(noexcept(
    std::declval<iv::PersistedPageStore::ReaderSlot&>().pin()));
static_assert(std::is_nothrow_destructible_v<
    iv::PersistedPageStore::ReaderPin>);
static_assert(std::is_nothrow_constructible_v<
    iv::TickInvocationFrame,
    iv::PersistedPageStore::ReaderSlot&,
    iv::TickMaterializationStore::ReaderSlot&,
    iv::TickInvocationWorkspace&>);

TEST(PersistedPageStore, TickInvocationFramePinsOnePublishedRootForItsLifetime)
{
    iv::PersistedPageStore store;
    auto slot = store.register_reader();
    iv::TickMaterializationStore materializations;
    auto materialization_slot = materializations.register_reader();
    iv::graph_jit::BackgroundEvaluationPlan plan;
    iv::TickInvocationWorkspace workspace{plan, 1};

    {
        iv::TickInvocationFrame frame{
            slot, materialization_slot, workspace};
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

TEST(PersistedPageStore, TickInvocationFrameBindsPublishedRandomAccessSamples)
{
    auto plan = direct_tick_sample_plan();
    iv::PersistedPageStore store;
    auto reader = store.register_reader();
    iv::TickMaterializationStore materializations;
    auto materialization_reader = materializations.register_reader();
    auto const output = local_output(iv::PortKind::sample, 0);
    auto candidate = store.begin_candidate(3, 4);
    candidate.put(sample_page(output, 10.0f));
    ASSERT_EQ(
        store.publish(std::move(candidate)),
        iv::PersistedPagePublishResult::published);

    iv::TickInvocationWorkspace workspace{plan, 3};
    iv::TickInvocationFrame frame{
        reader, materialization_reader, workspace};
    auto const inputs = static_cast<
        std::span<iv::RandomAccessSampleInputPort const>>(
        frame.call().random_access_sample_inputs);
    ASSERT_EQ(inputs.size(), 1u);
    EXPECT_EQ(inputs[0].coverage(), (iv::Coverage{{{0, 4}}}));
    EXPECT_FLOAT_EQ(inputs[0].at(0).value, 10.0f);
    EXPECT_FLOAT_EQ(inputs[0].at(3).value, 13.0f);
}

TEST(PersistedPageStore, TickSampleViewsFollowOnePinnedRootAndPackedPageCoverage)
{
    auto plan = direct_tick_sample_plan();
    iv::PersistedPageStore store;
    auto old_reader = store.register_reader();
    auto new_reader = store.register_reader();
    iv::TickMaterializationStore materializations;
    auto old_materialization_reader = materializations.register_reader();
    auto new_materialization_reader = materializations.register_reader();
    auto const output = local_output(iv::PortKind::sample, 0);
    auto first = store.begin_candidate(3, 4);
    first.put(sample_page(output, 10.0f));
    ASSERT_EQ(store.publish(std::move(first)),
        iv::PersistedPagePublishResult::published);

    iv::TickInvocationWorkspace old_workspace{plan, 3};
    iv::TickInvocationWorkspace new_workspace{plan, 3};
    iv::TickInvocationFrame old_frame{
        old_reader, old_materialization_reader, old_workspace};
    auto const old_inputs = static_cast<
        std::span<iv::RandomAccessSampleInputPort const>>(
        old_frame.call().random_access_sample_inputs);
    ASSERT_EQ(old_inputs.size(), 1u);
    EXPECT_EQ(old_inputs[0].coverage(), (iv::Coverage{{{0, 4}}}));

    auto second = store.begin_candidate(3, 4);
    second.put(iv::PersistedSamplePage{
        .output = output,
        .page_index = 1,
        .domain = iv::Coverage{{{4, 5}, {7, 8}}},
        .layout = plan.ports[0].sample_layout,
        .packing = iv::PersistedSamplePacking::coverage_packed,
        .values = {40.0f, 70.0f},
    });
    ASSERT_EQ(store.publish(std::move(second)),
        iv::PersistedPagePublishResult::published);
    EXPECT_EQ(old_inputs[0].coverage(), (iv::Coverage{{{0, 4}}}));
    EXPECT_FLOAT_EQ(old_inputs[0].at(3).value, 13.0f);

    iv::TickInvocationFrame new_frame{
        new_reader, new_materialization_reader, new_workspace};
    auto const new_inputs = static_cast<
        std::span<iv::RandomAccessSampleInputPort const>>(
        new_frame.call().random_access_sample_inputs);
    ASSERT_EQ(new_inputs.size(), 1u);
    EXPECT_EQ(new_inputs[0].coverage(),
        (iv::Coverage{{{0, 5}, {7, 8}}}));
    EXPECT_FLOAT_EQ(new_inputs[0].at(4).value, 40.0f);
    EXPECT_FLOAT_EQ(new_inputs[0].at(7).value, 70.0f);
}

TEST(PersistedPageStore, TickEventViewsVisitAcrossPinnedPagesInTimeOrder)
{
    auto plan = direct_tick_event_plan();
    iv::PersistedPageStore store;
    auto reader = store.register_reader();
    iv::TickMaterializationStore materializations;
    auto materialization_reader = materializations.register_reader();
    auto const output = local_output(iv::PortKind::event, 0);
    auto candidate = store.begin_candidate(3, 4);
    auto first = empty_event_page(output);
    first.events = {{.time = 2, .value = iv::TriggerEvent{}}};
    candidate.put(std::move(first));
    auto second = empty_event_page(output);
    second.page_index = 1;
    second.domain = iv::Coverage{{{4, 8}}};
    second.events = {
        {.time = 0, .value = iv::TriggerEvent{}},
        {.time = 3, .value = iv::TriggerEvent{}},
    };
    candidate.put(std::move(second));
    ASSERT_EQ(store.publish(std::move(candidate)),
        iv::PersistedPagePublishResult::published);

    iv::TickInvocationWorkspace workspace{plan, 3};
    iv::TickInvocationFrame frame{
        reader, materialization_reader, workspace};
    auto const inputs = static_cast<
        std::span<iv::RandomAccessEventInputPort const>>(
        frame.call().random_access_event_inputs);
    ASSERT_EQ(inputs.size(), 1u);
    EXPECT_EQ(inputs[0].coverage(), (iv::Coverage{{{0, 8}}}));
    std::vector<iv::EventTime> times;
    inputs[0].for_each(1, 8, [&](iv::TimedEvent const& event) {
        times.push_back(event.time);
    });
    EXPECT_EQ(times, (std::vector<iv::EventTime>{2, 4, 7}));
}

TEST(PersistedPageStore, TickViewsDoNotTreatMixedStorageAsDirectPages)
{
    auto plan = direct_tick_sample_plan();
    auto derived = plan.storage.ports.front();
    derived.storage = iv::graph_jit::PortStorageKind::tick_random_access;
    plan.storage.ports.push_back(std::move(derived));
    plan.tick_runtime.random_access_sample_inputs.front().storage.push_back(1);

    iv::PersistedPageStore store;
    auto reader = store.register_reader();
    iv::TickMaterializationStore materializations;
    auto materialization_reader = materializations.register_reader();
    auto candidate = store.begin_candidate(3, 4);
    candidate.put(sample_page(local_output(iv::PortKind::sample, 0), 10.0f));
    ASSERT_EQ(store.publish(std::move(candidate)),
        iv::PersistedPagePublishResult::published);

    iv::TickInvocationWorkspace workspace{plan, 3};
    iv::TickInvocationFrame frame{
        reader, materialization_reader, workspace};
    auto const inputs = static_cast<
        std::span<iv::RandomAccessSampleInputPort const>>(
        frame.call().random_access_sample_inputs);
    ASSERT_EQ(inputs.size(), 1u);
    EXPECT_TRUE(inputs[0].coverage().empty());
}

TEST(PersistedPageStore, TickFrameBindsOneCoherentMaterializationSnapshot)
{
    auto plan = direct_tick_sample_plan();
    auto materialized_storage = plan.storage.ports.front();
    materialized_storage.storage =
        iv::graph_jit::PortStorageKind::tick_random_access;
    materialized_storage.output_port.reset();
    plan.storage.ports = {std::move(materialized_storage)};
    plan.tick_runtime.random_access_sample_inputs.front().storage = {0};

    iv::PersistedPageStore pages;
    auto page_reader = pages.register_reader();
    iv::TickMaterializationStore materializations;
    auto materialization_reader = materializations.register_reader();
    ASSERT_EQ(materializations.promote(
        std::make_unique<iv::TickMaterializationSnapshot>(
            3,
            7,
            iv::PersistedPageSnapshotVersion{},
            std::vector<iv::TickMaterializedSampleInput>{
                {
                    .port = 1,
                    .coverage = iv::Coverage{{{4, 6}}},
                    .layout = plan.ports[1].sample_layout,
                    .channels = {0},
                    .values = {40.0f, 50.0f},
                },
            },
            std::vector<iv::TickMaterializedEventInput>{})),
        1u);

    iv::TickInvocationWorkspace workspace{plan, 3};
    iv::TickInvocationFrame frame{
        page_reader, materialization_reader, workspace};
    auto const inputs = static_cast<
        std::span<iv::RandomAccessSampleInputPort const>>(
        frame.call().random_access_sample_inputs);
    ASSERT_EQ(inputs.size(), 1u);
    EXPECT_EQ(inputs[0].coverage(), (iv::Coverage{{{4, 6}}}));
    EXPECT_FLOAT_EQ(inputs[0].at(4).value, 40.0f);
    EXPECT_FLOAT_EQ(inputs[0].at(5).value, 50.0f);

    auto candidate = pages.begin_candidate(7, 4);
    ASSERT_EQ(
        pages.publish(std::move(candidate)),
        iv::PersistedPagePublishResult::published);
    auto newer_page_reader = pages.register_reader();
    auto newer_materialization_reader = materializations.register_reader();
    iv::TickInvocationWorkspace newer_workspace{plan, 3};
    iv::TickInvocationFrame incoherent{
        newer_page_reader,
        newer_materialization_reader,
        newer_workspace};
    auto const incoherent_inputs = static_cast<
        std::span<iv::RandomAccessSampleInputPort const>>(
        incoherent.call().random_access_sample_inputs);
    ASSERT_EQ(incoherent_inputs.size(), 1u);
    EXPECT_TRUE(incoherent_inputs[0].coverage().empty());
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
