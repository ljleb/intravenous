#include <intravenous/runtime/background_evaluation_transaction.h>
#include <intravenous/runtime/persisted_data_store.h>
#include <intravenous/runtime/realtime_produced_record.h>
#include <intravenous/runtime/realtime_persisted_state.h>
#include <intravenous/runtime/tick_invocation_frame.h>

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

iv::GlobalIndex index(std::int64_t value)
{
    return iv::GlobalIndex::from_signed(value);
}

iv::GlobalIndexRange range(std::int64_t begin, std::uint64_t extent)
{
    return {.begin = index(begin), .extent = extent};
}

iv::PersistedOutputId stable_output(iv::PortKind kind, char const* name)
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
        .generation = 1,
        .port = port,
        .kind = kind,
    };
}

iv::ReflectedContinuousPortStorageBinding sample_storage_binding(
    std::span<iv::Sample> samples)
{
    auto const geometry = iv::continuous_value_storage_geometry(
        iv::ContinuousValueTypeId::sample, std::nullopt);
    IV_ASSERT(geometry.has_value(),
        "Sample must have a registered continuous storage geometry");
    iv::ReflectedContinuousPortStorageBinding result{
        .geometry = *geometry,
        .value_capacity = samples.size(),
        .channel_layout = iv::mono_planar_channel_layout,
    };
    result.channels[0] = {
        .storage = reinterpret_cast<std::byte*>(samples.data()),
        .value_capacity = samples.size(),
        .value_stride_bytes = sizeof(iv::Sample),
    };
    return result;
}

iv::ReflectedContinuousOutputPortBinding sample_output_binding(
    std::span<iv::Sample> samples,
    std::size_t history,
    std::size_t latency,
    iv::OutputDisposition disposition)
{
    return {
        .storage = sample_storage_binding(samples),
        .history = history,
        .latency = latency,
        .disposition = disposition,
    };
}

iv::PersistedSampleRegion sample_region(
    iv::PersistedOutputHandle output,
    iv::GlobalIndexRange selected,
    float first)
{
    std::vector<iv::Sample> values;
    values.reserve(static_cast<std::size_t>(selected.extent));
    for (std::uint64_t offset = 0; offset < selected.extent; ++offset) {
        values.push_back(first + static_cast<float>(offset));
    }
    return {
        .output = output,
        .range = selected,
        .layout = iv::mono_planar_channel_layout,
        .values = std::move(values),
    };
}

iv::PersistedEventChunk event_chunk(
    iv::PersistedOutputHandle output,
    iv::GlobalIndexRange selected,
    std::initializer_list<std::int64_t> times)
{
    iv::PersistedEventChunk result{
        .output = output,
        .range = selected,
        .type = iv::EventTypeId::trigger,
    };
    for (auto time : times) {
        result.events.push_back({
            .time = index(time),
            .value = iv::TriggerEvent{},
        });
    }
    return result;
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
    iv::graph_jit::BackgroundEvaluationCall*)
{}

iv::CompiledGraph queued_persisted_graph(iv::PortKind kind)
{
    using namespace iv::graph_jit;
    iv::CompiledGraph graph;
    graph.project_generation = 1;
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
        .storage = PortStorageKind::persisted_data,
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

iv::graph_jit::BackgroundEvaluationPlan direct_tick_plan(iv::PortKind kind)
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
            .configured_port = {0, kind, 0},
            .kind = kind,
            .direction = PortDirection::output,
            .persisted_tick_output = true,
            .output_history = 1,
            .output_latency = 1,
            .retention = iv::OutputRetention::persisted,
            .sample_layout = iv::mono_planar_channel_layout,
            .event_type = iv::EventTypeId::trigger,
            .max_events_per_index = 1.0,
        },
        BackgroundPortPlan{
            .node = 1,
            .configured_port = {1, kind, 0},
            .kind = kind,
            .direction = PortDirection::input,
            .random_access_input = true,
            .sample_layout = iv::mono_planar_channel_layout,
            .event_type = iv::EventTypeId::trigger,
        },
    };
    plan.storage.ports = {{
        .kind = kind,
        .storage = PortStorageKind::persisted_data,
        .source_port = iv::NodeBundlePortId{0, kind, 0},
        .output_port = 0,
        .sample_layout = iv::mono_planar_channel_layout,
        .sample_channels = kind == iv::PortKind::sample
            ? std::vector<std::size_t>{0}
            : std::vector<std::size_t>{},
        .event_type = iv::EventTypeId::trigger,
    }};
    if (kind == iv::PortKind::sample) {
        plan.sample_target_subsets = {{
            .port = {1, kind, 0},
            .target_layout = iv::mono_planar_channel_layout,
            .access = PlannedDestinationAccess::random_access,
            .channels = {{1, 0, 0}},
        }};
        plan.storage.ports[0].source_subsets = {0};
        plan.storage.ports[0].target_subsets = {0};
        plan.storage.sample_target_storage = {{0}};
        plan.storage.direct_samples = {{
            .target_subset = 0,
            .target_channel = 0,
            .source_subset = 0,
            .source_channel = 0,
            .storage = 0,
            .delivery =
                PlannedDeliveryMechanism::persisted_tick_to_random_access,
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
                .sample_capture_begin = 1,
            },
        };
    } else {
        plan.event_target_subsets = {{
            .port = {1, 0},
            .type = iv::EventTypeId::trigger,
            .access = PlannedDestinationAccess::random_access,
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
                .event_capture_begin = 1,
            },
        };
    }
    return plan;
}

iv::graph_jit::BackgroundEvaluationPlan sequential_tick_sample_plan()
{
    auto plan = direct_tick_plan(iv::PortKind::sample);
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
    plan.tick_runtime.sample_captures.clear();
    plan.tick_runtime.sequential_sample_inputs = {{
        .port = 1,
        .storage = {0},
    }};
    plan.tick_runtime.nodes[0] = {};
    plan.tick_runtime.nodes[1] = {
        .sequential_sample_begin = 0,
        .sequential_sample_count = 1,
    };
    return plan;
}

iv::Sample sequential_sample_at(
    iv::ReflectedContinuousInputPortBinding const& binding,
    iv::GlobalIndex position,
    std::size_t channel = 0)
{
    auto const& selected = binding.storage.channels[channel];
    auto const ordinal = position.bits()
        + binding.storage.storage_latency
        - selected.value_delay;
    auto const slot = static_cast<std::size_t>(
        ordinal & (selected.value_capacity - 1));
    auto const* value = reinterpret_cast<iv::Sample const*>(
        selected.storage + slot * selected.value_stride_bytes);
    return *value;
}

void publish_record(
    iv::ProducerReserve& reserve,
    iv::PendingQueue& pending,
    iv::RealtimeProducedRecordHeader header,
    std::span<std::byte const> payload = {})
{
    auto chain = reserve.acquire(header.record_block_count);
    ASSERT_TRUE(chain);
    auto writer = chain.writer();
    ASSERT_TRUE(writer.append(std::as_bytes(std::span{&header, 1u})));
    if (!payload.empty()) ASSERT_TRUE(writer.append(payload));
    ASSERT_TRUE(pending.publish(std::move(chain)));
}

static_assert(noexcept(
    std::declval<iv::PersistedDataStore::ReaderSlot&>().pin()));
static_assert(std::is_nothrow_destructible_v<
    iv::PersistedDataStore::ReaderPin>);
static_assert(std::is_nothrow_constructible_v<
    iv::TickInvocationFrame,
    iv::PersistedDataStore::ReaderSlot&,
    iv::TickMaterializationStore::ReaderSlot&,
    iv::TickInvocationWorkspace&,
    iv::GlobalIndex,
    std::size_t>);

TEST(PersistedDataStore, ResolvesStableTypedStoreLocalHandles)
{
    iv::PersistedDataStore first;
    auto const sample_id = stable_output(iv::PortKind::sample, "samples");
    auto const event_id = stable_output(iv::PortKind::event, "events");

    auto const sample = first.resolve_output(sample_id);
    auto const event = first.resolve_output(event_id);
    EXPECT_TRUE(sample.valid());
    EXPECT_TRUE(event.valid());
    EXPECT_EQ(sample.kind(), iv::PortKind::sample);
    EXPECT_EQ(event.kind(), iv::PortKind::event);
    EXPECT_EQ(first.resolve_output(sample_id), sample);
    EXPECT_NE(sample, event);

    iv::PersistedDataStore second;
    EXPECT_NE(second.resolve_output(sample_id), sample);
}

TEST(PersistedDataStore, PublishesSamplesAndAuthoritativeEmptyEventsTogether)
{
    iv::PersistedDataStore store;
    auto const samples = store.resolve_output(
        stable_output(iv::PortKind::sample, "samples"));
    auto const events = store.resolve_output(
        stable_output(iv::PortKind::event, "events"));
    auto reader = store.register_reader();
    auto before = reader.pin();

    auto candidate = store.begin_candidate(12);
    candidate.overwrite(sample_region(samples, range(-2, 4), 1.0f));
    candidate.overwrite(event_chunk(events, range(-2, 4), {}));
    EXPECT_EQ(store.publish(std::move(candidate)),
        iv::PersistedDataPublishResult::published);

    auto current_reader = store.register_reader();
    auto current = current_reader.pin();
    EXPECT_EQ(before->version(), (iv::PersistedDataVersion{}));
    EXPECT_EQ(current->version(),
        (iv::PersistedDataVersion{.semantic = 12, .revision = 1}));
    EXPECT_EQ(current->sample_region_count(), 1u);
    EXPECT_EQ(current->event_chunk_count(), 1u);

    auto const* sample = current->find_sample_region(samples, index(0));
    ASSERT_NE(sample, nullptr);
    EXPECT_FLOAT_EQ(sample->at(index(-2), 0), 1.0f);
    EXPECT_FLOAT_EQ(sample->at(index(1), 0), 4.0f);

    auto const* empty = current->find_event_chunk(events, index(0));
    ASSERT_NE(empty, nullptr);
    EXPECT_TRUE(empty->events.empty());
    auto const* event_coverage = current->find_event_coverage(
        events, iv::EventTypeId::trigger);
    ASSERT_NE(event_coverage, nullptr);
    EXPECT_TRUE(event_coverage->contains(range(-2, 4)));
}

TEST(PersistedDataStore, OverwriteSplitsOnlyIntersectingSampleRegions)
{
    iv::PersistedDataStore store;
    auto const output = store.resolve_output(
        stable_output(iv::PortKind::sample, "samples"));
    auto first = store.begin_candidate(1);
    first.overwrite(sample_region(output, range(-8, 16), 0.0f));
    ASSERT_EQ(store.publish(std::move(first)),
        iv::PersistedDataPublishResult::published);

    auto old_reader = store.register_reader();
    auto old = old_reader.pin();
    auto const* old_record = old->find_sample_region(output, index(-8));
    ASSERT_NE(old_record, nullptr);

    auto replacement = store.begin_candidate(2);
    replacement.overwrite(sample_region(output, range(-2, 4), 100.0f));
    ASSERT_EQ(store.publish(std::move(replacement)),
        iv::PersistedDataPublishResult::published);

    auto reader = store.register_reader();
    auto current = reader.pin();
    EXPECT_EQ(current->sample_region_count(), 3u);
    EXPECT_FLOAT_EQ(
        current->find_sample_region(output, index(-3))->at(index(-3), 0),
        5.0f);
    EXPECT_FLOAT_EQ(
        current->find_sample_region(output, index(-2))->at(index(-2), 0),
        100.0f);
    EXPECT_FLOAT_EQ(
        current->find_sample_region(output, index(1))->at(index(1), 0),
        103.0f);
    EXPECT_FLOAT_EQ(
        current->find_sample_region(output, index(2))->at(index(2), 0),
        10.0f);
    EXPECT_EQ(old->find_sample_region(output, index(-8)), old_record);
    EXPECT_FLOAT_EQ(old_record->at(index(-2), 0), 6.0f);
}

TEST(PersistedDataStore, UnchangedOutputsShareRecordsAcrossPublishedRoots)
{
    iv::PersistedDataStore store;
    auto const samples = store.resolve_output(
        stable_output(iv::PortKind::sample, "samples"));
    auto const events = store.resolve_output(
        stable_output(iv::PortKind::event, "events"));
    auto seed = store.begin_candidate(7);
    seed.overwrite(sample_region(samples, range(0, 4), 2.0f));
    ASSERT_EQ(store.publish(std::move(seed)),
        iv::PersistedDataPublishResult::published);
    auto old_reader = store.register_reader();
    auto old = old_reader.pin();
    auto const* shared = old->find_sample_region(samples, index(0));
    ASSERT_NE(shared, nullptr);

    auto successor = store.begin_candidate(7);
    successor.overwrite(event_chunk(events, range(0, 4), {1, 3}));
    ASSERT_EQ(store.publish(std::move(successor)),
        iv::PersistedDataPublishResult::published);
    auto current_reader = store.register_reader();
    auto current = current_reader.pin();
    EXPECT_EQ(current->find_sample_region(samples, index(0)), shared);
    EXPECT_EQ(old->find_event_chunk(events, index(1)), nullptr);
    EXPECT_NE(current->find_event_chunk(events, index(1)), nullptr);
}

TEST(PersistedDataStore, CircularOverwriteSplitsAtSignedOrderingSeam)
{
    iv::PersistedDataStore store;
    auto const output = store.resolve_output(
        stable_output(iv::PortKind::sample, "samples"));
    auto const begin = iv::GlobalIndex::from_signed(
        std::numeric_limits<std::int64_t>::max() - 1);
    auto candidate = store.begin_candidate(1);
    candidate.overwrite(sample_region(output, {begin, 4}, 10.0f));
    ASSERT_EQ(store.publish(std::move(candidate)),
        iv::PersistedDataPublishResult::published);

    auto reader = store.register_reader();
    auto snapshot = reader.pin();
    EXPECT_EQ(snapshot->sample_region_count(), 2u);
    for (std::uint64_t offset = 0; offset < 4; ++offset) {
        auto const position = begin.advanced_by(offset);
        auto const* selected = snapshot->find_sample_region(output, position);
        ASSERT_NE(selected, nullptr);
        EXPECT_FLOAT_EQ(
            selected->at(position, 0), 10.0f + static_cast<float>(offset));
    }

    std::vector<iv::GlobalIndexRange> intersections;
    snapshot->for_each_sample_region(
        output,
        {begin, 4},
        [&](iv::PersistedSampleRegion const&, iv::GlobalIndexRange selected) {
            intersections.push_back(selected);
        });
    ASSERT_EQ(intersections.size(), 2u);
    EXPECT_EQ(intersections[0], (iv::GlobalIndexRange{begin, 2}));
    EXPECT_EQ(intersections[1],
        (iv::GlobalIndexRange{iv::GlobalIndex::from_signed(
             std::numeric_limits<std::int64_t>::min()), 2}));
}

TEST(PersistedDataStore, EventIterationIsClippedAndRangeRelative)
{
    iv::PersistedDataStore store;
    auto const output = store.resolve_output(
        stable_output(iv::PortKind::event, "events"));
    auto candidate = store.begin_candidate(1);
    candidate.overwrite(event_chunk(output, range(-4, 8), {-4, -1, 0, 3}));
    ASSERT_EQ(store.publish(std::move(candidate)),
        iv::PersistedDataPublishResult::published);
    auto reader = store.register_reader();
    auto snapshot = reader.pin();

    std::vector<std::int64_t> observed;
    snapshot->for_each_event_chunk(
        output,
        range(-1, 3),
        [&](iv::PersistedEventChunk const& chunk,
            iv::GlobalIndexRange selected) {
            for (auto const& event : chunk.events) {
                if (selected.contains(event.time)) {
                    observed.push_back(event.time.signed_value());
                }
            }
        });
    EXPECT_EQ(observed, (std::vector<std::int64_t>{-1, 0}));
}

TEST(PersistedDataStore, ErasureSplitsRetainedDataAndCoverageExactly)
{
    iv::PersistedDataStore store;
    auto const output = store.resolve_output(
        stable_output(iv::PortKind::sample, "samples"));
    auto seed = store.begin_candidate(1);
    seed.overwrite(sample_region(output, range(0, 10), 0.0f));
    ASSERT_EQ(store.publish(std::move(seed)),
        iv::PersistedDataPublishResult::published);

    auto erased = store.begin_candidate(2);
    erased.erase_sample(output, range(3, 4), iv::mono_planar_channel_layout);
    ASSERT_EQ(store.publish(std::move(erased)),
        iv::PersistedDataPublishResult::published);
    auto reader = store.register_reader();
    auto snapshot = reader.pin();
    EXPECT_EQ(snapshot->sample_region_count(), 2u);
    EXPECT_NE(snapshot->find_sample_region(output, index(2)), nullptr);
    EXPECT_EQ(snapshot->find_sample_region(output, index(3)), nullptr);
    EXPECT_EQ(snapshot->find_sample_region(output, index(6)), nullptr);
    EXPECT_NE(snapshot->find_sample_region(output, index(7)), nullptr);
    auto const* coverage = snapshot->find_sample_coverage(
        output, iv::mono_planar_channel_layout);
    ASSERT_NE(coverage, nullptr);
    EXPECT_TRUE(coverage->contains(range(0, 3)));
    EXPECT_TRUE(coverage->contains(range(7, 3)));
    EXPECT_FALSE(coverage->contains(index(3)));
}

TEST(PersistedDataStore, RejectsInvalidPayloadsAndForeignHandles)
{
    iv::PersistedDataStore store;
    iv::PersistedDataStore foreign;
    auto const output = store.resolve_output(
        stable_output(iv::PortKind::sample, "samples"));
    auto const foreign_output = foreign.resolve_output(
        stable_output(iv::PortKind::sample, "samples"));
    auto candidate = store.begin_candidate(1);
    auto wrong_size = sample_region(output, range(0, 4), 0.0f);
    wrong_size.values.pop_back();
    EXPECT_THROW(candidate.overwrite(std::move(wrong_size)),
        std::invalid_argument);
    EXPECT_THROW(
        candidate.overwrite(sample_region(foreign_output, range(0, 4), 0.0f)),
        std::invalid_argument);
}

TEST(PersistedDataStore, RejectsSecondCandidateFromTheSameBaseAsStale)
{
    iv::PersistedDataStore store;
    auto const output = store.resolve_output(
        stable_output(iv::PortKind::sample, "samples"));
    auto first = store.begin_candidate(4);
    auto stale = store.begin_candidate(4);
    first.overwrite(sample_region(output, range(0, 4), 10.0f));
    stale.overwrite(sample_region(output, range(0, 4), 20.0f));
    EXPECT_EQ(store.publish(std::move(first)),
        iv::PersistedDataPublishResult::published);
    EXPECT_EQ(store.publish(std::move(stale)),
        iv::PersistedDataPublishResult::stale_base);
}

TEST(PersistedDataStore, ReaderPinsDeferRetiredRootReclamation)
{
    iv::PersistedDataStore store;
    auto const output = store.resolve_output(
        stable_output(iv::PortKind::sample, "samples"));
    auto reader = store.register_reader();
    {
        auto pinned = reader.pin();
        auto candidate = store.begin_candidate(1);
        candidate.overwrite(sample_region(output, range(0, 4), 1.0f));
        ASSERT_EQ(store.publish(std::move(candidate)),
            iv::PersistedDataPublishResult::published);
        EXPECT_EQ(store.reclaim_retired(), 0u);
        EXPECT_EQ(pinned->version(), (iv::PersistedDataVersion{}));
    }
    EXPECT_EQ(store.reclaim_retired(), 1u);
}

TEST(PersistedDataStore, RadixDirectoryKeepsManyOutputSlotsDistinct)
{
    iv::PersistedDataStore store;
    std::vector<iv::PersistedOutputHandle> outputs;
    constexpr std::size_t output_count = 300;
    constexpr std::size_t ranges_per_output = 4;
    outputs.reserve(output_count);

    auto candidate = store.begin_candidate(1);
    for (std::size_t output = 0; output < output_count; ++output) {
        auto const handle = store.resolve_output(local_output(
            iv::PortKind::sample,
            static_cast<iv::graph_jit::BackgroundPortIndex>(output)));
        outputs.push_back(handle);
        for (std::size_t region = 0; region < ranges_per_output; ++region) {
            auto const begin = static_cast<std::int64_t>(
                output * 1000 + region * 4);
            candidate.overwrite(sample_region(
                handle,
                range(begin, 2),
                static_cast<float>(output * 100 + region)));
        }
    }
    ASSERT_EQ(store.publish(std::move(candidate)),
        iv::PersistedDataPublishResult::published);

    auto old_reader = store.register_reader();
    auto old = old_reader.pin();
    EXPECT_EQ(
        old->sample_region_count(), output_count * ranges_per_output);
    auto const* unchanged = old->find_sample_region(outputs[299], index(299000));
    ASSERT_NE(unchanged, nullptr);

    auto replacement = store.begin_candidate(2);
    replacement.overwrite(
        sample_region(outputs[117], range(117000, 2), 9000.0f));
    replacement.erase_output(outputs[231]);
    ASSERT_EQ(store.publish(std::move(replacement)),
        iv::PersistedDataPublishResult::published);

    auto current_reader = store.register_reader();
    auto current = current_reader.pin();
    EXPECT_EQ(
        current->find_sample_region(outputs[299], index(299000)), unchanged);
    EXPECT_EQ(
        current->find_sample_region(outputs[231], index(231000)), nullptr);
    auto const* replaced =
        current->find_sample_region(outputs[117], index(117000));
    ASSERT_NE(replaced, nullptr);
    EXPECT_FLOAT_EQ(replaced->at(index(117000), 0).value, 9000.0f);
    EXPECT_NE(
        old->find_sample_region(outputs[231], index(231000)), nullptr);
}

TEST(TickInvocationWorkspace, FramePinsOnePersistedDataRootForItsLifetime)
{
    iv::PersistedDataStore data;
    auto reader = data.register_reader();
    iv::TickMaterializationStore materializations;
    auto materialization_reader = materializations.register_reader();
    iv::graph_jit::BackgroundEvaluationPlan plan;
    iv::TickInvocationWorkspace workspace{plan, 1, data};

    {
        iv::TickInvocationFrame frame{
            reader, materialization_reader, workspace};
        EXPECT_EQ(frame.published_data().version(),
            (iv::PersistedDataVersion{}));

        auto candidate = data.begin_candidate(1);
        ASSERT_EQ(data.publish(std::move(candidate)),
            iv::PersistedDataPublishResult::published);
        EXPECT_EQ(data.retired_snapshot_count(), 1u);
        EXPECT_EQ(data.reclaim_retired(), 0u);
        EXPECT_EQ(frame.published_data().version(),
            (iv::PersistedDataVersion{}));
    }

    EXPECT_EQ(data.reclaim_retired(), 1u);
}

TEST(TickInvocationWorkspace, RejectsAnInvalidRetainedSlotPlanBeforeRealtimeUse)
{
    iv::PersistedDataStore data;
    iv::graph_jit::BackgroundEvaluationPlan plan;
    plan.tick_runtime.nodes.push_back(
        iv::graph_jit::TickNodeInvocationPlan{
            .sequential_sample_begin = 1,
        });

    EXPECT_THROW(
        (iv::TickInvocationWorkspace{plan, 1, data}),
        std::invalid_argument);
}

TEST(TickInvocationWorkspace,
     RandomAccessSampleViewReadsARegionAcrossTheSignedOrderingSeam)
{
    auto plan = direct_tick_plan(iv::PortKind::sample);
    iv::PersistedDataStore data;
    auto reader = data.register_reader();
    iv::TickMaterializationStore materializations;
    auto materialization_reader = materializations.register_reader();
    auto const output = data.resolve_output(
        local_output(iv::PortKind::sample, 0));
    auto const begin = iv::GlobalIndex::from_signed(
        std::numeric_limits<std::int64_t>::max() - 1);
    auto candidate = data.begin_candidate(3);
    candidate.overwrite(sample_region(output, {begin, 4}, 10.0f));
    ASSERT_EQ(data.publish(std::move(candidate)),
        iv::PersistedDataPublishResult::published);

    iv::TickInvocationWorkspace workspace{plan, 1, data, 4};
    iv::TickInvocationFrame frame{
        reader, materialization_reader, workspace};
    auto const inputs = static_cast<
        std::span<iv::RandomAccessSampleInputPort const>>(
        frame.call().random_access_sample_inputs);
    ASSERT_EQ(inputs.size(), 1u);
    EXPECT_TRUE(inputs[0].coverage().contains(
        iv::GlobalIndexRange{begin, 4}));
    for (std::uint64_t offset = 0; offset < 4; ++offset) {
        EXPECT_FLOAT_EQ(
            inputs[0].at(begin.advanced_by(offset), 0).value,
            10.0f + static_cast<float>(offset));
    }
}

TEST(TickInvocationWorkspace,
     RandomAccessEventViewVisitsSeveralChunksInTimelineOrder)
{
    auto plan = direct_tick_plan(iv::PortKind::event);
    iv::PersistedDataStore data;
    auto reader = data.register_reader();
    iv::TickMaterializationStore materializations;
    auto materialization_reader = materializations.register_reader();
    auto const output = data.resolve_output(
        local_output(iv::PortKind::event, 0));
    auto candidate = data.begin_candidate(3);
    candidate.overwrite(event_chunk(output, range(0, 4), {2}));
    candidate.overwrite(event_chunk(output, range(4, 4), {4, 7}));
    ASSERT_EQ(data.publish(std::move(candidate)),
        iv::PersistedDataPublishResult::published);

    iv::TickInvocationWorkspace workspace{plan, 1, data, 4};
    iv::TickInvocationFrame frame{
        reader, materialization_reader, workspace};
    auto const inputs = static_cast<
        std::span<iv::RandomAccessEventInputPort const>>(
        frame.call().random_access_event_inputs);
    ASSERT_EQ(inputs.size(), 1u);
    std::vector<iv::GlobalIndex> times;
    inputs[0].for_each(range(1, 7), [&](iv::TimedEvent const& event) {
        times.push_back(event.time);
    });
    EXPECT_EQ(times, (std::vector<iv::GlobalIndex>{
        index(2), index(4), index(7)}));
}

TEST(TickInvocationWorkspace,
     SequentialSampleViewCopiesTheSelectedMaterializationAndFillsNeutral)
{
    auto plan = sequential_tick_sample_plan();
    iv::PersistedDataStore data;
    auto reader = data.register_reader();
    iv::TickMaterializationStore materializations;
    auto materialization_reader = materializations.register_reader();
    (void)materializations.promote(
        std::make_unique<iv::TickMaterializationSnapshot>(
            1,
            1,
            iv::PersistedDataVersion{},
            std::vector<iv::TickMaterializedSampleInput>{
                {
                    .port = 1,
                    .coverage = iv::IndexCoverage{range(4, 3)},
                    .layout = plan.ports[1].sample_layout,
                    .channels = {0},
                    .values = {40.0f, 50.0f, 60.0f},
                },
            },
            std::vector<iv::TickMaterializedEventInput>{}));

    iv::TickInvocationWorkspace workspace{plan, 1, data, 4};
    iv::TickInvocationFrame frame{
        reader,
        materialization_reader,
        workspace,
        index(5),
        3};
    auto const inputs = static_cast<
        std::span<iv::ReflectedContinuousInputPortBinding const>>(
        frame.call().sequential_sample_inputs);
    ASSERT_EQ(inputs.size(), 1u);
    EXPECT_FLOAT_EQ(sequential_sample_at(inputs[0], index(3)).value, -0.25f);
    EXPECT_FLOAT_EQ(sequential_sample_at(inputs[0], index(4)).value, 40.0f);
    EXPECT_FLOAT_EQ(sequential_sample_at(inputs[0], index(5)).value, 50.0f);
    EXPECT_FLOAT_EQ(sequential_sample_at(inputs[0], index(6)).value, 60.0f);
    EXPECT_FLOAT_EQ(sequential_sample_at(inputs[0], index(7)).value, -0.25f);
}

TEST(TickInvocationWorkspace,
     SampleCapturePublishesOneWrappedBlockAfterTheCallbackFinishes)
{
    auto plan = direct_tick_plan(iv::PortKind::sample);
    iv::PersistedDataStore data;
    auto data_reader = data.register_reader();
    iv::TickMaterializationStore materializations;
    auto materialization_reader = materializations.register_reader();
    iv::TickInvocationWorkspace workspace{plan, 1, data, 4};
    iv::ProducerReserve reserve{iv::realtime_produced_block_storage_size};
    iv::AsyncCapacityManager manager{1};
    iv::AsyncWorkSignal work_signal;
    iv::PendingQueue pending{reserve, work_signal};
    std::atomic<bool> reservation_failed{false};
    ASSERT_EQ(manager.maintain(reserve, {1, 1, 2}), 2u);
    workspace.bind_producer_endpoint(
        0, reserve, pending, reservation_failed);

    std::array<iv::Sample, 8> source{
        100.0f, 101.0f, 102.0f, 103.0f,
        104.0f, 105.0f, 106.0f, 107.0f};
    auto binding = sample_output_binding(
        std::span{source}, 1, 1, iv::OutputDisposition::written);
    binding.sequential_begin_ordinal = 6;
    auto const begin = iv::GlobalIndex::from_bits(
        std::numeric_limits<std::uint64_t>::max() - 1);

    {
        iv::TickInvocationFrame frame{
            data_reader, materialization_reader, workspace, begin, 4};
        auto const& operation = frame.call().sample_captures.data()[0];
        ASSERT_NE(operation.context, nullptr);
        ASSERT_NE(operation.capture, nullptr);
        binding.disposition = iv::OutputDisposition::untouched;
        operation.capture(operation.context, &binding, begin, 4);
        binding.disposition = iv::OutputDisposition::written;
        operation.capture(operation.context, &binding, begin, 4);
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
    EXPECT_EQ(header.begin, begin);
    EXPECT_EQ(header.sample_count, 4u);
    EXPECT_EQ(header.sample_layout, iv::mono_planar_channel_layout);
    std::vector<iv::Sample> captured(header.sample_count);
    ASSERT_EQ(bytes.size(), sizeof(header) + header.payload_size);
    std::memcpy(
        captured.data(), bytes.data() + sizeof(header), header.payload_size);
    EXPECT_EQ(captured, (std::vector<iv::Sample>{
        106.0f, 107.0f, 100.0f, 101.0f}));
}

TEST(TickInvocationWorkspace,
     EmptyWrittenEventBlockPublishesAnAuthoritativeEmptyRecord)
{
    auto plan = direct_tick_plan(iv::PortKind::event);
    iv::PersistedDataStore data;
    auto data_reader = data.register_reader();
    iv::TickMaterializationStore materializations;
    auto materialization_reader = materializations.register_reader();
    iv::TickInvocationWorkspace workspace{plan, 1, data, 4};
    iv::ProducerReserve reserve{iv::realtime_produced_block_storage_size};
    iv::AsyncCapacityManager manager{1};
    iv::AsyncWorkSignal work_signal;
    iv::PendingQueue pending{reserve, work_signal};
    std::atomic<bool> reservation_failed{false};
    ASSERT_EQ(manager.maintain(reserve, {1, 1, 2}), 2u);
    workspace.bind_producer_endpoint(
        0, reserve, pending, reservation_failed);

    struct EventStorage {
        std::size_t count = 0;
        std::array<iv::TimedEvent, 8> events{};
    } storage;
    iv::ReflectedEventOutputPortBinding binding{
        .storage = {
            .storage = reinterpret_cast<std::byte*>(&storage),
            .count_offset = offsetof(EventStorage, count),
            .events_offset = offsetof(EventStorage, events),
            .event_capacity = storage.events.size(),
            .type = iv::EventTypeId::trigger,
        },
        .source_type = iv::EventTypeId::trigger,
        .history = 1,
        .latency = 1,
        .disposition = iv::OutputDisposition::written,
    };

    {
        iv::TickInvocationFrame frame{
            data_reader, materialization_reader, workspace, index(-2), 4};
        auto const& operation = frame.call().event_captures.data()[0];
        operation.capture(operation.context, &binding, index(-2), 4);
        EXPECT_TRUE(pending.pin().empty());
    }

    EXPECT_FALSE(reservation_failed.load());
    auto selected = pending.pin();
    auto const bytes = queued_bytes(selected);
    ASSERT_EQ(bytes.size(), sizeof(iv::RealtimeProducedRecordHeader));
    iv::RealtimeProducedRecordHeader header;
    std::memcpy(&header, bytes.data(), sizeof(header));
    EXPECT_EQ(header.payload_kind,
        iv::RealtimeProducedPayloadKind::events);
    EXPECT_EQ(header.payload_size, 0u);
    EXPECT_EQ(header.begin, index(-2));
    EXPECT_EQ(header.sample_count, 4u);
    EXPECT_EQ(header.event_count, 0u);
}

TEST(TickInvocationWorkspace,
     SampleCaptureReserveExhaustionIsStickyAndPublishesNothing)
{
    auto plan = direct_tick_plan(iv::PortKind::sample);
    iv::PersistedDataStore data;
    auto data_reader = data.register_reader();
    iv::TickMaterializationStore materializations;
    auto materialization_reader = materializations.register_reader();
    iv::TickInvocationWorkspace workspace{plan, 1, data, 4};
    iv::ProducerReserve reserve{iv::realtime_produced_block_storage_size};
    iv::AsyncWorkSignal work_signal;
    iv::PendingQueue pending{reserve, work_signal};
    std::atomic<bool> reservation_failed{false};
    workspace.bind_producer_endpoint(
        0, reserve, pending, reservation_failed);

    std::array<iv::Sample, 8> source{};
    auto binding = sample_output_binding(
        std::span{source}, 1, 1, iv::OutputDisposition::written);

    {
        iv::TickInvocationFrame frame{
            data_reader, materialization_reader, workspace, index(8), 4};
        auto const& operation = frame.call().sample_captures.data()[0];
        operation.capture(operation.context, &binding, index(8), 4);
    }

    EXPECT_TRUE(reservation_failed.load());
    EXPECT_TRUE(pending.pin().empty());
}

TEST(TickInvocationWorkspace,
     VoidedSampleBlockPublishesOneHeaderOnlyEraseRecord)
{
    auto plan = direct_tick_plan(iv::PortKind::sample);
    iv::PersistedDataStore data;
    auto data_reader = data.register_reader();
    iv::TickMaterializationStore materializations;
    auto materialization_reader = materializations.register_reader();
    iv::TickInvocationWorkspace workspace{plan, 1, data, 4};
    iv::ProducerReserve reserve{iv::realtime_produced_block_storage_size};
    iv::AsyncCapacityManager manager{1};
    iv::AsyncWorkSignal work_signal;
    iv::PendingQueue pending{reserve, work_signal};
    std::atomic<bool> reservation_failed{false};
    ASSERT_EQ(manager.maintain(reserve, {1, 1, 2}), 2u);
    workspace.bind_producer_endpoint(
        0, reserve, pending, reservation_failed);

    std::array<iv::Sample, 8> source{};
    auto binding = sample_output_binding(
        std::span{source}, 1, 1, iv::OutputDisposition::voided);

    {
        iv::TickInvocationFrame frame{
            data_reader, materialization_reader, workspace, index(-4), 4};
        auto const& operation = frame.call().sample_captures.data()[0];
        operation.capture(operation.context, &binding, index(-4), 4);
    }

    EXPECT_FALSE(reservation_failed.load());
    auto const bytes = queued_bytes(pending.pin());
    ASSERT_EQ(bytes.size(), sizeof(iv::RealtimeProducedRecordHeader));
    iv::RealtimeProducedRecordHeader header;
    std::memcpy(&header, bytes.data(), sizeof(header));
    EXPECT_EQ(header.payload_kind,
        iv::RealtimeProducedPayloadKind::void_value);
    EXPECT_EQ(header.payload_size, 0u);
    EXPECT_EQ(header.begin, index(-4));
    EXPECT_EQ(header.sample_count, 4u);
    EXPECT_EQ(header.sample_layout, iv::mono_planar_channel_layout);
}

TEST(BackgroundEvaluationTransaction,
     AppliesOverlappingQueuedSampleOverwritesInPublicationOrder)
{
    auto graph = queued_persisted_graph(iv::PortKind::sample);
    iv::ResourceContext resources;
    auto tock_states = graph.tock_state_layout.create_storage(resources);
    tock_states.initialize();
    iv::BackgroundCoverageState coverage{1};
    iv::BackgroundPropagationWorkspace propagation{
        graph.background_evaluation_plan, graph.specialization.sample_rate};
    iv::PersistedDataStore data;
    auto reader = data.register_reader();
    iv::TickMaterializationStore materializations;
    iv::ProducerReserve reserve{iv::realtime_produced_block_storage_size};
    iv::AsyncCapacityManager manager{1};
    iv::AsyncWorkSignal work_signal;
    iv::PendingQueue pending{reserve, work_signal};
    ASSERT_EQ(manager.maintain(reserve, {2, 2, 4}), 4u);

    std::array<iv::Sample, 4> first{10.0f, 11.0f, 12.0f, 13.0f};
    publish_record(
        reserve,
        pending,
        {
            .payload_kind = iv::RealtimeProducedPayloadKind::samples,
            .record_block_count = 1,
            .payload_size = sizeof(first),
            .begin = index(2),
            .sample_count = first.size(),
            .sample_layout = iv::mono_planar_channel_layout,
        },
        std::as_bytes(std::span{first}));
    std::array<iv::Sample, 2> second{30.0f, 31.0f};
    publish_record(
        reserve,
        pending,
        {
            .payload_kind = iv::RealtimeProducedPayloadKind::samples,
            .record_block_count = 1,
            .payload_size = sizeof(second),
            .begin = index(3),
            .sample_count = second.size(),
            .sample_layout = iv::mono_planar_channel_layout,
        },
        std::as_bytes(std::span{second}));

    auto const output = data.resolve_output(
        local_output(iv::PortKind::sample, 0));
    std::array routes{iv::BackgroundProducedInputRoute{
        .output = output,
        .port = 0,
        .kind = iv::PortKind::sample,
    }};
    std::array selections{pending.pin()};
    iv::BackgroundEvaluationTransaction transaction{
        graph,
        tock_states,
        coverage,
        propagation,
        data,
        materializations,
        {},
        routes,
        selections,
    };
    auto result = transaction.execute();
    ASSERT_TRUE(result.has_value()) << result.error();
    ASSERT_EQ(result->status, iv::BackgroundEvaluationStatus::committed);
    ASSERT_TRUE(result->published_data.has_value());

    auto published = reader.pin();
    EXPECT_EQ(published->sample_region_count(), 3u);
    std::array expected{10.0f, 30.0f, 31.0f, 13.0f};
    for (std::size_t offset = 0; offset < expected.size(); ++offset) {
        auto const position = index(2).advanced_by(offset);
        auto const* region =
            published->find_sample_region(output, position);
        ASSERT_NE(region, nullptr);
        EXPECT_FLOAT_EQ(region->at(position, 0).value, expected[offset]);
    }
}

TEST(BackgroundEvaluationTransaction,
     RetiresADisappearedDestinationInTheSuccessorReconciliationCommit)
{
    iv::CompiledGraph graph;
    graph.project_generation = 2;
    graph.specialization.sample_rate = 48000;
    iv::ResourceContext resources;
    auto tock_states = graph.tock_state_layout.create_storage(resources);
    tock_states.initialize();
    iv::BackgroundCoverageState coverage{0};
    iv::BackgroundPropagationWorkspace propagation{
        graph.background_evaluation_plan, graph.specialization.sample_rate};
    iv::PersistedDataStore data;
    auto reader = data.register_reader();
    auto const output = data.resolve_output(
        local_output(iv::PortKind::sample, 0));
    auto initial = data.begin_candidate(1);
    initial.overwrite(iv::PersistedSampleRegion{
        .output = output,
        .range = range(2, 2),
        .layout = iv::mono_planar_channel_layout,
        .values = {iv::Sample{10.0f}, iv::Sample{11.0f}},
    });
    ASSERT_EQ(
        data.publish(std::move(initial)),
        iv::PersistedDataPublishResult::published);

    iv::TickMaterializationStore materializations;
    std::array retired{output};
    iv::BackgroundEvaluationTransaction transaction{
        graph,
        tock_states,
        coverage,
        propagation,
        data,
        materializations,
        iv::BackgroundTransactionInputs{
            .retired_outputs = retired,
        },
        {},
        {},
    };
    auto result = transaction.execute();
    ASSERT_TRUE(result.has_value()) << result.error();
    ASSERT_EQ(result->status, iv::BackgroundEvaluationStatus::committed);
    ASSERT_TRUE(result->published_data.has_value());
    ASSERT_TRUE(result->promoted_tick_materialization.has_value());

    auto published = reader.pin();
    EXPECT_EQ(published->sample_region_count(), 0u);
    EXPECT_EQ(published->find_sample_region(output, index(2)), nullptr);
}

TEST(BackgroundEvaluationTransaction,
     RetiresInheritedDestinationBeforeApplyingSuccessorWrites)
{
    auto graph = queued_persisted_graph(iv::PortKind::sample);
    iv::ResourceContext resources;
    auto tock_states = graph.tock_state_layout.create_storage(resources);
    tock_states.initialize();
    iv::BackgroundCoverageState coverage{1};
    iv::BackgroundPropagationWorkspace propagation{
        graph.background_evaluation_plan, graph.specialization.sample_rate};
    iv::PersistedDataStore data;
    auto reader = data.register_reader();
    auto const output = data.resolve_output(
        local_output(iv::PortKind::sample, 0));
    auto initial = data.begin_candidate(1);
    initial.overwrite(iv::PersistedSampleRegion{
        .output = output,
        .range = range(2, 2),
        .layout = iv::mono_planar_channel_layout,
        .values = {iv::Sample{10.0f}, iv::Sample{11.0f}},
    });
    ASSERT_EQ(
        data.publish(std::move(initial)),
        iv::PersistedDataPublishResult::published);

    iv::ProducerReserve reserve{iv::realtime_produced_block_storage_size};
    iv::AsyncCapacityManager manager{1};
    iv::AsyncWorkSignal work_signal;
    iv::PendingQueue pending{reserve, work_signal};
    ASSERT_EQ(manager.maintain(reserve, {1, 1, 2}), 2u);
    std::array<iv::Sample, 2> successor_values{40.0f, 41.0f};
    publish_record(
        reserve,
        pending,
        {
            .payload_kind = iv::RealtimeProducedPayloadKind::samples,
            .record_block_count = 1,
            .payload_size = sizeof(successor_values),
            .begin = index(10),
            .sample_count = successor_values.size(),
            .sample_layout = iv::mono_planar_channel_layout,
        },
        std::as_bytes(std::span{successor_values}));

    iv::TickMaterializationStore materializations;
    std::array retired{output};
    std::array routes{iv::BackgroundProducedInputRoute{
        .output = output,
        .port = 0,
        .kind = iv::PortKind::sample,
    }};
    std::array selections{pending.pin()};
    iv::BackgroundEvaluationTransaction transaction{
        graph,
        tock_states,
        coverage,
        propagation,
        data,
        materializations,
        iv::BackgroundTransactionInputs{
            .retired_outputs = retired,
        },
        routes,
        selections,
    };
    auto result = transaction.execute();
    ASSERT_TRUE(result.has_value()) << result.error();
    ASSERT_EQ(result->status, iv::BackgroundEvaluationStatus::committed);

    auto published = reader.pin();
    EXPECT_EQ(published->find_sample_region(output, index(2)), nullptr);
    auto const* successor =
        published->find_sample_region(output, index(10));
    ASSERT_NE(successor, nullptr);
    EXPECT_FLOAT_EQ(successor->at(index(10), 0).value, 40.0f);
    EXPECT_FLOAT_EQ(successor->at(index(11), 0).value, 41.0f);
}

TEST(BackgroundEvaluationTransaction,
     EmptyQueuedEventWindowAuthoritativelyReplacesExistingEvents)
{
    auto graph = queued_persisted_graph(iv::PortKind::event);
    iv::ResourceContext resources;
    auto tock_states = graph.tock_state_layout.create_storage(resources);
    tock_states.initialize();
    iv::BackgroundCoverageState coverage{1};
    iv::BackgroundPropagationWorkspace propagation{
        graph.background_evaluation_plan, graph.specialization.sample_rate};
    iv::PersistedDataStore data;
    auto reader = data.register_reader();
    auto const output = data.resolve_output(
        local_output(iv::PortKind::event, 0));
    auto seed = data.begin_candidate(1);
    seed.overwrite(event_chunk(output, range(0, 4), {1}));
    ASSERT_EQ(data.publish(std::move(seed)),
        iv::PersistedDataPublishResult::published);

    iv::TickMaterializationStore materializations;
    iv::ProducerReserve reserve{iv::realtime_produced_block_storage_size};
    iv::AsyncCapacityManager manager{1};
    iv::AsyncWorkSignal work_signal;
    iv::PendingQueue pending{reserve, work_signal};
    ASSERT_EQ(manager.maintain(reserve, {1, 1, 2}), 2u);
    publish_record(reserve, pending, {
        .payload_kind = iv::RealtimeProducedPayloadKind::events,
        .record_block_count = 1,
        .payload_size = 0,
        .begin = index(0),
        .sample_count = 4,
        .event_type = iv::EventTypeId::trigger,
        .event_count = 0,
    });

    std::array routes{iv::BackgroundProducedInputRoute{
        .output = output,
        .port = 0,
        .kind = iv::PortKind::event,
    }};
    std::array selections{pending.pin()};
    iv::BackgroundEvaluationTransaction transaction{
        graph,
        tock_states,
        coverage,
        propagation,
        data,
        materializations,
        {.semantic_version = 2},
        routes,
        selections,
    };
    auto result = transaction.execute();
    ASSERT_TRUE(result.has_value()) << result.error();
    ASSERT_EQ(result->status, iv::BackgroundEvaluationStatus::committed);

    auto published = reader.pin();
    auto const* emptied = published->find_event_chunk(output, index(1));
    ASSERT_NE(emptied, nullptr);
    EXPECT_EQ(emptied->range, range(0, 4));
    EXPECT_TRUE(emptied->events.empty());
}

TEST(BackgroundEvaluationTransaction,
     QueuedSampleVoidErasesOnlyItsAddressedPersistedRange)
{
    auto graph = queued_persisted_graph(iv::PortKind::sample);
    iv::ResourceContext resources;
    auto tock_states = graph.tock_state_layout.create_storage(resources);
    tock_states.initialize();
    iv::BackgroundCoverageState coverage{1};
    iv::BackgroundPropagationWorkspace propagation{
        graph.background_evaluation_plan, graph.specialization.sample_rate};
    iv::PersistedDataStore data;
    auto reader = data.register_reader();
    auto const output = data.resolve_output(
        local_output(iv::PortKind::sample, 0));
    auto seed = data.begin_candidate(1);
    seed.overwrite(sample_region(output, range(0, 8), 1.0f));
    ASSERT_EQ(data.publish(std::move(seed)),
        iv::PersistedDataPublishResult::published);

    iv::TickMaterializationStore materializations;
    iv::ProducerReserve reserve{iv::realtime_produced_block_storage_size};
    iv::AsyncCapacityManager manager{1};
    iv::AsyncWorkSignal work_signal;
    iv::PendingQueue pending{reserve, work_signal};
    ASSERT_EQ(manager.maintain(reserve, {1, 1, 2}), 2u);
    publish_record(reserve, pending, {
        .payload_kind = iv::RealtimeProducedPayloadKind::void_value,
        .record_block_count = 1,
        .payload_size = 0,
        .begin = index(2),
        .sample_count = 4,
        .sample_layout = iv::mono_planar_channel_layout,
    });

    std::array routes{iv::BackgroundProducedInputRoute{
        .output = output,
        .port = 0,
        .kind = iv::PortKind::sample,
    }};
    std::array selections{pending.pin()};
    iv::BackgroundEvaluationTransaction transaction{
        graph,
        tock_states,
        coverage,
        propagation,
        data,
        materializations,
        {.semantic_version = 2},
        routes,
        selections,
    };
    auto result = transaction.execute();
    ASSERT_TRUE(result.has_value()) << result.error();
    ASSERT_EQ(result->status, iv::BackgroundEvaluationStatus::committed);

    auto published = reader.pin();
    EXPECT_EQ(published->sample_region_count(), 2u);
    EXPECT_NE(published->find_sample_region(output, index(1)), nullptr);
    EXPECT_EQ(published->find_sample_region(output, index(2)), nullptr);
    EXPECT_EQ(published->find_sample_region(output, index(5)), nullptr);
    EXPECT_NE(published->find_sample_region(output, index(6)), nullptr);
}

TEST(BackgroundEvaluationTransaction,
     QueuedEventVoidErasesOnlyItsAddressedPersistedRange)
{
    auto graph = queued_persisted_graph(iv::PortKind::event);
    iv::ResourceContext resources;
    auto tock_states = graph.tock_state_layout.create_storage(resources);
    tock_states.initialize();
    iv::BackgroundCoverageState coverage{1};
    iv::BackgroundPropagationWorkspace propagation{
        graph.background_evaluation_plan, graph.specialization.sample_rate};
    iv::PersistedDataStore data;
    auto reader = data.register_reader();
    auto const output = data.resolve_output(
        local_output(iv::PortKind::event, 0));
    auto seed = data.begin_candidate(1);
    seed.overwrite(event_chunk(output, range(0, 8), {1, 3, 5, 7}));
    ASSERT_EQ(data.publish(std::move(seed)),
        iv::PersistedDataPublishResult::published);

    iv::TickMaterializationStore materializations;
    iv::ProducerReserve reserve{iv::realtime_produced_block_storage_size};
    iv::AsyncCapacityManager manager{1};
    iv::AsyncWorkSignal work_signal;
    iv::PendingQueue pending{reserve, work_signal};
    ASSERT_EQ(manager.maintain(reserve, {1, 1, 2}), 2u);
    publish_record(reserve, pending, {
        .payload_kind = iv::RealtimeProducedPayloadKind::void_value,
        .record_block_count = 1,
        .payload_size = 0,
        .begin = index(2),
        .sample_count = 4,
        .event_type = iv::EventTypeId::trigger,
        .event_count = 0,
    });

    std::array routes{iv::BackgroundProducedInputRoute{
        .output = output,
        .port = 0,
        .kind = iv::PortKind::event,
    }};
    std::array selections{pending.pin()};
    iv::BackgroundEvaluationTransaction transaction{
        graph,
        tock_states,
        coverage,
        propagation,
        data,
        materializations,
        {.semantic_version = 2},
        routes,
        selections,
    };
    auto result = transaction.execute();
    ASSERT_TRUE(result.has_value()) << result.error();
    ASSERT_EQ(result->status, iv::BackgroundEvaluationStatus::committed);

    auto published = reader.pin();
    auto const* prefix = published->find_event_chunk(output, index(1));
    auto const* suffix = published->find_event_chunk(output, index(7));
    ASSERT_NE(prefix, nullptr);
    ASSERT_NE(suffix, nullptr);
    ASSERT_EQ(prefix->events.size(), 1u);
    ASSERT_EQ(suffix->events.size(), 1u);
    EXPECT_EQ(prefix->events[0].time, index(1));
    EXPECT_EQ(suffix->events[0].time, index(7));
    EXPECT_EQ(published->find_event_chunk(output, index(3)), nullptr);
    EXPECT_EQ(published->find_event_chunk(output, index(5)), nullptr);
}

TEST(RealtimePersistedState, CapturesCoherentDataAndMaterializationVersions)
{
    iv::PersistedDataStore data;
    iv::TickMaterializationStore materializations;
    auto const output = data.resolve_output(
        stable_output(iv::PortKind::sample, "samples"));
    auto candidate = data.begin_candidate(2);
    candidate.overwrite(sample_region(output, range(0, 4), 30.0f));
    ASSERT_EQ(data.publish(std::move(candidate)),
        iv::PersistedDataPublishResult::published);
    auto materialization = std::make_unique<iv::TickMaterializationSnapshot>(
        5,
        2,
        iv::PersistedDataVersion{.semantic = 2, .revision = 1},
        std::vector<iv::TickMaterializedSampleInput>{},
        std::vector<iv::TickMaterializedEventInput>{});
    (void)materializations.promote(std::move(materialization));

    auto state = iv::RealtimePersistedState::prepare(
        5, data, materializations);
    ASSERT_TRUE(state->capture_current().has_value());
    EXPECT_EQ(state->data().version(), state->materialization().data_version());
    EXPECT_NE(state->data().find_sample_region(output, index(0)), nullptr);
}

TEST(RealtimePersistedStateMailbox,
     AdoptsOnlyTheNewestCoherentDataAndMaterializationPair)
{
    iv::PersistedDataStore data;
    iv::TickMaterializationStore materializations;
    iv::RealtimePersistedStateMailbox mailbox;
    auto const output = data.resolve_output(
        local_output(iv::PortKind::sample, 0));

    auto initial = iv::RealtimePersistedState::prepare_initial(3, data);
    ASSERT_TRUE(initial->capture_initial());
    mailbox.activate_generation(std::move(initial));
    auto const* initial_active = mailbox.adopt(3);
    ASSERT_NE(initial_active, nullptr);
    EXPECT_EQ(initial_active->data().version(), (iv::PersistedDataVersion{}));
    EXPECT_EQ(initial_active->materialization().generation(), 3u);

    auto publish_pair = [&](std::uint64_t semantic, float first) {
        auto state = iv::RealtimePersistedState::prepare(
            3, data, materializations);
        auto candidate = data.begin_candidate(semantic);
        candidate.overwrite(sample_region(output, range(0, 4), first));
        ASSERT_EQ(data.publish(std::move(candidate)),
            iv::PersistedDataPublishResult::published);
        auto reader = data.register_reader();
        auto pinned = reader.pin();
        (void)materializations.promote(
            std::make_unique<iv::TickMaterializationSnapshot>(
                3,
                semantic,
                pinned->version(),
                std::vector<iv::TickMaterializedSampleInput>{},
                std::vector<iv::TickMaterializedEventInput>{}));
        auto captured = state->capture_current();
        ASSERT_TRUE(captured.has_value()) << captured.error();
        mailbox.publish(std::move(state));
    };

    publish_pair(1, 10.0f);
    publish_pair(2, 20.0f);

    auto const* adopted = mailbox.adopt(3);
    ASSERT_NE(adopted, nullptr);
    EXPECT_EQ(adopted->data().version(),
        (iv::PersistedDataVersion{.semantic = 2, .revision = 2}));
    EXPECT_EQ(
        adopted->materialization().data_version(),
        adopted->data().version());
    auto const* region = adopted->data().find_sample_region(output, index(0));
    ASSERT_NE(region, nullptr);
    EXPECT_FLOAT_EQ(region->at(index(0), 0).value, 20.0f);

    EXPECT_EQ(mailbox.reclaim_returned(), 2u);
    EXPECT_EQ(data.reclaim_retired(), 2u);
    EXPECT_EQ(materializations.reclaim_retired(), 2u);
}

TEST(RealtimePersistedStateMailbox,
     InitialStatePinsDataAtActivationRatherThanPreparation)
{
    iv::PersistedDataStore data;
    auto initial = iv::RealtimePersistedState::prepare_initial(4, data);
    auto const output = data.resolve_output(
        stable_output(iv::PortKind::sample, "late-data"));

    auto candidate = data.begin_candidate(7);
    candidate.overwrite(sample_region(output, range(0, 4), 30.0f));
    ASSERT_EQ(data.publish(std::move(candidate)),
        iv::PersistedDataPublishResult::published);

    ASSERT_TRUE(initial->capture_initial());
    EXPECT_FALSE(initial->capture_initial());
    EXPECT_EQ(initial->data().version(),
        (iv::PersistedDataVersion{.semantic = 7, .revision = 1}));
    auto const* region = initial->data().find_sample_region(output, index(0));
    ASSERT_NE(region, nullptr);
    EXPECT_FLOAT_EQ(region->at(index(0), 0).value, 30.0f);
    EXPECT_EQ(initial->materialization().generation(), 4u);
    EXPECT_EQ(
        initial->materialization().data_version(), initial->data().version());
}

TEST(RealtimePersistedStateMailbox,
     RejectsAStateForAnotherExecutionGeneration)
{
    iv::PersistedDataStore data;
    iv::RealtimePersistedStateMailbox mailbox;
    auto initial = iv::RealtimePersistedState::prepare_initial(3, data);
    ASSERT_TRUE(initial->capture_initial());
    mailbox.activate_generation(std::move(initial));
    auto const* active = mailbox.adopt(3);
    ASSERT_NE(active, nullptr);

    auto successor = iv::RealtimePersistedState::prepare_initial(4, data);
    ASSERT_TRUE(successor->capture_initial());
    mailbox.publish(std::move(successor));
    EXPECT_EQ(mailbox.adopt(3), active);
    EXPECT_EQ(mailbox.reclaim_returned(), 1u);
}

TEST(RealtimePersistedStateMailbox,
     GenerationActivationRejectsLateOldGenerationPublication)
{
    iv::PersistedDataStore data;
    iv::RealtimePersistedStateMailbox mailbox;
    auto capture_initial = [&](std::uint64_t generation) {
        auto state = iv::RealtimePersistedState::prepare_initial(
            generation, data);
        EXPECT_TRUE(state->capture_initial());
        return state;
    };

    mailbox.activate_generation(capture_initial(3));
    mailbox.publish(capture_initial(3));
    mailbox.activate_generation(capture_initial(4));
    mailbox.publish(capture_initial(3));

    auto const* active = mailbox.adopt(4);
    ASSERT_NE(active, nullptr);
    EXPECT_EQ(active->generation(), 4u);
    EXPECT_EQ(mailbox.reclaim_returned(), 3u);
}

} // namespace
