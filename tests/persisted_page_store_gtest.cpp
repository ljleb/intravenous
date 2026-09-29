#include <intravenous/runtime/persisted_page_store.h>
#include <intravenous/runtime/tick_capture_store.h>
#include <intravenous/runtime/tick_invocation_frame.h>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>
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

template<class T>
std::vector<T> capture_payload_as(iv::TickCaptureRecordView const& record)
{
    if (record.payload.size() % sizeof(T) != 0) return {};
    std::vector<T> result(record.payload.size() / sizeof(T));
    if (!record.payload.copy_to(
            std::as_writable_bytes(std::span{result}))) {
        return {};
    }
    return result;
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
            .output_history = 1,
            .output_latency = 1,
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
    plan.tick_runtime.sample_captures = {{
        .port = 0,
        .maximum_block_size = 4,
        .maximum_invocations_per_callback = 1,
    }};
    plan.tick_runtime.nodes = {
        TickNodeInvocationPlan{
            .sample_capture_begin = 0,
            .sample_capture_count = 1,
        },
        TickNodeInvocationPlan{
            .random_access_sample_begin = 0,
            .random_access_sample_count = 1,
        },
    };
    return plan;
}

iv::graph_jit::BackgroundEvaluationPlan sequential_tick_sample_plan()
{
    auto plan = direct_tick_sample_plan();
    auto& input = plan.ports[1];
    input.random_access_input = false;
    input.tick_sequential_input = true;
    input.sample_neutral_value = -0.25f;
    input.sequential_history = 2;

    auto& storage = plan.storage.ports[0];
    storage.storage = iv::graph_jit::PortStorageKind::tick_random_access;
    storage.output_port.reset();
    plan.storage.direct_samples[0].delivery =
        iv::graph_jit::PlannedDeliveryMechanism::tock_to_sequential;
    plan.tick_runtime.random_access_sample_inputs.clear();
    plan.tick_runtime.sequential_sample_inputs = {{
        .port = 1,
        .storage = {0},
    }};
    plan.tick_runtime.nodes[1] = {
        .sequential_sample_begin = 0,
        .sequential_sample_count = 1,
    };
    return plan;
}

iv::Sample sequential_sample_at(
    iv::ReflectedSampleInputPortBinding const& binding,
    iv::SampleIndex index,
    std::size_t channel = 0)
{
    auto const& selected = binding.storage.channels[channel];
    auto const frame = static_cast<std::size_t>(
        (index - selected.frame_delay) & (selected.frame_capacity - 1));
    auto const* values = reinterpret_cast<iv::Sample const*>(selected.storage);
    return values[frame * selected.frame_stride];
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
            .persisted_tick_output = true,
            .output_history = 1,
            .output_latency = 1,
            .retention = iv::OutputRetention::persisted,
            .event_type = iv::EventTypeId::trigger,
            .max_events_per_index = 1.0,
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
    plan.tick_runtime.event_captures = {{
        .port = 0,
        .maximum_block_size = 4,
        .maximum_invocations_per_callback = 1,
    }};
    plan.tick_runtime.nodes = {
        TickNodeInvocationPlan{
            .event_capture_begin = 0,
            .event_capture_count = 1,
        },
        TickNodeInvocationPlan{
            .random_access_event_begin = 0,
            .random_access_event_count = 1,
        },
    };
    return plan;
}

iv::graph_jit::BackgroundEvaluationPlan sequential_tick_event_plan()
{
    auto plan = direct_tick_event_plan();
    auto& input = plan.ports[1];
    input.random_access_input = false;
    input.tick_sequential_input = true;

    plan.event_target_subsets[0].access =
        iv::graph_jit::PlannedDestinationAccess::sequential;
    auto& storage = plan.storage.ports[0];
    storage.storage = iv::graph_jit::PortStorageKind::tick_sequential;
    storage.output_port.reset();
    storage.max_events_per_index = 1.0;
    plan.tick_runtime.random_access_event_inputs.clear();
    plan.tick_runtime.sequential_event_inputs = {{
        .port = 1,
        .storage = {0},
    }};
    plan.tick_runtime.nodes[1] = {
        .sequential_event_begin = 0,
        .sequential_event_count = 1,
    };
    return plan;
}

std::span<iv::TimedEvent const> sequential_events(
    iv::ReflectedEventInputPortBinding const& binding)
{
    auto const& storage = binding.storage;
    auto const count = *reinterpret_cast<std::size_t const*>(
        storage.storage + storage.count_offset);
    auto const* events = reinterpret_cast<iv::TimedEvent const*>(
        storage.storage + storage.events_offset);
    return {events, count};
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
        EXPECT_TRUE(frame.call().sample_captures.empty());
        EXPECT_TRUE(frame.call().event_captures.empty());

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
    ASSERT_EQ(workspace.sample_capture_count(), 1u);
    EXPECT_EQ(workspace.event_capture_count(), 0u);
    ASSERT_EQ(frame.call().sample_captures.size(), 1u);
    EXPECT_EQ(frame.call().sample_captures.data()[0].context, nullptr);
    EXPECT_EQ(frame.call().sample_captures.data()[0].capture, nullptr);
    auto const inputs = static_cast<
        std::span<iv::RandomAccessSampleInputPort const>>(
        frame.call().random_access_sample_inputs);
    ASSERT_EQ(inputs.size(), 1u);
    EXPECT_EQ(inputs[0].coverage(), (iv::Coverage{{{0, 4}}}));
    EXPECT_FLOAT_EQ(inputs[0].at(0).value, 10.0f);
    EXPECT_FLOAT_EQ(inputs[0].at(3).value, 13.0f);
}

TEST(TickCaptureStore, TickInvocationFrameBindsAndScopesSampleCapture)
{
    auto plan = direct_tick_sample_plan();
    iv::PersistedPageStore pages;
    auto page_reader = pages.register_reader();
    iv::TickMaterializationStore materializations;
    auto materialization_reader = materializations.register_reader();
    iv::TickCaptureStore captures{2 * sizeof(iv::Sample)};
    iv::PersistedTickCaptureRegistry capture_outputs{captures};
    iv::TickInvocationWorkspace workspace{plan, 3, 4, &capture_outputs};
    EXPECT_EQ(workspace.capture_block_reserve(), 3u);
    ASSERT_EQ(
        captures.ensure_free_block_reserve(workspace.capture_block_reserve()),
        workspace.capture_block_reserve());

    std::array<iv::Sample, 8> source{
        10.0f, 11.0f, 12.0f, 13.0f,
        14.0f, 15.0f, 16.0f, 17.0f};
    iv::ReflectedSampleOutputPortBinding binding{
        .storage = {
            .frame_capacity = source.size(),
            .storage_latency = 0,
            .channel_layout = iv::mono_planar_channel_layout,
        },
        .history = 1,
        .latency = 1,
    };
    binding.storage.channels[0] = {
        .storage = reinterpret_cast<std::byte*>(source.data()),
        .frame_capacity = source.size(),
        .frame_stride = 1,
        .frame_delay = 0,
    };

    {
        iv::TickInvocationFrame frame{
            page_reader, materialization_reader, workspace, 8, 4};
        ASSERT_EQ(frame.call().sample_captures.size(), 1u);
        auto const& operation = frame.call().sample_captures.data()[0];
        ASSERT_NE(operation.context, nullptr);
        ASSERT_NE(operation.capture, nullptr);
        operation.capture(operation.context, &binding, 8, 4);
        EXPECT_EQ(captures.published_sequence(), 1u);
        EXPECT_EQ(captures.reclaim_committed(), 0u);
    }

    auto batch = captures.snapshot_pending();
    ASSERT_EQ(batch.size(), 1u);
    std::vector<iv::Sample> captured;
    batch.for_each([&](iv::TickCaptureRecordView const& record) {
        auto const* output = capture_outputs.persisted_output(record.output);
        ASSERT_NE(output, nullptr);
        EXPECT_EQ(*output, local_output(iv::PortKind::sample, 0));
        EXPECT_EQ(record.payload_kind,
            iv::TickCapturePayloadKind::samples);
        EXPECT_EQ(record.sample_count, 6u);
        auto const values = capture_payload_as<iv::Sample>(record);
        captured.insert(captured.end(), values.begin(), values.end());
    });
    EXPECT_EQ(captured,
        (std::vector<iv::Sample>{
            17.0f, 10.0f, 11.0f, 12.0f, 13.0f, 14.0f}));
}

TEST(TickCaptureStore, CaptureReserveCountsEveryPlannedInvocation)
{
    auto plan = direct_tick_sample_plan();
    auto& capture = plan.tick_runtime.sample_captures.front();
    capture.maximum_block_size = 2;
    capture.maximum_invocations_per_callback = 2;

    iv::TickCaptureStore captures{2 * sizeof(iv::Sample)};
    iv::PersistedTickCaptureRegistry capture_outputs{captures};
    iv::TickInvocationWorkspace workspace{plan, 3, 4, &capture_outputs};

    // Each two-frame SCC slice captures history + slice + latency = four
    // samples, requiring two physical blocks. Two slices may be sealed before
    // the callback ends, so all four blocks must be available concurrently.
    EXPECT_EQ(workspace.capture_block_reserve(), 4u);
}

TEST(PersistedPageStore, TickFrameCopiesSequentialMaterializationAndUsesNeutral)
{
    auto plan = sequential_tick_sample_plan();
    iv::PersistedPageStore pages;
    auto page_reader = pages.register_reader();
    iv::TickMaterializationStore materializations;
    auto materialization_reader = materializations.register_reader();
    ASSERT_EQ(materializations.promote(
        std::make_unique<iv::TickMaterializationSnapshot>(
            3,
            1,
            iv::PersistedPageSnapshotVersion{},
            std::vector<iv::TickMaterializedSampleInput>{
                {
                    .port = 1,
                    .coverage = iv::Coverage{{{4, 7}}},
                    .layout = plan.ports[1].sample_layout,
                    .channels = {0},
                    .values = {40.0f, 50.0f, 60.0f},
                },
            },
            std::vector<iv::TickMaterializedEventInput>{})),
        1u);

    iv::TickInvocationWorkspace workspace{plan, 3, 4};
    EXPECT_EQ(workspace.sequential_sample_count(), 1u);
    iv::TickInvocationFrame frame{
        page_reader,
        materialization_reader,
        workspace,
        5,
        3};
    auto const inputs = static_cast<
        std::span<iv::ReflectedSampleInputPortBinding const>>(
        frame.call().sequential_sample_inputs);
    ASSERT_EQ(inputs.size(), 1u);
    EXPECT_EQ(inputs[0].history, 2u);
    EXPECT_EQ(inputs[0].storage.frame_capacity, 8u);
    EXPECT_FLOAT_EQ(sequential_sample_at(inputs[0], 3), -0.25f);
    EXPECT_FLOAT_EQ(sequential_sample_at(inputs[0], 4), 40.0f);
    EXPECT_FLOAT_EQ(sequential_sample_at(inputs[0], 5), 50.0f);
    EXPECT_FLOAT_EQ(sequential_sample_at(inputs[0], 6), 60.0f);
    EXPECT_FLOAT_EQ(sequential_sample_at(inputs[0], 7), -0.25f);
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

TEST(TickCaptureStore, TickInvocationFrameBindsAndScopesEventCapture)
{
    auto plan = direct_tick_event_plan();
    iv::PersistedPageStore pages;
    auto page_reader = pages.register_reader();
    iv::TickMaterializationStore materializations;
    auto materialization_reader = materializations.register_reader();
    iv::TickCaptureStore captures{sizeof(iv::TimedEvent)};
    iv::PersistedTickCaptureRegistry capture_outputs{captures};
    iv::TickInvocationWorkspace workspace{plan, 3, 4, &capture_outputs};
    EXPECT_EQ(workspace.capture_block_reserve(), 8u);
    ASSERT_EQ(
        captures.ensure_free_block_reserve(workspace.capture_block_reserve()),
        workspace.capture_block_reserve());

    struct EventStorage {
        std::size_t count = 0;
        std::array<iv::TimedEvent, 8> events{};
    } source{
        .count = 2,
        .events = {{
            {.time = 8, .value = iv::TriggerEvent{}},
            {.time = 10, .value = iv::TriggerEvent{}},
        }},
    };
    iv::ReflectedEventOutputPortBinding binding{
        .storage = {
            .storage = reinterpret_cast<std::byte*>(&source),
            .count_offset = offsetof(EventStorage, count),
            .events_offset = offsetof(EventStorage, events),
            .event_capacity = source.events.size(),
            .type = iv::EventTypeId::trigger,
        },
        .source_type = iv::EventTypeId::trigger,
        .history = 1,
        .latency = 1,
    };

    {
        iv::TickInvocationFrame frame{
            page_reader, materialization_reader, workspace, 8, 4};
        ASSERT_EQ(frame.call().event_captures.size(), 1u);
        auto const& operation = frame.call().event_captures.data()[0];
        ASSERT_NE(operation.context, nullptr);
        ASSERT_NE(operation.capture, nullptr);
        operation.capture(operation.context, &binding, 8, 4);
        EXPECT_EQ(captures.published_sequence(), 1u);
    }

    auto batch = captures.snapshot_pending();
    ASSERT_EQ(batch.size(), 1u);
    std::vector<iv::EventTime> captured;
    batch.for_each([&](iv::TickCaptureRecordView const& record) {
        auto const* output = capture_outputs.persisted_output(record.output);
        ASSERT_NE(output, nullptr);
        EXPECT_EQ(*output, local_output(iv::PortKind::event, 0));
        EXPECT_EQ(record.payload_kind,
            iv::TickCapturePayloadKind::events);
        ASSERT_EQ(record.event_count, 2u);
        for (auto const& event : capture_payload_as<iv::TimedEvent>(record)) {
            captured.push_back(event.time);
        }
    });
    EXPECT_EQ(captured, (std::vector<iv::EventTime>{8, 10}));
}

TEST(PersistedPageStore, TickFrameCopiesBoundedSequentialEvents)
{
    auto plan = sequential_tick_event_plan();
    iv::PersistedPageStore pages;
    auto page_reader = pages.register_reader();
    iv::TickMaterializationStore materializations;
    auto materialization_reader = materializations.register_reader();
    ASSERT_EQ(materializations.promote(
        std::make_unique<iv::TickMaterializationSnapshot>(
            3,
            1,
            iv::PersistedPageSnapshotVersion{},
            std::vector<iv::TickMaterializedSampleInput>{},
            std::vector<iv::TickMaterializedEventInput>{
                {
                    .port = 1,
                    .coverage = iv::Coverage{{{4, 9}}},
                    .type = iv::EventTypeId::trigger,
                    .events = {
                        {.time = 4, .value = iv::TriggerEvent{}},
                        {.time = 6, .value = iv::TriggerEvent{}},
                        {.time = 8, .value = iv::TriggerEvent{}},
                    },
                },
            })),
        1u);

    iv::TickInvocationWorkspace workspace{plan, 3, 4};
    EXPECT_EQ(workspace.sequential_event_count(), 1u);
    {
        iv::TickInvocationFrame frame{
            page_reader,
            materialization_reader,
            workspace,
            5,
            3};
        auto const inputs = static_cast<
            std::span<iv::ReflectedEventInputPortBinding const>>(
            frame.call().sequential_event_inputs);
        ASSERT_EQ(inputs.size(), 1u);
        EXPECT_EQ(inputs[0].storage.event_capacity, 4u);
        auto const events = sequential_events(inputs[0]);
        ASSERT_EQ(events.size(), 1u);
        EXPECT_EQ(events[0].time, 6u);
        EXPECT_TRUE(std::holds_alternative<iv::TriggerEvent>(events[0].value));
    }
    {
        iv::TickInvocationFrame frame{
            page_reader,
            materialization_reader,
            workspace,
            7,
            1};
        auto const inputs = static_cast<
            std::span<iv::ReflectedEventInputPortBinding const>>(
            frame.call().sequential_event_inputs);
        ASSERT_EQ(inputs.size(), 1u);
        EXPECT_TRUE(sequential_events(inputs[0]).empty());
    }
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

TEST(TickCaptureStore, FixesSequenceCutoffAndRecyclesAfterCallback)
{
    iv::TickCaptureStore captures{256};
    auto const samples = captures.register_output(iv::PortKind::sample);
    auto const events = captures.register_output(iv::PortKind::event);
    ASSERT_EQ(captures.ensure_free_block_reserve(3), 3u);
    EXPECT_EQ(captures.free_block_count(), 3u);

    iv::TickCaptureStore::Batch first_batch;
    {
        auto callback = captures.begin_callback();
        ASSERT_TRUE(callback);
        auto nested_callback = captures.begin_callback();
        EXPECT_FALSE(nested_callback);

        auto sample_writer = captures.reserve_record(2 * sizeof(iv::Sample));
        ASSERT_TRUE(sample_writer);
        std::array<iv::Sample, 2> sample_values{10.0f, 11.0f};
        ASSERT_TRUE(sample_writer.append(
            std::as_bytes(std::span{sample_values})));
        ASSERT_TRUE(sample_writer.seal_samples(
            samples,
            1200,
            2,
            iv::mono_planar_channel_layout));

        auto event_writer = captures.reserve_record(sizeof(iv::TimedEvent));
        ASSERT_TRUE(event_writer);
        std::array<iv::TimedEvent, 1> event_values{{{
            .time = 1201,
            .value = iv::TriggerEvent{},
        }}};
        ASSERT_TRUE(event_writer.append(
            std::as_bytes(std::span{event_values})));
        ASSERT_TRUE(event_writer.seal_events(
            events,
            1200,
            2,
            iv::EventTypeId::trigger,
            1));

        first_batch = captures.snapshot_pending();
        EXPECT_EQ(first_batch.begin(), 0u);
        EXPECT_EQ(first_batch.cutoff(), 2u);
        EXPECT_EQ(first_batch.size(), 2u);

        auto later = captures.reserve_record(sizeof(iv::Sample));
        ASSERT_TRUE(later);
        std::array<iv::Sample, 1> later_values{40.0f};
        ASSERT_TRUE(later.append(
            std::as_bytes(std::span{later_values})));
        ASSERT_TRUE(later.seal_samples(
            samples,
            400,
            1,
            iv::mono_planar_channel_layout));
        EXPECT_EQ(captures.published_sequence(), 3u);

        std::vector<iv::CaptureSequence> sequences;
        first_batch.for_each([&](iv::TickCaptureRecordView const& record) {
            sequences.push_back(record.sequence);
            ASSERT_TRUE(record.output);
            if (record.sequence == 0) {
                EXPECT_EQ(record.output, samples);
                EXPECT_EQ(record.begin, 1200u);
                EXPECT_EQ(record.sample_count, 2u);
                EXPECT_EQ(
                    record.payload_kind,
                    iv::TickCapturePayloadKind::samples);
                auto const values = capture_payload_as<iv::Sample>(record);
                ASSERT_EQ(values.size(), 2u);
                EXPECT_FLOAT_EQ(values[0], 10.0f);
                EXPECT_FLOAT_EQ(values[1], 11.0f);
            } else {
                EXPECT_EQ(record.output, events);
                EXPECT_EQ(record.event_count, 1u);
                auto const values = capture_payload_as<iv::TimedEvent>(record);
                ASSERT_EQ(values.size(), 1u);
                EXPECT_EQ(values[0].time, 1201u);
            }
        });
        EXPECT_EQ(sequences,
            (std::vector<iv::CaptureSequence>{0, 1}));

        ASSERT_TRUE(captures.commit(std::move(first_batch)));
        EXPECT_EQ(captures.processed_sequence(), 2u);
        EXPECT_EQ(captures.retired_block_count(), 1u);
        EXPECT_EQ(captures.reclaim_committed(), 0u);
    }

    EXPECT_EQ(captures.reclaim_committed(), 1u);
    EXPECT_EQ(captures.free_block_count(), 1u);

    auto second_batch = captures.snapshot_pending();
    EXPECT_EQ(second_batch.begin(), 2u);
    EXPECT_EQ(second_batch.cutoff(), 3u);
    ASSERT_TRUE(captures.commit(std::move(second_batch)));
    EXPECT_EQ(captures.reclaim_committed(), 1u);
    // The queue retains its current consumer sentinel until a later record is
    // committed, so two of the three blocks are immediately reusable here.
    EXPECT_EQ(captures.free_block_count(), 2u);
}

TEST(TickCaptureStore, MaintainsAFreeBlockTargetWithoutRepeatedGrowth)
{
    iv::TickCaptureStore captures{2 * sizeof(iv::Sample)};
    auto const samples = captures.register_output(iv::PortKind::sample);

    EXPECT_EQ(captures.ensure_free_block_reserve(3), 3u);
    EXPECT_EQ(captures.free_block_count(), 3u);
    EXPECT_EQ(captures.ensure_free_block_reserve(3), 0u);
    EXPECT_EQ(captures.ensure_free_block_reserve(2), 0u);
    EXPECT_EQ(captures.free_block_count(), 3u);

    {
        auto callback = captures.begin_callback();
        ASSERT_TRUE(callback);
        auto writer = captures.reserve_record(3 * sizeof(iv::Sample));
        ASSERT_TRUE(writer);
        std::array<iv::Sample, 3> values{1.0f, 2.0f, 3.0f};
        ASSERT_TRUE(writer.append(std::as_bytes(std::span{values})));
        ASSERT_TRUE(writer.seal_samples(
            samples,
            0,
            values.size(),
            iv::mono_planar_channel_layout));
    }

    // The pending two-block record is backlog, not part of the free reserve.
    EXPECT_EQ(captures.free_block_count(), 1u);
    EXPECT_EQ(captures.ensure_free_block_reserve(3), 2u);
    EXPECT_EQ(captures.free_block_count(), 3u);
    EXPECT_EQ(captures.ensure_free_block_reserve(3), 0u);
}

TEST(PersistedTickCaptureRegistry, InternsMappingsAboveGenericTransport)
{
    iv::TickCaptureStore captures{64};
    iv::PersistedTickCaptureRegistry registry{captures};
    auto const output = stable_output(iv::PortKind::sample, "recorded");

    auto const first = registry.register_output(output);
    auto const repeated = registry.register_output(output);
    EXPECT_EQ(first, repeated);
    EXPECT_EQ(first.kind(), iv::PortKind::sample);
    EXPECT_EQ(registry.size(), 1u);
    auto const* resolved = registry.persisted_output(first);
    ASSERT_NE(resolved, nullptr);
    EXPECT_EQ(*resolved, output);

    iv::TickCaptureStore other_captures{64};
    auto const foreign = other_captures.register_output(iv::PortKind::sample);
    EXPECT_EQ(registry.persisted_output(foreign), nullptr);
}

TEST(TickCaptureStore, PublishesOneLogicalRecordBackedByMultipleBlocks)
{
    // Deliberately split the second event across the physical-block boundary.
    iv::TickCaptureStore captures{sizeof(iv::TimedEvent) + 1};
    auto const events = captures.register_output(iv::PortKind::event);
    ASSERT_EQ(captures.ensure_free_block_reserve(2), 2u);

    {
        auto callback = captures.begin_callback();
        ASSERT_TRUE(callback);
        auto writer = captures.reserve_record(2 * sizeof(iv::TimedEvent));
        ASSERT_TRUE(writer);
        EXPECT_EQ(captures.free_block_count(), 0u);
        EXPECT_EQ(captures.published_sequence(), 0u);
        EXPECT_TRUE(captures.snapshot_pending().empty());

        std::array<iv::TimedEvent, 2> captured{{
            {.time = 4, .value = iv::TriggerEvent{}},
            {.time = 6, .value = iv::TriggerEvent{}},
        }};
        ASSERT_TRUE(writer.append(
            std::as_bytes(std::span{captured})));
        ASSERT_TRUE(writer.seal_events(
            events,
            4,
            4,
            iv::EventTypeId::trigger,
            2));

        EXPECT_EQ(captures.published_sequence(), 1u);
        auto complete = captures.snapshot_pending();
        ASSERT_EQ(complete.size(), 1u);
        std::size_t segment_count = 0;
        complete.for_each([&](iv::TickCaptureRecordView const& record) {
            EXPECT_EQ(record.sequence, 0u);
            EXPECT_EQ(record.event_count, 2u);
            record.payload.for_each_segment(
                [&](std::span<std::byte const>) { ++segment_count; });
            auto const values = capture_payload_as<iv::TimedEvent>(record);
            ASSERT_EQ(values.size(), 2u);
            EXPECT_EQ(values[0].time, 4u);
            EXPECT_EQ(values[1].time, 6u);
        });
        EXPECT_EQ(segment_count, 2u);

        ASSERT_TRUE(captures.commit(std::move(complete)));
        EXPECT_EQ(captures.retired_block_count(), 1u);
        EXPECT_EQ(captures.reclaim_committed(), 0u);
    }
    EXPECT_EQ(captures.reclaim_committed(), 1u);
    EXPECT_EQ(captures.free_block_count(), 1u);
}

TEST(TickCaptureStore, PublishesIntentionalEmptyEventRecord)
{
    iv::TickCaptureStore captures{64};
    auto const events = captures.register_output(iv::PortKind::event);
    ASSERT_EQ(captures.ensure_free_block_reserve(1), 1u);

    auto callback = captures.begin_callback();
    ASSERT_TRUE(callback);
    auto writer = captures.reserve_record(0);
    ASSERT_TRUE(writer);
    ASSERT_TRUE(writer.seal_events(
        events,
        4,
        4,
        iv::EventTypeId::trigger,
        0));

    EXPECT_EQ(captures.published_sequence(), 1u);
    auto batch = captures.snapshot_pending();
    ASSERT_EQ(batch.size(), 1u);
    batch.for_each([&](iv::TickCaptureRecordView const& record) {
        EXPECT_EQ(record.payload_kind,
            iv::TickCapturePayloadKind::events);
        EXPECT_EQ(record.event_count, 0u);
        EXPECT_TRUE(record.payload.empty());
    });
}

TEST(TickCaptureStore, FailedBatchAndAbandonedWriterPreserveState)
{
    iv::TickCaptureStore captures{64};
    auto const samples = captures.register_output(iv::PortKind::sample);
    ASSERT_EQ(captures.ensure_free_block_reserve(2), 2u);

    {
        auto callback = captures.begin_callback();
        ASSERT_TRUE(callback);
        {
            auto abandoned = captures.reserve_record(65);
            ASSERT_TRUE(abandoned);
            EXPECT_EQ(captures.free_block_count(), 0u);
        }
        EXPECT_EQ(captures.free_block_count(), 2u);
        EXPECT_EQ(captures.published_sequence(), 0u);
        EXPECT_TRUE(captures.snapshot_pending().empty());

        {
            auto incomplete = captures.reserve_record(2 * sizeof(iv::Sample));
            ASSERT_TRUE(incomplete);
            std::array<iv::Sample, 1> partial{2.0f};
            ASSERT_TRUE(incomplete.append(
                std::as_bytes(std::span{partial})));
            EXPECT_EQ(incomplete.written_size(), sizeof(iv::Sample));
            EXPECT_FALSE(incomplete.seal_samples(
                samples,
                8,
                2,
                iv::mono_planar_channel_layout));
        }
        EXPECT_EQ(captures.free_block_count(), 2u);
        EXPECT_EQ(captures.published_sequence(), 0u);

        auto unavailable = captures.reserve_record(129);
        EXPECT_FALSE(unavailable);
        EXPECT_EQ(captures.free_block_count(), 2u);
        EXPECT_EQ(captures.published_sequence(), 0u);

        auto writer = captures.reserve_record(sizeof(iv::Sample));
        ASSERT_TRUE(writer);
        std::array<iv::Sample, 1> values{3.0f};
        ASSERT_TRUE(writer.append(
            std::as_bytes(std::span{values})));
        ASSERT_TRUE(writer.seal_samples(
            samples,
            8,
            1,
            iv::mono_planar_channel_layout));
    }

    {
        auto failed_transaction = captures.snapshot_pending();
        EXPECT_EQ(failed_transaction.begin(), 0u);
        EXPECT_EQ(failed_transaction.cutoff(), 1u);
    }
    EXPECT_EQ(captures.processed_sequence(), 0u);
    auto retry = captures.snapshot_pending();
    EXPECT_EQ(retry.begin(), 0u);
    EXPECT_EQ(retry.cutoff(), 1u);
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
