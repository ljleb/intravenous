#include <intravenous/runtime/background_evaluation_transaction.h>
#include <intravenous/runtime/persisted_page_store.h>
#include <intravenous/runtime/realtime_produced_record.h>
#include <intravenous/runtime/realtime_persisted_state.h>
#include <intravenous/runtime/tick_invocation_frame.h>

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
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

std::vector<std::byte> queued_bytes(iv::PinnedBlockPrefix const& prefix)
{
    std::vector<std::byte> result;
    prefix.for_each([&](iv::AsyncQueueBlock const& block) {
        result.insert(
            result.end(),
            block.used_storage().begin(),
            block.used_storage().end());
    });
    return result;
}

void no_op_background(
    std::byte*, iv::graph_jit::BackgroundEvaluationCall*)
{}

iv::CompiledGraph queued_persisted_graph(iv::PortKind kind)
{
    using namespace iv::graph_jit;
    iv::CompiledGraph graph;
    graph.project_generation = 3;
    graph.specialization.sample_rate = 48000;
    graph.specialization.block_size = 4;
    auto& plan = graph.background_evaluation_plan;
    plan.nodes = {{
        .outputs = {0},
        .accumulators = {
            .output_change_begin = 0,
            .output_change_count = 1,
            .output_requirement_begin = 0,
            .output_requirement_count = 1,
        },
    }};
    plan.ports = {{
        .node = 0,
        .configured_port = {0, kind, 0},
        .kind = kind,
        .direction = PortDirection::output,
        .persisted_tick_output = true,
        .retention = iv::OutputRetention::persisted,
        .sample_layout = iv::mono_planar_channel_layout,
        .event_type = iv::EventTypeId::trigger,
        .accumulators = {
            .output_change = 0,
            .output_requirement = 0,
        },
    }};
    plan.storage.ports = {{
        .kind = kind,
        .storage = PortStorageKind::persisted_pages,
        .source_port = iv::NodeBundlePortId{0, kind, 0},
        .output_port = 0,
        .sample_layout = iv::mono_planar_channel_layout,
        .sample_channels = kind == iv::PortKind::sample
            ? std::vector<std::size_t>{0}
            : std::vector<std::size_t>{},
        .event_type = iv::EventTypeId::trigger,
    }};
    plan.runtime.node_operations.resize(1);
    plan.runtime.node_replay_invocations.resize(1);
    plan.runtime.port_bindings.resize(1);
    plan.accumulators = {
        .output_change_count = 1,
        .output_requirement_count = 1,
    };
    graph.background_operations = {
        .propagate_forward = &no_op_background,
        .propagate_reverse = &no_op_background,
        .evaluate = &no_op_background,
    };
    return graph;
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

TEST(TickInvocationWorkspace, TickInvocationFramePublishesQueuedSampleRecord)
{
    auto plan = direct_tick_sample_plan();
    iv::PersistedPageStore pages;
    auto page_reader = pages.register_reader();
    iv::TickMaterializationStore materializations;
    auto materialization_reader = materializations.register_reader();
    iv::TickInvocationWorkspace workspace{plan, 3, 4};
    iv::ProducerReserve reserve{iv::realtime_produced_block_storage_size};
    iv::AsyncCapacityManager manager{1};
    iv::AsyncWorkSignal work_signal;
    iv::PendingQueue pending{reserve, work_signal};
    std::atomic<bool> reservation_failed{false};
    ASSERT_EQ(
        manager.maintain(reserve, {
            .maximum_burst = 1,
            .low_watermark = 1,
            .high_watermark = 2,
        }),
        2u);
    workspace.bind_producer_endpoint(
        0, reserve, pending, reservation_failed);

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
        auto const& operation = frame.call().sample_captures.data()[0];
        ASSERT_NE(operation.context, nullptr);
        ASSERT_NE(operation.capture, nullptr);
        operation.capture(operation.context, &binding, 8, 4);
        EXPECT_TRUE(pending.pin().empty());
    }

    EXPECT_FALSE(reservation_failed.load());
    auto selected = pending.pin();
    ASSERT_FALSE(selected.empty());
    auto const bytes = queued_bytes(selected);
    ASSERT_GE(bytes.size(), sizeof(iv::RealtimeProducedRecordHeader));
    iv::RealtimeProducedRecordHeader header;
    std::memcpy(&header, bytes.data(), sizeof(header));
    EXPECT_EQ(header.payload_kind,
        iv::RealtimeProducedPayloadKind::samples);
    EXPECT_EQ(header.record_block_count, 1u);
    EXPECT_EQ(header.payload_size, 6u * sizeof(iv::Sample));
    EXPECT_EQ(header.begin, 7u);
    EXPECT_EQ(header.sample_count, 6u);
    EXPECT_EQ(header.sample_layout, iv::mono_planar_channel_layout);

    std::vector<iv::Sample> captured(header.sample_count);
    ASSERT_EQ(
        bytes.size(), sizeof(header) + std::as_bytes(std::span{captured}).size());
    std::memcpy(
        captured.data(), bytes.data() + sizeof(header), header.payload_size);
    EXPECT_EQ(captured,
        (std::vector<iv::Sample>{
            17.0f, 10.0f, 11.0f, 12.0f, 13.0f, 14.0f}));
}

TEST(TickInvocationWorkspace, QueuedSampleReservationFailureIsSticky)
{
    auto plan = direct_tick_sample_plan();
    iv::PersistedPageStore pages;
    auto page_reader = pages.register_reader();
    iv::TickMaterializationStore materializations;
    auto materialization_reader = materializations.register_reader();
    iv::TickInvocationWorkspace workspace{plan, 3, 4};
    iv::ProducerReserve empty_reserve{
        iv::realtime_produced_block_storage_size};
    iv::AsyncWorkSignal work_signal;
    iv::PendingQueue pending{empty_reserve, work_signal};
    std::atomic<bool> reservation_failed{false};
    workspace.bind_producer_endpoint(
        0, empty_reserve, pending, reservation_failed);

    std::array<iv::Sample, 8> source{};
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
        auto const& operation = frame.call().sample_captures.data()[0];
        operation.capture(operation.context, &binding, 8, 4);
    }

    EXPECT_TRUE(reservation_failed.load());
    EXPECT_TRUE(pending.pin().empty());
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

TEST(TickInvocationWorkspace, QueuedEventCapturePublishesAuthoritativeEmptyRecord)
{
    auto plan = direct_tick_event_plan();
    iv::PersistedPageStore pages;
    auto page_reader = pages.register_reader();
    iv::TickMaterializationStore materializations;
    auto materialization_reader = materializations.register_reader();
    iv::TickInvocationWorkspace workspace{plan, 3, 4};
    iv::ProducerReserve reserve{iv::realtime_produced_block_storage_size};
    iv::AsyncCapacityManager manager{1};
    iv::AsyncWorkSignal work_signal;
    iv::PendingQueue pending{reserve, work_signal};
    std::atomic<bool> reservation_failed{false};
    ASSERT_EQ(
        manager.maintain(reserve, {
            .maximum_burst = 1,
            .low_watermark = 1,
            .high_watermark = 2,
        }),
        2u);
    workspace.bind_producer_endpoint(
        0, reserve, pending, reservation_failed);

    struct EventStorage {
        std::size_t count = 0;
        std::array<iv::TimedEvent, 8> events{};
    } source;
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
        auto const& operation = frame.call().event_captures.data()[0];
        operation.capture(operation.context, &binding, 8, 4);
        EXPECT_TRUE(pending.pin().empty());
    }

    EXPECT_FALSE(reservation_failed.load());
    auto selected = pending.pin();
    ASSERT_FALSE(selected.empty());
    auto const bytes = queued_bytes(selected);
    ASSERT_EQ(bytes.size(), sizeof(iv::RealtimeProducedRecordHeader));
    iv::RealtimeProducedRecordHeader header;
    std::memcpy(&header, bytes.data(), sizeof(header));
    EXPECT_EQ(header.payload_kind,
        iv::RealtimeProducedPayloadKind::events);
    EXPECT_EQ(header.record_block_count, 1u);
    EXPECT_EQ(header.payload_size, 0u);
    EXPECT_EQ(header.begin, 7u);
    EXPECT_EQ(header.sample_count, 6u);
    EXPECT_EQ(header.event_type, iv::EventTypeId::trigger);
    EXPECT_EQ(header.event_count, 0u);
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

TEST(BackgroundEvaluationTransaction,
     AppliesQueuedSampleOverwriteBeforePublishingItsCandidate)
{
    auto graph = queued_persisted_graph(iv::PortKind::sample);
    iv::BackgroundCoverageState coverage{1};
    iv::BackgroundPropagationWorkspace propagation{
        graph.background_evaluation_plan, graph.specialization.sample_rate};
    iv::PersistedPageStore pages;
    auto reader = pages.register_reader();
    iv::TickMaterializationStore materializations;
    iv::ProducerReserve reserve{iv::realtime_produced_block_storage_size};
    iv::AsyncCapacityManager manager{1};
    iv::AsyncWorkSignal work_signal;
    iv::PendingQueue pending{reserve, work_signal};
    ASSERT_EQ(manager.maintain(reserve, {1, 1, 2}), 2u);

    std::array<iv::Sample, 4> values{10.0f, 11.0f, 12.0f, 13.0f};
    auto chain = reserve.acquire(1);
    ASSERT_TRUE(chain);
    auto writer = chain.writer();
    auto const header = iv::RealtimeProducedRecordHeader{
        .payload_kind = iv::RealtimeProducedPayloadKind::samples,
        .record_block_count = 1,
        .payload_size = sizeof(values),
        .begin = 2,
        .sample_count = values.size(),
        .sample_layout = iv::mono_planar_channel_layout,
    };
    ASSERT_TRUE(writer.append(std::as_bytes(std::span{&header, 1u})));
    ASSERT_TRUE(writer.append(std::as_bytes(std::span{values})));
    ASSERT_TRUE(pending.publish(std::move(chain)));

    std::array<iv::Sample, 2> overlap{30.0f, 31.0f};
    auto later_chain = reserve.acquire(1);
    ASSERT_TRUE(later_chain);
    auto later_writer = later_chain.writer();
    auto later_header = header;
    later_header.payload_size = sizeof(overlap);
    later_header.begin = 3;
    later_header.sample_count = overlap.size();
    ASSERT_TRUE(later_writer.append(
        std::as_bytes(std::span{&later_header, 1u})));
    ASSERT_TRUE(later_writer.append(std::as_bytes(std::span{overlap})));
    ASSERT_TRUE(pending.publish(std::move(later_chain)));

    auto selected = pending.pin();
    ASSERT_FALSE(selected.empty());
    std::array routes{iv::BackgroundProducedInputRoute{
        .output = local_output(iv::PortKind::sample, 0),
        .port = 0,
        .kind = iv::PortKind::sample,
    }};
    std::array selections{std::move(selected)};

    iv::BackgroundEvaluationTransaction transaction{
        graph,
        nullptr,
        coverage,
        propagation,
        pages,
        materializations,
        {},
        routes,
        selections,
    };
    auto result = transaction.execute();
    ASSERT_TRUE(result.has_value()) << result.error();
    ASSERT_EQ(result->status, iv::BackgroundEvaluationStatus::committed);
    ASSERT_TRUE(result->published_pages.has_value());

    auto published = reader.pin();
    auto const output = local_output(iv::PortKind::sample, 0);
    auto const* first = published->find_sample_page(output, 0);
    auto const* second = published->find_sample_page(output, 1);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(first->domain, (iv::Coverage{{{2, 4}}}));
    EXPECT_EQ(second->domain, (iv::Coverage{{{4, 6}}}));
    EXPECT_EQ(first->values, (std::vector<iv::Sample>{10.0f, 30.0f}));
    EXPECT_EQ(second->values, (std::vector<iv::Sample>{31.0f, 13.0f}));
}

TEST(BackgroundEvaluationTransaction,
     QueuedEmptyEventWindowAuthoritativelyErasesExistingEvents)
{
    auto graph = queued_persisted_graph(iv::PortKind::event);
    iv::BackgroundCoverageState coverage{1};
    iv::BackgroundPropagationWorkspace propagation{
        graph.background_evaluation_plan, graph.specialization.sample_rate};
    iv::PersistedPageStore pages;
    auto reader = pages.register_reader();
    auto const output = local_output(iv::PortKind::event, 0);
    auto seed = pages.begin_candidate(1, 4);
    auto page = empty_event_page(output);
    page.events = {{.time = 1, .value = iv::TriggerEvent{}}};
    seed.put(std::move(page));
    ASSERT_EQ(
        pages.publish(std::move(seed)),
        iv::PersistedPagePublishResult::published);

    iv::TickMaterializationStore materializations;
    iv::ProducerReserve reserve{iv::realtime_produced_block_storage_size};
    iv::AsyncCapacityManager manager{1};
    iv::AsyncWorkSignal work_signal;
    iv::PendingQueue pending{reserve, work_signal};
    ASSERT_EQ(manager.maintain(reserve, {1, 1, 2}), 2u);
    auto chain = reserve.acquire(1);
    ASSERT_TRUE(chain);
    auto writer = chain.writer();
    auto const header = iv::RealtimeProducedRecordHeader{
        .payload_kind = iv::RealtimeProducedPayloadKind::events,
        .record_block_count = 1,
        .payload_size = 0,
        .begin = 0,
        .sample_count = 4,
        .event_type = iv::EventTypeId::trigger,
        .event_count = 0,
    };
    ASSERT_TRUE(writer.append(std::as_bytes(std::span{&header, 1u})));
    ASSERT_TRUE(pending.publish(std::move(chain)));
    std::array selections{pending.pin()};
    ASSERT_FALSE(selections[0].empty());
    std::array routes{iv::BackgroundProducedInputRoute{
        .output = output,
        .port = 0,
        .kind = iv::PortKind::event,
    }};

    iv::BackgroundEvaluationTransaction transaction{
        graph,
        nullptr,
        coverage,
        propagation,
        pages,
        materializations,
        {.semantic_version = 2},
        routes,
        selections,
    };
    auto result = transaction.execute();
    ASSERT_TRUE(result.has_value()) << result.error();
    ASSERT_EQ(result->status, iv::BackgroundEvaluationStatus::committed);

    auto published = reader.pin();
    auto const* emptied = published->find_event_page(output, 0);
    ASSERT_NE(emptied, nullptr);
    EXPECT_EQ(emptied->domain, (iv::Coverage{{{0, 4}}}));
    EXPECT_TRUE(emptied->events.empty());
}

TEST(RealtimePersistedStateMailbox,
     AdoptsOnlyTheNewestCoherentPageAndMaterializationPair)
{
    iv::PersistedPageStore pages;
    iv::TickMaterializationStore materializations;
    iv::RealtimePersistedStateMailbox mailbox;

    auto initial_state = iv::RealtimePersistedState::prepare_initial(3, pages);
    ASSERT_TRUE(initial_state->capture_initial());
    mailbox.activate_generation(std::move(initial_state));
    auto const* initial = mailbox.adopt(3);
    ASSERT_NE(initial, nullptr);
    EXPECT_EQ(initial->pages().version(), iv::PersistedPageSnapshotVersion{});
    EXPECT_EQ(initial->materialization().generation(), 3u);

    auto prepare_state = [&] {
        return iv::RealtimePersistedState::prepare(
            3, pages, materializations);
    };
    auto publish_pair = [&](std::unique_ptr<iv::RealtimePersistedState> state,
                            std::uint64_t semantic,
                            float first) {
        auto candidate = pages.begin_candidate(semantic, 4);
        candidate.put(sample_page(
            local_output(iv::PortKind::sample, 0), first));
        ASSERT_EQ(
            pages.publish(std::move(candidate)),
            iv::PersistedPagePublishResult::published);
        auto const page_version = [&] {
            auto reader = pages.register_reader();
            auto pin = reader.pin();
            return pin->version();
        }();
        static_cast<void>(materializations.promote(
            std::make_unique<iv::TickMaterializationSnapshot>(
                3,
                semantic,
                page_version,
                std::vector<iv::TickMaterializedSampleInput>{},
                std::vector<iv::TickMaterializedEventInput>{})));
        auto captured = state->capture_current();
        ASSERT_TRUE(captured.has_value()) << captured.error();
        mailbox.publish(std::move(state));
    };

    auto second = prepare_state();
    publish_pair(std::move(second), 1, 10.0f);
    auto third = prepare_state();
    publish_pair(std::move(third), 2, 20.0f);

    auto const* adopted = mailbox.adopt(3);
    ASSERT_NE(adopted, nullptr);
    EXPECT_EQ(
        adopted->pages().version(),
        (iv::PersistedPageSnapshotVersion{.semantic = 2, .page = 2}));
    EXPECT_EQ(adopted->materialization().pages(), adopted->pages().version());
    auto const* page = adopted->pages().find_sample_page(
        local_output(iv::PortKind::sample, 0), 0);
    ASSERT_NE(page, nullptr);
    EXPECT_FLOAT_EQ(page->values.front().value, 20.0f);

    // The superseded-but-never-adopted second root and the replaced initial
    // active root are both destroyed only by explicit off-realtime reclamation.
    EXPECT_EQ(mailbox.reclaim_returned(), 2u);
    EXPECT_EQ(pages.reclaim_retired(), 2u);
    EXPECT_EQ(materializations.reclaim_retired(), 2u);
}

TEST(RealtimePersistedStateMailbox,
     InitialStatePinsPagesAtActivationRatherThanPreparation)
{
    iv::PersistedPageStore pages;
    auto initial_state = iv::RealtimePersistedState::prepare_initial(4, pages);

    auto const output = stable_output(iv::PortKind::sample, "late-page");
    auto candidate = pages.begin_candidate(7, 4);
    candidate.put(sample_page(output, 30.0f));
    ASSERT_EQ(
        pages.publish(std::move(candidate)),
        iv::PersistedPagePublishResult::published);

    ASSERT_TRUE(initial_state->capture_initial());
    EXPECT_FALSE(initial_state->capture_initial());
    EXPECT_EQ(
        initial_state->pages().version(),
        (iv::PersistedPageSnapshotVersion{.semantic = 7, .page = 1}));
    auto const* page = initial_state->pages().find_sample_page(output, 0);
    ASSERT_NE(page, nullptr);
    EXPECT_FLOAT_EQ(page->values.front().value, 30.0f);
    EXPECT_EQ(initial_state->materialization().generation(), 4u);
    EXPECT_EQ(
        initial_state->materialization().pages(),
        initial_state->pages().version());
}

TEST(RealtimePersistedStateMailbox,
     RejectsAStateForAnotherExecutionGeneration)
{
    iv::PersistedPageStore pages;
    iv::RealtimePersistedStateMailbox mailbox;
    auto initial_state = iv::RealtimePersistedState::prepare_initial(3, pages);
    ASSERT_TRUE(initial_state->capture_initial());
    mailbox.activate_generation(std::move(initial_state));
    auto const* active = mailbox.adopt(3);
    ASSERT_NE(active, nullptr);

    auto successor_state = iv::RealtimePersistedState::prepare_initial(4, pages);
    ASSERT_TRUE(successor_state->capture_initial());
    mailbox.publish(std::move(successor_state));
    EXPECT_EQ(mailbox.adopt(3), active);
    EXPECT_EQ(mailbox.reclaim_returned(), 1u);
}

TEST(RealtimePersistedStateMailbox,
     GenerationActivationCannotBeDisplacedByLateOldGenerationPublication)
{
    iv::PersistedPageStore pages;
    iv::RealtimePersistedStateMailbox mailbox;
    auto capture_initial = [&](std::uint64_t generation) {
        auto state = iv::RealtimePersistedState::prepare_initial(
            generation, pages);
        EXPECT_TRUE(state->capture_initial());
        return state;
    };

    mailbox.activate_generation(capture_initial(3));
    mailbox.publish(capture_initial(3));
    mailbox.activate_generation(capture_initial(4));

    // A background operation selected before cutover may finish afterward.
    // Its incompatible pending root is rejected without replacing generation
    // four's directly installed active root.
    mailbox.publish(capture_initial(3));
    auto const* active = mailbox.adopt(4);
    ASSERT_NE(active, nullptr);
    EXPECT_EQ(active->generation(), 4u);
    EXPECT_EQ(mailbox.reclaim_returned(), 3u);
}

} // namespace
