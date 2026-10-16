#include <intravenous/runtime/background_evaluation_call_frame.h>
#include <intravenous/runtime/background_storage_realization.h>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

iv::GlobalIndex index(std::int64_t value)
{
    return iv::GlobalIndex::from_signed(value);
}

iv::GlobalIndexRange range(std::int64_t begin, std::int64_t end)
{
    return iv::GlobalIndexRange::from_begin_and_end(index(begin), index(end));
}

iv::IndexCoverage index_coverage(
    std::initializer_list<std::pair<std::int64_t, std::int64_t>> ranges)
{
    iv::IndexCoverage result;
    for (auto const [begin, end] : ranges) result.include(range(begin, end));
    return result;
}

iv::graph_jit::StableOutputPortId stable_output(
    iv::PortKind kind,
    char const* name)
{
    return {
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

TEST(BackgroundStorageRealization, OwnsAddressStableSparseTransactionStorage)
{
    iv::graph_jit::BackgroundEvaluationPlan plan;
    plan.ports = {
        {
            .kind = iv::PortKind::sample,
            .direction = iv::graph_jit::PortDirection::output,
            .authored_tock_output = true,
        },
        {
            .kind = iv::PortKind::event,
            .direction = iv::graph_jit::PortDirection::output,
            .authored_tock_output = true,
        },
    };
    plan.storage.ports = {
        {
            .kind = iv::PortKind::sample,
            .storage = iv::graph_jit::PortStorageKind::background,
            .output_port = 0,
            .sample_layout = {
                .channel_type = iv::ChannelTypeId::stereo,
                .sample_layout = iv::SampleStreamLayout::interleaved,
            },
            .sample_channels = {0, 1},
        },
        {
            .kind = iv::PortKind::event,
            .storage = iv::graph_jit::PortStorageKind::background,
            .output_port = 1,
            .event_type = iv::EventTypeId::trigger,
            .max_events_per_index = 1.0,
        },
    };
    plan.runtime.bindings = {
        {
            .port = 0,
            .kind = iv::PortKind::sample,
            .direction = iv::graph_jit::PortDirection::output,
            .storage = {0},
        },
        {
            .port = 1,
            .kind = iv::PortKind::event,
            .direction = iv::graph_jit::PortDirection::output,
            .storage = {1},
        },
    };

    iv::PersistedDataStore data;
    iv::BackgroundStorageRealization realization{
        plan,
        data,
        {
            .generation = 9,
            .storage_coverage = {
                index_coverage({{10, 12}, {20, 21}}),
                index_coverage({{10, 12}}),
            },
        }};

    auto const* sample_read = realization.sample_read(0);
    auto const* sample_write = realization.sample_write(0);
    auto const* event_read = realization.event_read(1);
    auto const* event_write = realization.event_write(1);
    ASSERT_NE(sample_read, nullptr);
    ASSERT_NE(sample_write, nullptr);
    ASSERT_NE(event_read, nullptr);
    ASSERT_NE(event_write, nullptr);

    EXPECT_TRUE(sample_write->write(index(10), 0, 1.0f));
    EXPECT_TRUE(sample_write->write(index(10), 1, 2.0f));
    EXPECT_TRUE(sample_write->write(index(20), 0, 3.0f));
    EXPECT_FALSE(sample_write->write(index(15), 0, 4.0f));
    EXPECT_FLOAT_EQ(sample_read->at(index(10), 0).value, 1.0f);
    EXPECT_FLOAT_EQ(sample_read->at(index(10), 1).value, 2.0f);
    EXPECT_FLOAT_EQ(sample_read->at(index(20), 0).value, 3.0f);

    EXPECT_TRUE(event_write->write({
        .time = index(10),
        .value = iv::TriggerEvent{},
    }));
    EXPECT_TRUE(event_write->write({
        .time = index(11),
        .value = iv::TriggerEvent{},
    }));
    EXPECT_FALSE(event_write->write({
        .time = index(11),
        .value = iv::TriggerEvent{},
    }));
    std::vector<iv::GlobalIndex> times;
    event_read->for_each(range(10, 12), [&](iv::TimedEvent const& event) {
        times.push_back(event.time);
    });
    EXPECT_EQ(times, (std::vector<iv::GlobalIndex>{index(10), index(11)}));

    EXPECT_TRUE(realization.seal().has_value());
    EXPECT_TRUE(realization.sealed());
    EXPECT_EQ(realization.sample_read(0), sample_read);
    EXPECT_EQ(realization.event_read(1), event_read);
}

TEST(
    BackgroundStorageRealization,
    FreezesTickVisibleStorageAndDefersPinnedSnapshotReclamation)
{
    using namespace iv;
    graph_jit::BackgroundEvaluationPlan plan;
    plan.nodes = {{.inputs = {0, 1, 2, 3}}};
    plan.ports = {
        {
            .node = 0,
            .configured_port = {0, PortKind::sample, 0},
            .kind = PortKind::sample,
            .direction = graph_jit::PortDirection::input,
            .random_access_input = true,
            .sample_layout = {
                .channel_type = ChannelTypeId::stereo,
                .sample_layout = SampleStreamLayout::interleaved,
            },
        },
        {
            .node = 0,
            .configured_port = {0, PortKind::event, 0},
            .kind = PortKind::event,
            .direction = graph_jit::PortDirection::input,
            .random_access_input = true,
            .event_type = EventTypeId::trigger,
        },
        {
            .node = 0,
            .configured_port = {0, PortKind::sample, 1},
            .kind = PortKind::sample,
            .direction = graph_jit::PortDirection::input,
            .tick_sequential_input = true,
            .sample_layout = {
                .channel_type = ChannelTypeId::mono,
                .sample_layout = SampleStreamLayout::planar,
            },
        },
        {
            .node = 0,
            .configured_port = {0, PortKind::event, 1},
            .kind = PortKind::event,
            .direction = graph_jit::PortDirection::input,
            .tick_sequential_input = true,
            .event_type = EventTypeId::trigger,
        },
    };
    plan.sample_target_subsets = {
        {
            .port = {0, PortKind::sample, 0},
            .target_layout = {
                .channel_type = ChannelTypeId::stereo,
                .sample_layout = SampleStreamLayout::interleaved,
            },
            .access = graph_jit::PlannedDestinationAccess::random_access,
            .channels = {{0, 0, 0}, {0, 0, 1}},
        },
        {
            .port = {0, PortKind::sample, 1},
            .target_layout = {
                .channel_type = ChannelTypeId::mono,
                .sample_layout = SampleStreamLayout::planar,
            },
            .access = graph_jit::PlannedDestinationAccess::sequential,
            .channels = {{0, 1, 0}},
        },
    };
    plan.event_target_subsets = {
        {
            .port = {0, 0},
            .type = EventTypeId::trigger,
            .access = graph_jit::PlannedDestinationAccess::random_access,
        },
        {
            .port = {0, 1},
            .type = EventTypeId::trigger,
            .access = graph_jit::PlannedDestinationAccess::sequential,
        },
    };
    plan.storage.ports = {
        {
            .kind = PortKind::sample,
            .storage = graph_jit::PortStorageKind::tick_random_access,
            .target_subsets = {0},
            .sample_layout = {
                .channel_type = ChannelTypeId::stereo,
                .sample_layout = SampleStreamLayout::interleaved,
            },
            .sample_channels = {0, 1},
        },
        {
            .kind = PortKind::event,
            .storage = graph_jit::PortStorageKind::tick_random_access,
            .target_subsets = {0},
            .event_type = EventTypeId::trigger,
            .max_events_per_index = 1.0,
        },
        {
            .kind = PortKind::sample,
            .storage = graph_jit::PortStorageKind::tick_sequential,
            .target_subsets = {1},
            .sample_layout = {
                .channel_type = ChannelTypeId::mono,
                .sample_layout = SampleStreamLayout::planar,
            },
            .sample_channels = {0},
        },
        {
            .kind = PortKind::event,
            .storage = graph_jit::PortStorageKind::tick_sequential,
            .target_subsets = {1},
            .event_type = EventTypeId::trigger,
            .max_events_per_index = 1.0,
        },
    };
    plan.tick_runtime.sequential_sample_inputs = {{
        .port = 2,
        .storage = {2},
    }};
    plan.tick_runtime.sequential_event_inputs = {{
        .port = 3,
        .storage = {3},
    }};
    plan.tick_runtime.random_access_sample_inputs = {{
        .port = 0,
        .storage = {0},
    }};
    plan.tick_runtime.random_access_event_inputs = {{
        .port = 1,
        .storage = {1},
    }};
    plan.tick_runtime.nodes = {{
        .sequential_sample_begin = 0,
        .sequential_sample_count = 1,
        .sequential_event_begin = 0,
        .sequential_event_count = 1,
        .random_access_sample_begin = 0,
        .random_access_sample_count = 1,
        .random_access_event_begin = 0,
        .random_access_event_count = 1,
    }};

    iv::PersistedDataStore data;
    BackgroundStorageRealization realization{
        plan,
        data,
        {
            .generation = 4,
            .storage_coverage = {
                index_coverage({{10, 12}}),
                index_coverage({{10, 12}}),
                index_coverage({{10, 12}}),
                index_coverage({{10, 12}}),
            },
            .produce_storage = {true, true, true, true},
        }};
    auto const* sample = realization.sample_write(0);
    auto const* event = realization.event_write(1);
    auto const* sequential_sample = realization.sample_write(2);
    auto const* sequential_event = realization.event_write(3);
    ASSERT_NE(sample, nullptr);
    ASSERT_NE(event, nullptr);
    ASSERT_NE(sequential_sample, nullptr);
    ASSERT_NE(sequential_event, nullptr);
    EXPECT_TRUE(sample->write(index(10), 0, 1.0f));
    EXPECT_TRUE(sample->write(index(11), 0, 2.0f));
    EXPECT_TRUE(sample->write(index(10), 1, 10.0f));
    EXPECT_TRUE(sample->write(index(11), 1, 20.0f));
    EXPECT_TRUE(event->write({.time = index(11), .value = TriggerEvent{}}));
    EXPECT_TRUE(sequential_sample->write(index(10), 0, 30.0f));
    EXPECT_TRUE(sequential_sample->write(index(11), 0, 40.0f));
    EXPECT_TRUE(
        sequential_event->write({.time = index(10), .value = TriggerEvent{}}));
    ASSERT_TRUE(realization.seal().has_value());

    auto frozen = realization.make_tick_materialization_snapshot(
        4, 9, PersistedDataVersion{.semantic = 9, .revision = 2});
    ASSERT_TRUE(frozen.has_value())
        << (frozen ? std::string{} : frozen.error());
    ASSERT_NE(*frozen, nullptr);
    EXPECT_EQ((*frozen)->generation(), 4u);
    EXPECT_EQ((*frozen)->semantic_version(), 9u);
    EXPECT_EQ((*frozen)->data_version(),
        (PersistedDataVersion{.semantic = 9, .revision = 2}));
    auto const* frozen_sample = (*frozen)->find_sample(0);
    auto const* frozen_event = (*frozen)->find_event(1);
    auto const* frozen_sequential_sample = (*frozen)->find_sample(2);
    auto const* frozen_sequential_event = (*frozen)->find_event(3);
    ASSERT_NE(frozen_sample, nullptr);
    ASSERT_NE(frozen_event, nullptr);
    ASSERT_NE(frozen_sequential_sample, nullptr);
    ASSERT_NE(frozen_sequential_event, nullptr);
    EXPECT_FLOAT_EQ(frozen_sample->at(index(10), 0).value, 1.0f);
    EXPECT_FLOAT_EQ(frozen_sample->at(index(11), 1).value, 20.0f);
    EXPECT_FLOAT_EQ(frozen_sequential_sample->at(index(10), 0).value, 30.0f);
    EXPECT_FLOAT_EQ(frozen_sequential_sample->at(index(11), 0).value, 40.0f);
    std::vector<GlobalIndex> times;
    frozen_event->for_each(
        range(10, 12),
        &times,
        +[](void* opaque, TimedEvent const& value) {
            static_cast<std::vector<GlobalIndex>*>(opaque)->push_back(value.time);
        });
    EXPECT_EQ(times, (std::vector<GlobalIndex>{index(11)}));
    times.clear();
    frozen_sequential_event->for_each(
        range(10, 12),
        &times,
        +[](void* opaque, TimedEvent const& value) {
            static_cast<std::vector<GlobalIndex>*>(opaque)->push_back(value.time);
        });
    EXPECT_EQ(times, (std::vector<GlobalIndex>{index(10)}));

    TickMaterializationStore store;
    auto reader = store.register_reader();
    EXPECT_EQ(store.promote(std::move(*frozen)), 1u);
    {
        auto pin = reader.pin();
        EXPECT_EQ(pin->generation(), 4u);
        EXPECT_EQ(store.promote(std::make_unique<TickMaterializationSnapshot>(
            4,
            10,
            PersistedDataVersion{.semantic = 9, .revision = 2},
            std::vector<TickMaterializedSampleInput>{},
            std::vector<TickMaterializedEventInput>{})), 2u);
        EXPECT_EQ(store.retired_snapshot_count(), 2u);
        EXPECT_EQ(store.reclaim_retired(), 1u);
        EXPECT_EQ(pin->semantic_version(), 9u);
    }
    EXPECT_EQ(store.reclaim_retired(), 1u);
}

TEST(BackgroundStorageRealization, ReadsTypedPublishedDataWithoutCopyingIt)
{
    iv::PersistedDataStore store;
    auto reader = store.register_reader();
    auto const sample_id = stable_output(iv::PortKind::sample, "samples");
    auto const event_id = stable_output(iv::PortKind::event, "events");
    auto const sample_output = store.resolve_output(sample_id);
    auto const event_output = store.resolve_output(event_id);
    auto candidate = store.begin_candidate(6);
    candidate.overwrite(iv::PersistedSampleRegion{
        .output = sample_output,
        .range = range(0, 4),
        .layout = {
            .channel_type = iv::ChannelTypeId::stereo,
            .sample_layout = iv::SampleStreamLayout::planar,
        },
        .values = {1, 2, 3, 4, 10, 20, 30, 40},
    });
    candidate.overwrite(iv::PersistedEventChunk{
        .output = event_output,
        .range = range(4, 8),
        .type = iv::EventTypeId::trigger,
        .events = {
            {.time = index(5), .value = iv::TriggerEvent{}},
            {.time = index(7), .value = iv::TriggerEvent{}},
        },
    });
    ASSERT_EQ(
        store.publish(std::move(candidate)),
        iv::PersistedDataPublishResult::published);
    auto pin = reader.pin();

    iv::graph_jit::BackgroundEvaluationPlan plan;
    plan.ports = {
        {
            .kind = iv::PortKind::sample,
            .direction = iv::graph_jit::PortDirection::output,
            .persisted_tick_output = true,
            .retention = iv::OutputRetention::persisted,
            .stable_identity = sample_id,
        },
        {
            .kind = iv::PortKind::event,
            .direction = iv::graph_jit::PortDirection::output,
            .persisted_tick_output = true,
            .retention = iv::OutputRetention::persisted,
            .stable_identity = event_id,
        },
    };
    plan.storage.ports = {
        {
            .kind = iv::PortKind::sample,
            .storage = iv::graph_jit::PortStorageKind::persisted_data,
            .output_port = 0,
            .sample_layout = {
                .channel_type = iv::ChannelTypeId::stereo,
                .sample_layout = iv::SampleStreamLayout::planar,
            },
            .sample_channels = {0, 1},
        },
        {
            .kind = iv::PortKind::event,
            .storage = iv::graph_jit::PortStorageKind::persisted_data,
            .output_port = 1,
            .event_type = iv::EventTypeId::trigger,
        },
    };
    plan.runtime.bindings = {
        {
            .kind = iv::PortKind::sample,
            .direction = iv::graph_jit::PortDirection::input,
            .storage = {0},
        },
        {
            .kind = iv::PortKind::event,
            .direction = iv::graph_jit::PortDirection::input,
            .storage = {1},
        },
    };

    iv::BackgroundStorageRealization realization{
        plan,
        store,
        {
            .generation = 20,
            .storage_coverage = {
                index_coverage({{0, 4}}),
                index_coverage({{4, 8}}),
            },
            .published = &pin.snapshot(),
        }};

    auto const* samples = realization.sample_read(0);
    auto const* events = realization.event_read(1);
    ASSERT_NE(samples, nullptr);
    ASSERT_NE(events, nullptr);
    EXPECT_EQ(realization.sample_write(0), nullptr);
    EXPECT_EQ(realization.event_write(1), nullptr);
    EXPECT_FLOAT_EQ(samples->at(index(2), 0).value, 3.0f);
    EXPECT_FLOAT_EQ(samples->at(index(2), 1).value, 30.0f);

    std::vector<iv::GlobalIndex> times;
    events->for_each(range(4, 8), [&](iv::TimedEvent const& event) {
        times.push_back(event.time);
    });
    EXPECT_EQ(times, (std::vector<iv::GlobalIndex>{index(5), index(7)}));
    ASSERT_NE(realization.persisted_output_handle(0), nullptr);
    EXPECT_EQ(*realization.persisted_output_handle(0), sample_output);
    EXPECT_TRUE(realization.seal().has_value());

    iv::BackgroundStorageRealization missing_data{
        plan,
        store,
        {
            .generation = 20,
            .storage_coverage = {
                index_coverage({{0, 5}}),
                iv::IndexCoverage{},
            },
            .published = &pin.snapshot(),
        }};
    auto missing_result = missing_data.seal();
    ASSERT_FALSE(missing_result.has_value());
    EXPECT_NE(
        missing_result.error().find("missing selected published coverage"),
        std::string::npos);
}

TEST(BackgroundStorageRealization, GivesProducedPersistedStoragePrivateOwners)
{
    auto const output = stable_output(iv::PortKind::sample, "samples");
    iv::graph_jit::BackgroundEvaluationPlan plan;
    plan.ports = {{
        .kind = iv::PortKind::sample,
        .direction = iv::graph_jit::PortDirection::output,
        .authored_tock_output = true,
        .retention = iv::OutputRetention::persisted,
        .stable_identity = output,
    }};
    plan.storage.ports = {{
        .kind = iv::PortKind::sample,
        .storage = iv::graph_jit::PortStorageKind::persisted_data,
        .output_port = 0,
        .sample_layout = {
            .channel_type = iv::ChannelTypeId::mono,
            .sample_layout = iv::SampleStreamLayout::planar,
        },
        .sample_channels = {0},
    }};
    plan.runtime.bindings = {{
        .kind = iv::PortKind::sample,
        .direction = iv::graph_jit::PortDirection::output,
        .storage = {0},
    }};

    iv::PersistedDataStore store;
    iv::BackgroundStorageRealization realization{
        plan,
        store,
        {
            .generation = 4,
            .storage_coverage = {index_coverage({{100, 102}})},
        }};

    ASSERT_NE(realization.sample_read(0), nullptr);
    ASSERT_NE(realization.sample_write(0), nullptr);
    EXPECT_TRUE(realization.sample_write(0)->write(index(100), 0, 8.0f));
    EXPECT_TRUE(realization.sample_write(0)->write(index(101), 0, 9.0f));
    EXPECT_FLOAT_EQ(realization.sample_read(0)->at(index(100), 0).value, 8.0f);
    ASSERT_NE(realization.persisted_output_handle(0), nullptr);
    auto const output_handle = store.resolve_output(output);
    EXPECT_EQ(*realization.persisted_output_handle(0), output_handle);
    EXPECT_TRUE(realization.seal().has_value());
    auto candidate = store.begin_candidate(12);
    ASSERT_TRUE(realization.stage_persisted_data(candidate).has_value());
    ASSERT_EQ(store.publish(std::move(candidate)),
              iv::PersistedDataPublishResult::published);
    auto reader = store.register_reader();
    auto pin = reader.pin();
    auto const* region = pin->find_sample_region(output_handle, index(100));
    ASSERT_NE(region, nullptr);
    EXPECT_EQ(region->range, range(100, 102));
    ASSERT_EQ(region->values.size(), 2);
    EXPECT_FLOAT_EQ(region->values[0].value, 8.0f);
    EXPECT_FLOAT_EQ(region->values[1].value, 9.0f);
}

TEST(BackgroundStorageRealization, SealingRejectsMissingExternalDirectViews)
{
    iv::graph_jit::BackgroundEvaluationPlan plan;
    plan.storage.ports = {{
        .kind = iv::PortKind::sample,
        .storage = iv::graph_jit::PortStorageKind::current_tick,
        .sample_layout = {
            .channel_type = iv::ChannelTypeId::mono,
            .sample_layout = iv::SampleStreamLayout::planar,
        },
        .sample_channels = {0},
    }};
    plan.storage.direct_samples = {{.storage = 0}};
    plan.runtime.operations = {{
        .kind = iv::graph_jit::BackgroundRuntimeOperationKind::direct_sample,
        .operation = 0,
    }};
    plan.runtime.node_operations = {{.before = {0}}};

    iv::PersistedDataStore data;
    iv::BackgroundStorageRealization realization{
        plan,
        data,
        {
            .storage_coverage = {index_coverage({{0, 1}})},
        }};
    iv::BackgroundStorageOperationFrame operation_frame{
        realization,
        plan.runtime.node_operations[0].before,
        plan.runtime.node_operations[0].after,
    };
    EXPECT_FALSE(realization.seal().has_value());
    EXPECT_FALSE(realization.execute_operation(0).has_value());

    struct External {
        iv::IndexCoverage coverage = index_coverage({{0, 1}});
        std::array<std::size_t, 1> channels{0};
        iv::Sample value{5.0f};
    } external;
    realization.bind_sample(0, {
        .data = &external,
        .coverage_value = &external.coverage,
        .layout = {
            .channel_type = iv::ChannelTypeId::mono,
            .sample_layout = iv::SampleStreamLayout::planar,
        },
        .channels = external.channels,
        .read_sample = +[](void const* opaque,
                           iv::GlobalIndex,
                           std::size_t) noexcept {
            return static_cast<External const*>(opaque)->value;
        },
    });

    EXPECT_TRUE(realization.seal().has_value());
    EXPECT_EQ(realization.operation_count(), 1);
    EXPECT_FALSE(realization.operation_executed(0));
    EXPECT_NO_THROW(
        iv::BackgroundStorageOperationFrame::prepare_callback(&operation_frame));
    EXPECT_TRUE(realization.operation_executed(0));
    EXPECT_FALSE(realization.execute_operation(0).has_value());
    EXPECT_NO_THROW(
        iv::BackgroundStorageOperationFrame::finalize_callback(&operation_frame));
    EXPECT_FLOAT_EQ(realization.sample_read(0)->at(index(0), 0).value, 5.0f);
    EXPECT_THROW(
        realization.bind_sample(0, *realization.sample_read(0)),
        std::logic_error);
}

TEST(BackgroundStorageRealization, ExecutesHeterogeneousSampleProjectionPlan)
{
    iv::graph_jit::BackgroundEvaluationPlan plan;
    plan.ports = {
        {
            .kind = iv::PortKind::sample,
            .direction = iv::graph_jit::PortDirection::output,
            .authored_tock_output = true,
        },
        {
            .kind = iv::PortKind::sample,
            .direction = iv::graph_jit::PortDirection::output,
            .authored_tock_output = true,
        },
    };
    plan.storage.ports = {
        {
            .kind = iv::PortKind::sample,
            .storage = iv::graph_jit::PortStorageKind::background,
            .output_port = 0,
            .sample_layout = {
                .channel_type = iv::ChannelTypeId::stereo,
                .sample_layout = iv::SampleStreamLayout::planar,
            },
            .sample_channels = {0, 1},
        },
        {
            .kind = iv::PortKind::sample,
            .storage = iv::graph_jit::PortStorageKind::background,
            .output_port = 1,
            .sample_layout = {
                .channel_type = iv::ChannelTypeId::mono,
                .sample_layout = iv::SampleStreamLayout::planar,
            },
            .sample_channels = {0},
        },
        {
            .kind = iv::PortKind::sample,
            .storage = iv::graph_jit::PortStorageKind::background,
            .sample_layout = {
                .channel_type = iv::ChannelTypeId::stereo,
                .sample_layout = iv::SampleStreamLayout::interleaved,
            },
            .sample_channels = {0, 1},
            .sample_materialization = 0,
        },
    };
    plan.storage.sample_materializations = {{
        .storage = iv::graph_jit::PortStorageKind::background,
        .inputs = {0, 0, 1},
        .source_channels = {
            {.channel = 0},
            {.channel = 1},
            {.channel = 0},
        },
        .source_read_latencies = {1, 1, 0},
        .source_type = iv::ChannelTypeId::stereo,
        .target_layout = {
            .channel_type = iv::ChannelTypeId::stereo,
            .sample_layout = iv::SampleStreamLayout::interleaved,
        },
        .target_channels = {0, 1},
        .projections = {
            {
                .source_type = iv::ChannelTypeId::stereo,
                .source_channel_indices = {0, 1},
                .target_type = iv::ChannelTypeId::mono,
                .target_channels = {0},
            },
            {
                .source_type = iv::ChannelTypeId::mono,
                .source_channel_indices = {2},
                .target_type = iv::ChannelTypeId::mono,
                .target_channels = {1},
            },
        },
        .output = 2,
    }};
    plan.runtime.operations = {{
        .kind = iv::graph_jit::BackgroundRuntimeOperationKind::
            sample_materialization,
        .operation = 0,
    }};

    iv::PersistedDataStore data;
    iv::BackgroundStorageRealization realization{
        plan,
        data,
        {
            .storage_coverage = {
                index_coverage({{9, 11}}),
                index_coverage({{10, 12}}),
                index_coverage({{10, 12}}),
            },
        }};
    auto const* stereo = realization.sample_write(0);
    auto const* mono = realization.sample_write(1);
    ASSERT_NE(stereo, nullptr);
    ASSERT_NE(mono, nullptr);
    ASSERT_TRUE(stereo->write(index(9), 0, 2.0f));
    ASSERT_TRUE(stereo->write(index(9), 1, 6.0f));
    ASSERT_TRUE(stereo->write(index(10), 0, 4.0f));
    ASSERT_TRUE(stereo->write(index(10), 1, 8.0f));
    ASSERT_TRUE(mono->write(index(10), 0, 9.0f));
    ASSERT_TRUE(mono->write(index(11), 0, 10.0f));

    ASSERT_TRUE(realization.seal().has_value());
    ASSERT_TRUE(realization.execute_operation(0).has_value());
    auto const* output = realization.sample_read(2);
    ASSERT_NE(output, nullptr);
    EXPECT_FLOAT_EQ(output->at(index(10), 0).value, 4.0f);
    EXPECT_FLOAT_EQ(output->at(index(10), 1).value, 9.0f);
    EXPECT_FLOAT_EQ(output->at(index(11), 0).value, 6.0f);
    EXPECT_FLOAT_EQ(output->at(index(11), 1).value, 10.0f);
}

TEST(BackgroundStorageRealization, ConvertsAndStablyMergesEventSources)
{
    iv::graph_jit::BackgroundEvaluationPlan plan;
    plan.ports = {
        {
            .kind = iv::PortKind::event,
            .direction = iv::graph_jit::PortDirection::output,
            .authored_tock_output = true,
        },
        {
            .kind = iv::PortKind::event,
            .direction = iv::graph_jit::PortDirection::output,
            .authored_tock_output = true,
        },
    };
    plan.storage.ports = {
        {
            .kind = iv::PortKind::event,
            .storage = iv::graph_jit::PortStorageKind::background,
            .output_port = 0,
            .event_type = iv::EventTypeId::midi,
            .max_events_per_index = 1.0,
        },
        {
            .kind = iv::PortKind::event,
            .storage = iv::graph_jit::PortStorageKind::background,
            .output_port = 1,
            .event_type = iv::EventTypeId::midi,
            .max_events_per_index = 1.0,
        },
        {
            .kind = iv::PortKind::event,
            .storage = iv::graph_jit::PortStorageKind::background,
            .event_type = iv::EventTypeId::boundary,
            .max_events_per_index = 2.0,
            .event_materialization = 0,
        },
    };
    auto const conversion = iv::EventConversionRegistry::instance().plan(
        iv::EventTypeId::midi, iv::EventTypeId::boundary);
    plan.storage.event_materializations = {{
        .storage = iv::graph_jit::PortStorageKind::background,
        .inputs = {0, 1},
        .source_type = iv::EventTypeId::midi,
        .target_type = iv::EventTypeId::boundary,
        .conversion = conversion,
        .output = 2,
    }};
    plan.storage.direct_events = {{.storage = 0}};
    plan.runtime.operations = {
        {
            .kind = iv::graph_jit::BackgroundRuntimeOperationKind::direct_event,
            .operation = 0,
        },
        {
            .kind = iv::graph_jit::BackgroundRuntimeOperationKind::
                event_materialization,
            .operation = 0,
        },
    };

    iv::PersistedDataStore data;
    iv::BackgroundStorageRealization realization{
        plan,
        data,
        {
            .storage_coverage = {
                index_coverage({{0, 4}}),
                index_coverage({{0, 4}}),
                index_coverage({{0, 4}}),
            },
        }};
    ASSERT_TRUE(realization.event_write(0)->write({
        .time = index(1),
        .value = iv::MidiEvent{
            .bytes = {0x90, 60, 100},
            .size = 3,
        },
    }));
    ASSERT_TRUE(realization.event_write(1)->write({
        .time = index(1),
        .value = iv::MidiEvent{
            .bytes = {0x80, 60, 0},
            .size = 3,
        },
    }));

    ASSERT_TRUE(realization.seal().has_value());
    ASSERT_TRUE(realization.execute_operation(0).has_value());
    ASSERT_TRUE(realization.execute_operation(1).has_value());
    std::vector<bool> boundaries;
    realization.event_read(2)->for_each(
        range(0, 4),
        [&](iv::TimedEvent const& event) {
            boundaries.push_back(std::get<iv::BoundaryEvent>(event.value)
                .is_begin);
        });
    EXPECT_EQ(boundaries, (std::vector<bool>{true, false}));
}

TEST(BackgroundStorageRealization, ResolvesAnonymousGenerationLocalOutputHandle)
{
    iv::graph_jit::BackgroundEvaluationPlan plan;
    plan.ports = {{
        .kind = iv::PortKind::event,
        .direction = iv::graph_jit::PortDirection::output,
        .persisted_tick_output = true,
        .retention = iv::OutputRetention::persisted,
    }};
    plan.storage.ports = {{
        .kind = iv::PortKind::event,
        .storage = iv::graph_jit::PortStorageKind::persisted_data,
        .output_port = 0,
        .event_type = iv::EventTypeId::empty,
    }};

    iv::PersistedDataStore data;
    iv::BackgroundStorageRealization realization{
        plan,
        data,
        {
            .generation = 77,
            .storage_coverage = {iv::IndexCoverage{}},
        }};
    auto const* output = realization.persisted_output_handle(0);
    ASSERT_NE(output, nullptr);
    EXPECT_EQ(
        *output,
        data.resolve_output(iv::GenerationLocalPersistedOutputId{
            .generation = 77,
            .port = 0,
            .kind = iv::PortKind::event,
        }));
}

TEST(BackgroundEvaluationCallFrame, OwnsOperationFramesAndTockBindings)
{
    iv::graph_jit::BackgroundEvaluationPlan plan;
    plan.nodes = {{
        .authored_tock_execution = true,
        .inputs = {0},
        .outputs = {1, 2},
    }};
    plan.ports = {
        {
            .node = 0,
            .configured_port =
                {
                    .node_bundle_handle = 0,
                    .port_kind = iv::PortKind::sample,
                    .port_index = 0,
                },
            .kind = iv::PortKind::sample,
            .direction = iv::graph_jit::PortDirection::input,
            .random_access_input = true,
            .sample_layout =
                {
                    .channel_type = iv::ChannelTypeId::mono,
                    .sample_layout = iv::SampleStreamLayout::planar,
                },
        },
        {
            .node = 0,
            .configured_port =
                {
                    .node_bundle_handle = 0,
                    .port_kind = iv::PortKind::sample,
                    .port_index = 1,
                },
            .kind = iv::PortKind::sample,
            .direction = iv::graph_jit::PortDirection::output,
            .authored_tock_output = true,
            .sample_layout =
                {
                    .channel_type = iv::ChannelTypeId::mono,
                    .sample_layout = iv::SampleStreamLayout::planar,
                },
        },
        {
            .node = 0,
            .configured_port =
                {
                    .node_bundle_handle = 0,
                    .port_kind = iv::PortKind::event,
                    .port_index = 2,
                },
            .kind = iv::PortKind::event,
            .direction = iv::graph_jit::PortDirection::output,
            .authored_tock_output = true,
            .event_type = iv::EventTypeId::trigger,
        },
    };
    plan.sample_target_subsets = {{
        .port =
            {
                .node_bundle_handle = 0,
                .port_kind = iv::PortKind::sample,
                .port_index = 0,
            },
        .target_layout =
            {
                .channel_type = iv::ChannelTypeId::mono,
                .sample_layout = iv::SampleStreamLayout::planar,
            },
        .access = iv::graph_jit::PlannedDestinationAccess::random_access,
        .channels = {{.bundle = 0, .port = 0, .channel = 0}},
    }};
    plan.storage.ports = {
        {
            .kind = iv::PortKind::sample,
            .storage = iv::graph_jit::PortStorageKind::background,
            .output_port = 1,
            .sample_layout =
                {
                    .channel_type = iv::ChannelTypeId::mono,
                    .sample_layout = iv::SampleStreamLayout::planar,
                },
            .sample_channels = {0},
        },
        {
            .kind = iv::PortKind::sample,
            .storage = iv::graph_jit::PortStorageKind::background,
            .output_port = 1,
            .sample_layout =
                {
                    .channel_type = iv::ChannelTypeId::mono,
                    .sample_layout = iv::SampleStreamLayout::planar,
                },
            .sample_channels = {0},
        },
        {
            .kind = iv::PortKind::event,
            .storage = iv::graph_jit::PortStorageKind::background,
            .output_port = 2,
            .event_type = iv::EventTypeId::trigger,
            .max_events_per_index = 1.0,
        },
    };
    plan.storage.direct_samples = {{
        .target_subset = 0,
        .target_channel = 0,
        .source_channel = 0,
        .storage = 0,
        .delivery =
            iv::graph_jit::PlannedDeliveryMechanism::tock_to_random_access,
        .read_latency = 1,
    }};
    plan.runtime.bindings = {
        {
            .node = 0,
            .port = 0,
            .kind = iv::PortKind::sample,
            .direction = iv::graph_jit::PortDirection::input,
            .storage = {0},
        },
        {
            .node = 0,
            .port = 1,
            .kind = iv::PortKind::sample,
            .direction = iv::graph_jit::PortDirection::output,
            .storage = {1},
        },
        {
            .node = 0,
            .port = 2,
            .kind = iv::PortKind::event,
            .direction = iv::graph_jit::PortDirection::output,
            .storage = {2},
        },
    };
    plan.runtime.port_bindings = {0, 1, 2};
    plan.runtime.node_operations.resize(1);
    plan.runtime.node_replay_invocations.resize(1);

    iv::PersistedDataStore data;
    iv::BackgroundStorageRealization realization{
        plan,
        data,
        {
            .storage_coverage =
                {
                    index_coverage({{9, 11}}),
                    index_coverage({{10, 12}}),
                    index_coverage({{10, 12}}),
                },
        }};
    ASSERT_TRUE(realization.sample_write(0)->write(index(9), 0, 3.0f));
    ASSERT_TRUE(realization.sample_write(0)->write(index(10), 0, 4.0f));
    ASSERT_TRUE(realization.seal().has_value());

    iv::BackgroundEvaluationCallFrame frame{
        plan,
        realization,
        {
            .binding_coverage =
                {
                    index_coverage({{10, 12}}),
                    index_coverage({{10, 12}}),
                    index_coverage({{10, 12}}),
                },
            .sample_rate = 96000,
        }};
    ASSERT_TRUE(frame.seal().has_value());
    ASSERT_EQ(frame.call().nodes.size(), 1);
    auto& node = frame.nodes().front();
    EXPECT_NE(node.operation_frame, nullptr);
    EXPECT_NE(node.prepare_operations, nullptr);
    EXPECT_NE(node.finalize_operations, nullptr);
    ASSERT_EQ(node.tock.inputs.size(), 1);
    ASSERT_EQ(node.tock.outputs.size(), 1);
    ASSERT_EQ(node.tock.event_outputs.size(), 1);
    EXPECT_EQ(node.tock.sample_rate, 96000);
    EXPECT_FLOAT_EQ(node.tock.inputs.data()[0].at(index(10)).value, 3.0f);
    node.tock.outputs.data()[0].write(index(11), 0, iv::Sample{8.0f});
    EXPECT_FLOAT_EQ(realization.sample_read(1)->at(index(11), 0).value, 8.0f);
    node.tock.event_outputs.data()[0].write({
        .time = index(10),
        .value = iv::TriggerEvent{},
    });
    std::vector<iv::GlobalIndex> event_times;
    realization.event_read(2)->for_each(range(10, 12),
                                        [&](iv::TimedEvent const& event) {
                                            event_times.push_back(event.time);
                                        });
    EXPECT_EQ(event_times, (std::vector<iv::GlobalIndex>{index(10)}));
}

TEST(BackgroundEvaluationCallFrame, ReservesAndValidatesReplaySchedule)
{
    iv::graph_jit::BackgroundEvaluationPlan plan;
    plan.nodes = {{
        .replays_tick = true,
        .replay_maximum_block_size = 4,
        .inputs = {0, 1},
        .outputs = {2, 3},
    }};
    plan.ports = {
        {
            .node = 0,
            .configured_port = {0, iv::PortKind::sample, 0},
            .kind = iv::PortKind::sample,
            .direction = iv::graph_jit::PortDirection::input,
            .replay_sequential_input = true,
            .sample_layout =
                {
                    .channel_type = iv::ChannelTypeId::mono,
                    .sample_layout = iv::SampleStreamLayout::planar,
                },
        },
        {
            .node = 0,
            .configured_port = {0, iv::PortKind::event, 0},
            .kind = iv::PortKind::event,
            .direction = iv::graph_jit::PortDirection::input,
            .replay_sequential_input = true,
        },
        {
            .node = 0,
            .kind = iv::PortKind::sample,
            .direction = iv::graph_jit::PortDirection::output,
            .replayed_tick_output = true,
            .sample_layout =
                {
                    .channel_type = iv::ChannelTypeId::mono,
                    .sample_layout = iv::SampleStreamLayout::planar,
                },
        },
        {
            .node = 0,
            .kind = iv::PortKind::event,
            .direction = iv::graph_jit::PortDirection::output,
            .replayed_tick_output = true,
        },
    };
    plan.sample_target_subsets = {{
        .port = {0, iv::PortKind::sample, 0},
    }};
    plan.event_target_subsets = {{
        .port = {.bundle = 0, .port = 0},
    }};
    plan.storage.ports = {
        {
            .kind = iv::PortKind::sample,
            .storage = iv::graph_jit::PortStorageKind::background,
            .output_port = 2,
            .sample_layout =
                {
                    .channel_type = iv::ChannelTypeId::mono,
                    .sample_layout = iv::SampleStreamLayout::planar,
                },
            .sample_channels = {0},
        },
        {
            .kind = iv::PortKind::event,
            .storage = iv::graph_jit::PortStorageKind::background,
            .output_port = 3,
        },
        {
            .kind = iv::PortKind::sample,
            .storage = iv::graph_jit::PortStorageKind::background,
            .output_port = 2,
            .sample_layout =
                {
                    .channel_type = iv::ChannelTypeId::mono,
                    .sample_layout = iv::SampleStreamLayout::planar,
                },
            .sample_channels = {0},
        },
        {
            .kind = iv::PortKind::event,
            .storage = iv::graph_jit::PortStorageKind::background,
            .output_port = 3,
        },
    };
    plan.storage.direct_samples = {{
        .target_subset = 0,
        .target_channel = 0,
        .source_channel = 0,
        .storage = 0,
    }};
    plan.storage.direct_events = {{
        .target_subset = 0,
        .storage = 1,
    }};
    plan.runtime.bindings = {
        {.node = 0,
         .port = 0,
         .kind = iv::PortKind::sample,
         .direction = iv::graph_jit::PortDirection::input,
         .storage = {0}},
        {.node = 0,
         .port = 1,
         .kind = iv::PortKind::event,
         .direction = iv::graph_jit::PortDirection::input,
         .storage = {1}},
        {.node = 0,
         .port = 2,
         .kind = iv::PortKind::sample,
         .direction = iv::graph_jit::PortDirection::output,
         .storage = {2}},
        {.node = 0,
         .port = 3,
         .kind = iv::PortKind::event,
         .direction = iv::graph_jit::PortDirection::output,
         .storage = {3}},
    };
    plan.runtime.port_bindings = {0, 1, 2, 3};
    plan.runtime.node_operations.resize(1);
    plan.runtime.replay_invocations = {{
        .node = 0,
        .maximum_block_size = 4,
        .input_bindings = {0, 1},
        .output_bindings = {2, 3},
    }};
    plan.runtime.node_replay_invocations = {0};

    iv::PersistedDataStore data;
    iv::BackgroundStorageRealization realization{
        plan,
        data,
        {.storage_coverage = std::vector<iv::IndexCoverage>(
             4, index_coverage({{0, 10}}))}};
    ASSERT_TRUE(realization.sample_write(0)->write(index(0), 0, 3.0f));
    ASSERT_TRUE(realization.seal().has_value());
    iv::BackgroundEvaluationCallFrame frame{
        plan,
        realization,
        {.binding_coverage = std::vector<iv::IndexCoverage>(
             4, index_coverage({{0, 10}}))}};
    EXPECT_EQ(frame.replay_sample_inputs(0).size(), 1);
    EXPECT_EQ(frame.replay_event_inputs(0).size(), 1);
    EXPECT_EQ(frame.replay_sample_outputs(0).size(), 1);
    EXPECT_EQ(frame.replay_event_outputs(0).size(), 1);
    EXPECT_FALSE(frame.set_replay_regions(0, {range(0, 5)}).has_value());
    ASSERT_TRUE(frame.set_replay_regions(
        0, {range(0, 4), range(8, 10)}).has_value());
    ASSERT_TRUE(frame.seal().has_value());
    auto const& replay = frame.nodes().front().replay;
    EXPECT_EQ(replay.sample_input_bindings.size(), 1);
    EXPECT_EQ(replay.event_input_bindings.size(), 1);
    EXPECT_EQ(replay.sample_output_bindings.size(), 1);
    EXPECT_EQ(replay.event_output_bindings.size(), 1);
    ASSERT_EQ(frame.nodes().front().replay_regions.size(), 2);
    EXPECT_EQ(frame.nodes().front().replay_regions.data()[1],
              range(8, 10));

    auto& node = frame.nodes().front();
    ASSERT_NE(node.prepare_operations, nullptr);
    ASSERT_NE(node.finalize_operations, nullptr);
    node.prepare_operations(node.operation_frame);
    auto const& sample_input = node.replay.sample_input_bindings.data()[0];
    auto* input_values = reinterpret_cast<iv::Sample*>(
        sample_input.storage.channels[0].storage);
    EXPECT_FLOAT_EQ(input_values[0].value, 3.0f);

    auto const& sample_output = node.replay.sample_output_bindings.data()[0];
    auto* output_values = reinterpret_cast<iv::Sample*>(
        sample_output.storage.channels[0].storage);
    output_values[8 & (sample_output.storage.frame_capacity - 1)] =
        iv::Sample{9.0f};
    node.finalize_operations(node.operation_frame);
    EXPECT_FLOAT_EQ(realization.sample_read(2)->at(index(8), 0).value, 9.0f);
}

TEST(
    BackgroundEvaluationCallFrame,
    ReplayFlushesOnlyTheCoverageSelectedForEachOutput)
{
    iv::graph_jit::BackgroundEvaluationPlan plan;
    plan.nodes = {{
        .replays_tick = true,
        .replay_maximum_block_size = 4,
        .outputs = {0, 1},
    }};
    plan.ports = {
        {
            .node = 0,
            .kind = iv::PortKind::sample,
            .direction = iv::graph_jit::PortDirection::output,
            .replayed_tick_output = true,
            .sample_layout = {
                .channel_type = iv::ChannelTypeId::mono,
                .sample_layout = iv::SampleStreamLayout::planar,
            },
        },
        {
            .node = 0,
            .kind = iv::PortKind::sample,
            .direction = iv::graph_jit::PortDirection::output,
            .replayed_tick_output = true,
            .sample_layout = {
                .channel_type = iv::ChannelTypeId::mono,
                .sample_layout = iv::SampleStreamLayout::planar,
            },
        },
    };
    plan.storage.ports = {
        {
            .kind = iv::PortKind::sample,
            .storage = iv::graph_jit::PortStorageKind::background,
            .output_port = 0,
            .sample_layout = {
                .channel_type = iv::ChannelTypeId::mono,
                .sample_layout = iv::SampleStreamLayout::planar,
            },
            .sample_channels = {0},
        },
        {
            .kind = iv::PortKind::sample,
            .storage = iv::graph_jit::PortStorageKind::background,
            .output_port = 1,
            .sample_layout = {
                .channel_type = iv::ChannelTypeId::mono,
                .sample_layout = iv::SampleStreamLayout::planar,
            },
            .sample_channels = {0},
        },
    };
    plan.runtime.bindings = {
        {.node = 0,
         .port = 0,
         .kind = iv::PortKind::sample,
         .direction = iv::graph_jit::PortDirection::output,
         .storage = {0}},
        {.node = 0,
         .port = 1,
         .kind = iv::PortKind::sample,
         .direction = iv::graph_jit::PortDirection::output,
         .storage = {1}},
    };
    plan.runtime.port_bindings = {0, 1};
    plan.runtime.node_operations.resize(1);
    plan.runtime.replay_invocations = {{
        .node = 0,
        .maximum_block_size = 4,
        .output_bindings = {0, 1},
    }};
    plan.runtime.node_replay_invocations = {0};

    iv::PersistedDataStore data;
    iv::BackgroundStorageRealization realization{
        plan,
        data,
        {
            .storage_coverage = {
                index_coverage({{0, 4}}),
                index_coverage({{4, 8}}),
            },
        }};
    ASSERT_TRUE(realization.seal().has_value());
    iv::BackgroundEvaluationCallFrame frame{
        plan,
        realization,
        {
            .binding_coverage = {
                index_coverage({{0, 4}}),
                index_coverage({{4, 8}}),
            },
        }};
    ASSERT_TRUE(frame.set_replay_regions(
        0, {range(0, 4), range(4, 8)}).has_value());
    ASSERT_TRUE(frame.seal().has_value());

    auto& node = frame.nodes().front();
    node.prepare_operations(node.operation_frame);
    auto outputs = node.replay.sample_output_bindings;
    ASSERT_EQ(outputs.size(), 2u);
    for (std::size_t output = 0; output < outputs.size(); ++output) {
        auto* values = reinterpret_cast<iv::Sample*>(
            outputs.data()[output].storage.channels[0].storage);
        for (std::size_t index = 0; index < 8; ++index) {
            values[index & (outputs.data()[output].storage.frame_capacity - 1)] =
                iv::Sample{static_cast<float>(10 * output + index)};
        }
    }
    node.finalize_operations(node.operation_frame);
    ASSERT_TRUE(frame.validate_evaluation().has_value());
    EXPECT_FLOAT_EQ(realization.sample_read(0)->at(index(3), 0).value, 3.0f);
    EXPECT_FLOAT_EQ(realization.sample_read(1)->at(index(4), 0).value, 14.0f);
    EXPECT_FLOAT_EQ(realization.sample_read(1)->at(index(7), 0).value, 17.0f);
}

} // namespace
