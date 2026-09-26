#include <intravenous/runtime/background_storage_realization.h>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

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

    iv::BackgroundStorageRealization realization{
        plan,
        {
            .generation = 9,
            .storage_coverage = {
                iv::Coverage{{{10, 12}, {20, 21}}},
                iv::Coverage{{{10, 12}}},
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

    EXPECT_TRUE(sample_write->write(10, 0, 1.0f));
    EXPECT_TRUE(sample_write->write(10, 1, 2.0f));
    EXPECT_TRUE(sample_write->write(20, 0, 3.0f));
    EXPECT_FALSE(sample_write->write(15, 0, 4.0f));
    EXPECT_FLOAT_EQ(sample_read->at(10, 0).value, 1.0f);
    EXPECT_FLOAT_EQ(sample_read->at(10, 1).value, 2.0f);
    EXPECT_FLOAT_EQ(sample_read->at(20, 0).value, 3.0f);

    EXPECT_TRUE(event_write->write({
        .time = 10,
        .value = iv::TriggerEvent{},
    }));
    EXPECT_TRUE(event_write->write({
        .time = 11,
        .value = iv::TriggerEvent{},
    }));
    EXPECT_FALSE(event_write->write({
        .time = 11,
        .value = iv::TriggerEvent{},
    }));
    std::vector<iv::EventTime> times;
    event_read->for_each({10, 12}, [&](iv::TimedEvent const& event) {
        times.push_back(event.time);
    });
    EXPECT_EQ(times, (std::vector<iv::EventTime>{10, 11}));

    EXPECT_TRUE(realization.seal().has_value());
    EXPECT_TRUE(realization.sealed());
    EXPECT_EQ(realization.sample_read(0), sample_read);
    EXPECT_EQ(realization.event_read(1), event_read);
}

TEST(BackgroundStorageRealization, ReadsTypedPublishedPagesWithoutCopyingThem)
{
    iv::PersistedPageStore store;
    auto reader = store.register_reader();
    auto const sample_id = stable_output(iv::PortKind::sample, "samples");
    auto const event_id = stable_output(iv::PortKind::event, "events");
    auto candidate = store.begin_candidate(6, 4);
    candidate.put(iv::PersistedSamplePage{
        .output = sample_id,
        .page_index = 0,
        .domain = iv::Coverage{{{0, 4}}},
        .layout = {
            .channel_type = iv::ChannelTypeId::stereo,
            .sample_layout = iv::SampleStreamLayout::planar,
        },
        .packing = iv::PersistedSamplePacking::dense,
        .values = {1, 2, 3, 4, 10, 20, 30, 40},
    });
    candidate.put(iv::PersistedEventPage{
        .output = event_id,
        .page_index = 1,
        .domain = iv::Coverage{{{4, 8}}},
        .type = iv::EventTypeId::trigger,
        .events = {
            {.time = 1, .value = iv::TriggerEvent{}},
            {.time = 3, .value = iv::TriggerEvent{}},
        },
    });
    ASSERT_EQ(
        store.publish(std::move(candidate)),
        iv::PersistedPagePublishResult::published);
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
            .storage = iv::graph_jit::PortStorageKind::persisted_pages,
            .output_port = 0,
            .sample_layout = {
                .channel_type = iv::ChannelTypeId::stereo,
                .sample_layout = iv::SampleStreamLayout::planar,
            },
            .sample_channels = {0, 1},
        },
        {
            .kind = iv::PortKind::event,
            .storage = iv::graph_jit::PortStorageKind::persisted_pages,
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
        {
            .generation = 20,
            .storage_coverage = {
                iv::Coverage{{{0, 4}}},
                iv::Coverage{{{4, 8}}},
            },
            .published = &pin.snapshot(),
        }};

    auto const* samples = realization.sample_read(0);
    auto const* events = realization.event_read(1);
    ASSERT_NE(samples, nullptr);
    ASSERT_NE(events, nullptr);
    EXPECT_EQ(realization.sample_write(0), nullptr);
    EXPECT_EQ(realization.event_write(1), nullptr);
    EXPECT_FLOAT_EQ(samples->at(2, 0).value, 3.0f);
    EXPECT_FLOAT_EQ(samples->at(2, 1).value, 30.0f);

    std::vector<iv::EventTime> times;
    events->for_each({4, 8}, [&](iv::TimedEvent const& event) {
        times.push_back(event.time);
    });
    EXPECT_EQ(times, (std::vector<iv::EventTime>{5, 7}));
    ASSERT_NE(realization.persisted_output(0), nullptr);
    EXPECT_EQ(
        std::get<iv::graph_jit::StableOutputPortId>(
            *realization.persisted_output(0)),
        sample_id);
    EXPECT_TRUE(realization.seal().has_value());
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
        .storage = iv::graph_jit::PortStorageKind::persisted_pages,
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

    iv::BackgroundStorageRealization realization{
        plan,
        {
            .generation = 4,
            .storage_coverage = {iv::Coverage{{{100, 102}}}},
        }};

    ASSERT_NE(realization.sample_read(0), nullptr);
    ASSERT_NE(realization.sample_write(0), nullptr);
    EXPECT_TRUE(realization.sample_write(0)->write(100, 0, 8.0f));
    EXPECT_FLOAT_EQ(realization.sample_read(0)->at(100, 0).value, 8.0f);
    ASSERT_NE(realization.persisted_output(0), nullptr);
    EXPECT_EQ(
        std::get<iv::graph_jit::StableOutputPortId>(
            *realization.persisted_output(0)),
        output);
    EXPECT_TRUE(realization.seal().has_value());
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

    iv::BackgroundStorageRealization realization{
        plan,
        {
            .storage_coverage = {iv::Coverage{{{0, 1}}}},
        }};
    iv::BackgroundStorageOperationFrame operation_frame{
        realization,
        plan.runtime.node_operations[0].before,
        plan.runtime.node_operations[0].after,
    };
    EXPECT_FALSE(realization.seal().has_value());
    EXPECT_FALSE(realization.execute_operation(0).has_value());

    struct External {
        iv::Coverage coverage{{{0, 1}}};
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
                           iv::SampleIndex,
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
    EXPECT_FLOAT_EQ(realization.sample_read(0)->at(0, 0).value, 5.0f);
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

    iv::BackgroundStorageRealization realization{
        plan,
        {
            .storage_coverage = {
                iv::Coverage{{{9, 11}}},
                iv::Coverage{{{10, 12}}},
                iv::Coverage{{{10, 12}}},
            },
        }};
    auto const* stereo = realization.sample_write(0);
    auto const* mono = realization.sample_write(1);
    ASSERT_NE(stereo, nullptr);
    ASSERT_NE(mono, nullptr);
    ASSERT_TRUE(stereo->write(9, 0, 2.0f));
    ASSERT_TRUE(stereo->write(9, 1, 6.0f));
    ASSERT_TRUE(stereo->write(10, 0, 4.0f));
    ASSERT_TRUE(stereo->write(10, 1, 8.0f));
    ASSERT_TRUE(mono->write(10, 0, 9.0f));
    ASSERT_TRUE(mono->write(11, 0, 10.0f));

    ASSERT_TRUE(realization.seal().has_value());
    ASSERT_TRUE(realization.execute_operation(0).has_value());
    auto const* output = realization.sample_read(2);
    ASSERT_NE(output, nullptr);
    EXPECT_FLOAT_EQ(output->at(10, 0).value, 4.0f);
    EXPECT_FLOAT_EQ(output->at(10, 1).value, 9.0f);
    EXPECT_FLOAT_EQ(output->at(11, 0).value, 6.0f);
    EXPECT_FLOAT_EQ(output->at(11, 1).value, 10.0f);
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

    iv::BackgroundStorageRealization realization{
        plan,
        {
            .storage_coverage = {
                iv::Coverage{{{0, 4}}},
                iv::Coverage{{{0, 4}}},
                iv::Coverage{{{0, 4}}},
            },
        }};
    ASSERT_TRUE(realization.event_write(0)->write({
        .time = 1,
        .value = iv::MidiEvent{
            .bytes = {0x90, 60, 100},
            .size = 3,
        },
    }));
    ASSERT_TRUE(realization.event_write(1)->write({
        .time = 1,
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
        {0, 4},
        [&](iv::TimedEvent const& event) {
            boundaries.push_back(std::get<iv::BoundaryEvent>(event.value)
                .is_begin);
        });
    EXPECT_EQ(boundaries, (std::vector<bool>{true, false}));
}

TEST(BackgroundStorageRealization, UsesGenerationLocalIdentityForAnonymousOutput)
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
        .storage = iv::graph_jit::PortStorageKind::persisted_pages,
        .output_port = 0,
        .event_type = iv::EventTypeId::empty,
    }};

    iv::BackgroundStorageRealization realization{
        plan,
        {
            .generation = 77,
            .storage_coverage = {iv::Coverage{}},
        }};
    auto const* identity = realization.persisted_output(0);
    ASSERT_NE(identity, nullptr);
    EXPECT_EQ(
        std::get<iv::GenerationLocalPersistedOutputId>(*identity),
        (iv::GenerationLocalPersistedOutputId{
            .generation = 77,
            .port = 0,
            .kind = iv::PortKind::event,
        }));
}

} // namespace
