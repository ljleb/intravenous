#include "test_support.h"
#include <intravenous/graph_jit/background_storage_plan.h>

namespace iv::test::graph_jit_tests {

namespace {

std::vector<std::byte> sample_value_bytes(iv::Sample value)
{
    std::vector<std::byte> result(sizeof(value));
    std::memcpy(result.data(), &value, sizeof(value));
    return result;
}

// Completes a hand-built feed-forward SchedulePlan from its execution
// positions, as build_connection_analysis_plan() schedules independent
// primitives without callback batch classes: each positioned bundle is one
// acyclic SccRegionPlan in its own ready group, in position order.
void add_acyclic_test_regions(
    iv::graph_jit::detail::ConnectionAnalysisPlan& connections)
{
    auto& schedule = connections.schedule;
    std::vector<std::pair<std::size_t, iv::NodeBundleHandle>> positioned;
    for (iv::NodeBundleHandle bundle = 0;
         bundle < schedule.bundle_execution_position.size();
         ++bundle) {
        if (auto const position = schedule.bundle_execution_position[bundle]) {
            positioned.emplace_back(*position, bundle);
        }
    }
    std::ranges::sort(positioned);
    schedule.bundle_to_region.resize(schedule.bundle_execution_position.size());
    for (auto const& [position, bundle] : positioned) {
        auto const region = schedule.regions.size();
        schedule.regions.push_back(iv::graph_jit::detail::SccRegionPlan{
            .nodes = {bundle},
            .execution_order = {bundle},
        });
        schedule.region_order.push_back(region);
        schedule.ready_groups.push_back({region});
        schedule.bundle_to_region[bundle] = region;
    }
}

// Gives each listed bundle of a hand-built analysis a stable identity, as an
// authored node has. Only then does its compiler-owned storage have a
// migration identity.
void add_identified_test_nodes(
    iv::graph_jit::detail::ConnectionAnalysisPlan& connections,
    std::initializer_list<iv::NodeBundleHandle> bundles)
{
    for (auto const bundle : bundles) {
        connections.nodes.push_back(iv::graph_jit::detail::PlannedGraphNode{
            .bundle = bundle,
            .stable_identity = iv::graph_jit::StableConcreteNodeId{
                .graph = "test",
                .virtual_node = "node-" + std::to_string(bundle),
            },
        });
    }
}

} // namespace

TEST(GraphJitTransientArenaPlan, PartitionsOneDeadLargeRangeAmongLaterSmallRanges)
{
    using namespace iv::graph_jit::detail;

    std::array requests{
        TransientArenaAllocationRequest{
            .size_bytes = 1024,
            .alignment = 64,
            .live_interval = {.begin = 0, .end = 0},
        },
        TransientArenaAllocationRequest{
            .size_bytes = 256,
            .alignment = 64,
            .live_interval = {.begin = 1, .end = 2},
        },
        TransientArenaAllocationRequest{
            .size_bytes = 256,
            .alignment = 64,
            .live_interval = {.begin = 1, .end = 2},
        },
        TransientArenaAllocationRequest{
            .size_bytes = 256,
            .alignment = 64,
            .live_interval = {.begin = 1, .end = 2},
        },
        TransientArenaAllocationRequest{
            .size_bytes = 256,
            .alignment = 64,
            .live_interval = {.begin = 1, .end = 2},
        },
    };

    auto plan = plan_transient_arena(requests);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());
    ASSERT_EQ(plan->allocations.size(), requests.size());
    EXPECT_EQ(plan->allocations[0].offset, 0u);
    EXPECT_EQ(plan->allocations[1].offset, 0u);
    EXPECT_EQ(plan->allocations[2].offset, 256u);
    EXPECT_EQ(plan->allocations[3].offset, 512u);
    EXPECT_EQ(plan->allocations[4].offset, 768u);
    // The old 1024-byte range is partitioned among the four later values;
    // historical index width does not add another 3 * 256 bytes.
    EXPECT_EQ(plan->size_bytes, 1024u);
}

TEST(GraphJitTransientArenaPlan, ReusesAlignedHoleBetweenStillLiveRanges)
{
    using namespace iv::graph_jit::detail;

    std::array requests{
        TransientArenaAllocationRequest{
            .size_bytes = 16,
            .alignment = 64,
            .live_interval = {.begin = 0, .end = 2},
        },
        TransientArenaAllocationRequest{
            .size_bytes = 16,
            .alignment = 64,
            .live_interval = {.begin = 0, .end = 2},
        },
        TransientArenaAllocationRequest{
            .size_bytes = 32,
            .alignment = 16,
            .live_interval = {.begin = 1, .end = 1},
        },
    };

    auto plan = plan_transient_arena(requests);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());
    ASSERT_EQ(plan->allocations.size(), requests.size());
    EXPECT_EQ(plan->allocations[0].offset, 0u);
    EXPECT_EQ(plan->allocations[1].offset, 64u);
    // The third allocation fits in the alignment hole [16, 64) while both
    // surrounding ranges are still live; a whole-index allocator would append.
    EXPECT_EQ(plan->allocations[2].offset, 16u);
    EXPECT_EQ(plan->size_bytes, 80u);
    EXPECT_EQ(plan->alignment, 64u);
}

TEST(GraphJitTransientArenaPlan, CoalescesAllExpiredByteRangesImplicitly)
{
    using namespace iv::graph_jit::detail;

    std::array requests{
        TransientArenaAllocationRequest{
            .size_bytes = 64,
            .alignment = 64,
            .live_interval = {.begin = 0, .end = 0},
        },
        TransientArenaAllocationRequest{
            .size_bytes = 64,
            .alignment = 64,
            .live_interval = {.begin = 0, .end = 0},
        },
        TransientArenaAllocationRequest{
            .size_bytes = 128,
            .alignment = 64,
            .live_interval = {.begin = 1, .end = 1},
        },
    };

    auto plan = plan_transient_arena(requests);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());
    ASSERT_EQ(plan->allocations.size(), requests.size());
    EXPECT_EQ(plan->allocations[0].offset, 0u);
    EXPECT_EQ(plan->allocations[1].offset, 64u);
    // Both adjacent ranges have expired, so the later 128-byte allocation
    // reuses their combined [0, 128) range without explicit free-list merging.
    EXPECT_EQ(plan->allocations[2].offset, 0u);
    EXPECT_EQ(plan->size_bytes, 128u);
}

TEST(GraphJitTransientArenaPlan, InclusiveLifetimePortsNeverAlias)
{
    using namespace iv::graph_jit::detail;

    std::array requests{
        TransientArenaAllocationRequest{
            .size_bytes = 64,
            .alignment = 16,
            .live_interval = {.begin = 0, .end = 1},
        },
        TransientArenaAllocationRequest{
            .size_bytes = 64,
            .alignment = 16,
            .live_interval = {.begin = 1, .end = 2},
        },
        TransientArenaAllocationRequest{
            .size_bytes = 64,
            .alignment = 16,
            .live_interval = {.begin = 2, .end = 3},
        },
    };

    auto plan = plan_transient_arena(requests);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());
    EXPECT_EQ(plan->allocations[0].offset, 0u);
    EXPECT_EQ(plan->allocations[1].offset, 64u);
    // Request 0 is dead by begin=2 and can be reused, while request 1 still
    // overlaps request 2 at schedule position 2.
    EXPECT_EQ(plan->allocations[2].offset, 0u);
    EXPECT_EQ(plan->size_bytes, 128u);
}

TEST(GraphJitTransientArenaPlan, EqualStartRequestsPlaceLargestFirst)
{
    using namespace iv::graph_jit::detail;

    std::array requests{
        TransientArenaAllocationRequest{
            .size_bytes = 16,
            .alignment = 16,
            .live_interval = {.begin = 0, .end = 1},
        },
        TransientArenaAllocationRequest{
            .size_bytes = 64,
            .alignment = 64,
            .live_interval = {.begin = 0, .end = 1},
        },
    };

    auto plan = plan_transient_arena(requests);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());
    // Size/alignment-first ordering avoids placing the 64-byte range at offset
    // 64 after a small allocation and therefore keeps the high-water mark 80.
    EXPECT_EQ(plan->allocations[1].offset, 0u);
    EXPECT_EQ(plan->allocations[0].offset, 64u);
    EXPECT_EQ(plan->size_bytes, 80u);
}

TEST(GraphJitTransientArenaPlan, IsDeterministicAndRejectsPersistentLifetime)
{
    using namespace iv::graph_jit::detail;

    std::array requests{
        TransientArenaAllocationRequest{
            .size_bytes = 96,
            .alignment = 32,
            .live_interval = {.begin = 3, .end = 5},
        },
        TransientArenaAllocationRequest{
            .size_bytes = 24,
            .alignment = 8,
            .live_interval = {.begin = 0, .end = 2},
        },
        TransientArenaAllocationRequest{
            .size_bytes = 40,
            .alignment = 16,
            .live_interval = {.begin = 3, .end = 4},
        },
    };
    auto first = plan_transient_arena(requests);
    auto second = plan_transient_arena(requests);
    ASSERT_TRUE(first.has_value()) << (first ? std::string{} : first.error());
    ASSERT_TRUE(second.has_value()) << (second ? std::string{} : second.error());
    EXPECT_EQ(*first, *second);

    requests[0].live_interval.crosses_kernel_invocations = true;
    auto persistent = plan_transient_arena(requests);
    ASSERT_FALSE(persistent.has_value());
    EXPECT_NE(persistent.error().find("cross-kernel"), std::string::npos);
}

TEST(GraphJitTransientArenaPlan, RejectsMalformedRequestsAndAcceptsEmptyArena)
{
    using namespace iv::graph_jit::detail;

    std::array<TransientArenaAllocationRequest, 0> empty_requests{};
    auto empty = plan_transient_arena(empty_requests);
    ASSERT_TRUE(empty.has_value()) << (empty ? std::string{} : empty.error());
    EXPECT_TRUE(empty->allocations.empty());
    EXPECT_EQ(empty->size_bytes, 0u);
    EXPECT_EQ(empty->alignment, 1u);

    std::array malformed{
        TransientArenaAllocationRequest{
            .size_bytes = 0,
            .alignment = 8,
            .live_interval = {.begin = 0, .end = 0},
        },
    };
    EXPECT_FALSE(plan_transient_arena(malformed).has_value());

    malformed[0].size_bytes = 8;
    malformed[0].alignment = 3;
    EXPECT_FALSE(plan_transient_arena(malformed).has_value());

    malformed[0].alignment = 8;
    malformed[0].live_interval = {.begin = 2, .end = 1};
    EXPECT_FALSE(plan_transient_arena(malformed).has_value());
}

TEST(GraphJitTransientArenaPlan, ComplexPackingPreservesAllSafetyInvariants)
{
    using namespace iv::graph_jit::detail;

    std::array requests{
        TransientArenaAllocationRequest{64, 64, {.begin = 0, .end = 2}},
        TransientArenaAllocationRequest{24, 8, {.begin = 0, .end = 0}},
        TransientArenaAllocationRequest{48, 16, {.begin = 1, .end = 3}},
        TransientArenaAllocationRequest{80, 32, {.begin = 3, .end = 4}},
        TransientArenaAllocationRequest{16, 16, {.begin = 1, .end = 1}},
        TransientArenaAllocationRequest{96, 32, {.begin = 5, .end = 6}},
        TransientArenaAllocationRequest{32, 8, {.begin = 4, .end = 5}},
    };

    auto plan = plan_transient_arena(requests);
    ASSERT_TRUE(plan.has_value()) << (plan ? std::string{} : plan.error());
    ASSERT_EQ(plan->allocations.size(), requests.size());

    std::size_t observed_high_water = 0;
    for (std::size_t i = 0; i < requests.size(); ++i) {
        auto const& allocation = plan->allocations[i];
        EXPECT_EQ(allocation.size_bytes, requests[i].size_bytes);
        EXPECT_EQ(allocation.alignment, requests[i].alignment);
        EXPECT_EQ(allocation.offset % allocation.alignment, 0u);
        ASSERT_LE(allocation.offset, plan->size_bytes);
        ASSERT_LE(allocation.size_bytes, plan->size_bytes - allocation.offset);
        observed_high_water = std::max(
            observed_high_water, allocation.offset + allocation.size_bytes);

        for (std::size_t j = i + 1; j < requests.size(); ++j) {
            auto const& a_live = requests[i].live_interval;
            auto const& b_live = requests[j].live_interval;
            auto const lifetimes_overlap =
                !(a_live.end < b_live.begin || b_live.end < a_live.begin);
            if (!lifetimes_overlap) continue;

            auto const& other = plan->allocations[j];
            auto const byte_ranges_overlap =
                !(allocation.offset + allocation.size_bytes <= other.offset
                    || other.offset + other.size_bytes <= allocation.offset);
            EXPECT_FALSE(byte_ranges_overlap)
                << "overlapping lifetimes " << i << " and " << j
                << " were assigned overlapping arena bytes";
        }
    }
    EXPECT_EQ(plan->size_bytes, observed_high_water);
}

TEST(GraphJitSampleStoragePlan, PacksExactTransientByteRangesAcrossLifetimes)
{
    using namespace iv::graph_jit::detail;

    iv::ChannelLayout const mono{
        .channel_type = iv::ChannelTypeId::mono,
        .sample_layout = iv::SampleStreamLayout::planar,
    };
    iv::ChannelLayout const stereo{
        .channel_type = iv::ChannelTypeId::stereo,
        .sample_layout = iv::SampleStreamLayout::interleaved,
    };
    auto group = [](iv::ChannelLayout layout,
                    std::size_t begin,
                    std::size_t end) {
        SampleProducerGroupPlan result;
        result.canonical_source_layout = layout;
        result.has_realtime_connections = true;
        result.storage_plan = iv::SampleConnectionStoragePlan{
            iv::RealtimeBufferStorageKind::transient_stack};
        result.live_interval = ConnectionLiveIntervalPlan{
            .begin = begin,
            .end = end,
        };
        return result;
    };

    ConnectionAnalysisPlan connections;
    // A large early value, followed by two simultaneous half-sized values. The
    // latter should split the dead large range instead of reserving one large
    // historical index plus a second small index.
    connections.sample_producer_groups.push_back(group(stereo, 0, 0));
    connections.sample_producer_groups.push_back(group(mono, 1, 2));
    connections.sample_producer_groups.push_back(group(mono, 1, 2));

    auto storage = build_sample_storage_plan(connections, 64);
    ASSERT_TRUE(storage.has_value())
        << (storage ? std::string{} : storage.error());
    ASSERT_EQ(storage->representations.size(), 3u);
    ASSERT_EQ(storage->transient_allocations.size(), 3u);

    for (auto const& producer : storage->producer_groups) {
        ASSERT_TRUE(producer.has_value());
        ASSERT_LT(
            producer->canonical_representation,
            storage->representations.size());
        auto const allocation = storage->representations[
            producer->canonical_representation].transient_allocation;
        ASSERT_NE(allocation, no_sample_transient_allocation);
        ASSERT_LT(allocation, storage->transient_allocations.size());
    }
    auto allocation_for_group = [&](std::size_t group_index)
        -> SampleTransientAllocationPlan const& {
        auto const representation_index =
            storage->producer_groups[group_index]->canonical_representation;
        auto const allocation =
            storage->representations[representation_index].transient_allocation;
        return storage->transient_allocations[allocation];
    };
    auto const& large = allocation_for_group(0);
    auto const& small_a = allocation_for_group(1);
    auto const& small_b = allocation_for_group(2);
    auto const mono_bytes = 64u * sizeof(iv::Sample);
    auto const stereo_bytes = 2u * mono_bytes;

    EXPECT_EQ(large.size_bytes, stereo_bytes);
    EXPECT_EQ(large.region_relative_offset, 0u);
    EXPECT_EQ(small_a.size_bytes, mono_bytes);
    EXPECT_EQ(small_b.size_bytes, mono_bytes);
    EXPECT_EQ(small_a.region_relative_offset, 0u);
    EXPECT_EQ(small_b.region_relative_offset, mono_bytes);
    EXPECT_EQ(storage->transient_arena_size, stereo_bytes);

    iv::NodeLayoutBuilder builder(64);
    auto declared = declare_sample_storage(builder, *storage);
    ASSERT_TRUE(declared.has_value())
        << (declared ? std::string{} : declared.error());
    auto layout = std::move(builder).build();
    auto finalized = finalize_sample_storage(layout, *storage);
    ASSERT_TRUE(finalized.has_value())
        << (finalized ? std::string{} : finalized.error());
    EXPECT_TRUE(storage->persistent_allocations.empty());
    ASSERT_EQ(storage->sequential_pace_cursors.size(), 1u);
    EXPECT_EQ(layout.regions.size(), storage->sequential_pace_cursors.size());
    EXPECT_EQ(storage->sequential_pace_cursors[0].absolute_pace_exponent, 0);
    ASSERT_TRUE(storage->sequential_pace_cursors[0].region.valid());
    ASSERT_LT(
        storage->sequential_pace_cursors[0].region.index,
        layout.regions.size());
}

TEST(GraphJitSampleStoragePlan, SharesOneProducerBetweenRealtimeAndReplayConnections)
{
    using namespace iv::graph_jit::detail;

    iv::ChannelLayout const mono{
        .channel_type = iv::ChannelTypeId::mono,
        .sample_layout = iv::SampleStreamLayout::planar,
    };
    iv::NodeBundlePortId const source_port{1, iv::PortKind::sample, 0};
    iv::SampleOutputChannelId const source_channel{1, 0, 0};
    ConnectionAnalysisPlan connections;
    SampleConnectionPlan realtime;
    realtime.source_channels = {source_channel};
    realtime.source_channel_timings = {SampleSourceChannelTimingPlan{
        .source = source_channel, .source_layout = mono}};
    realtime.canonical_source_port = source_port;
    realtime.canonical_source_layout = mono;
    realtime.target_port = {2, iv::PortKind::sample, 0};
    realtime.target_layout = mono;
    connections.sample_connections.push_back(realtime);
    auto replay = realtime;
    replay.target_port = {3, iv::PortKind::sample, 0};
    replay.destination_access = PlannedDestinationAccess::random_access;
    replay.source_channel_timings[0].destination_access = PlannedDestinationAccess::random_access;
    replay.source_channel_timings[0].delivery = PlannedDeliveryMechanism::replayed_tick_to_random_access;
    replay.source_channel_timings[0].contextually_replayable = true;
    connections.sample_connections.push_back(replay);
    connections.schedule.bundle_execution_position.resize(4);
    connections.schedule.bundle_execution_position[1] = 0;
    connections.schedule.bundle_execution_position[2] = 1;

    SampleProducerGroupPlan group;
    group.source_port = source_port;
    group.source_channels = {source_channel};
    group.canonical_source_layout = mono;
    group.connection_indices = {0, 1};
    group.has_realtime_connections = true;
    group.has_background_connections = true;
    group.storage_plan = iv::SampleConnectionStoragePlan{
        iv::RealtimeBufferStorageKind::transient_stack};
    group.live_interval = {.begin = 0, .end = 1};
    connections.sample_producer_groups.push_back(group);

    auto storage = build_sample_storage_plan(connections, 64);
    ASSERT_TRUE(storage.has_value()) << storage.error();
    ASSERT_EQ(storage->connection_representations.size(), 2u);
    EXPECT_TRUE(storage->connection_representations[0].has_value());
    EXPECT_FALSE(storage->connection_representations[1].has_value());
    ASSERT_EQ(storage->producer_groups.size(), 1u);
    EXPECT_TRUE(storage->producer_groups[0].has_value());
    EXPECT_EQ(storage->representations.size(), 1u);
    EXPECT_TRUE(storage->persistent_allocations.empty());
    EXPECT_TRUE(storage->materializations.empty());

    // A background connection from a different producer remains invalid.
    connections.sample_connections[1].source_channel_timings[0].source = {4, 0, 0};
    auto invalid = build_sample_storage_plan(connections, 64);
    ASSERT_FALSE(invalid.has_value());
    EXPECT_NE(invalid.error().find("foreign source port"), std::string::npos);
}

TEST(GraphJitSampleStoragePlan, LeavesBackgroundAccessBranchesUnresolved)
{
    using namespace iv::graph_jit::detail;

    ConnectionAnalysisPlan connections;
    SampleConnectionPlan realtime;
    realtime.source_channel_timings = {SampleSourceChannelTimingPlan{}};
    realtime.target_layout = iv::ChannelLayout{
        .channel_type = iv::ChannelTypeId::mono,
        .sample_layout = iv::SampleStreamLayout::planar,
    };
    connections.sample_connections.push_back(std::move(realtime));
    SampleConnectionPlan background;
    background.destination_access = PlannedDestinationAccess::random_access;
    background.source_channel_timings = {SampleSourceChannelTimingPlan{
        .destination_access = PlannedDestinationAccess::random_access,
        .delivery = PlannedDeliveryMechanism::tock_to_random_access,
    }};
    connections.sample_connections.push_back(std::move(background));

    SampleProducerGroupPlan realtime_group;
    realtime_group.canonical_source_layout = iv::ChannelLayout{
        .channel_type = iv::ChannelTypeId::mono,
        .sample_layout = iv::SampleStreamLayout::planar,
    };
    realtime_group.connection_indices = {0};
    realtime_group.has_realtime_connections = true;
    realtime_group.storage_plan = iv::SampleConnectionStoragePlan{
        iv::RealtimeBufferStorageKind::transient_stack};
    realtime_group.live_interval = ConnectionLiveIntervalPlan{
        .begin = 0,
        .end = 1,
    };
    connections.sample_producer_groups.push_back(std::move(realtime_group));

    SampleProducerGroupPlan background_group;
    background_group.connection_indices = {1};
    background_group.has_background_connections = true;
    connections.sample_producer_groups.push_back(std::move(background_group));

    auto storage = build_sample_storage_plan(connections, 64);
    ASSERT_TRUE(storage.has_value())
        << (storage ? std::string{} : storage.error());
    ASSERT_EQ(storage->connection_representations.size(), 2u);
    ASSERT_TRUE(storage->connection_representations[0].has_value());
    EXPECT_FALSE(storage->connection_representations[1].has_value());
    ASSERT_EQ(storage->producer_groups.size(), 2u);
    ASSERT_TRUE(storage->producer_groups[0].has_value());
    EXPECT_FALSE(storage->producer_groups[1].has_value());
    EXPECT_EQ(
        *storage->connection_representations[0],
        storage->producer_groups[0]->canonical_representation);
}

TEST(GraphJitSampleStoragePlan, AliasesLayoutAndMonoToStereoConvertedFanout)
{
    using namespace iv::graph_jit::detail;

    iv::ChannelLayout const mono{
        .channel_type = iv::ChannelTypeId::mono,
        .sample_layout = iv::SampleStreamLayout::planar,
    };
    iv::ChannelLayout const stereo_interleaved{
        .channel_type = iv::ChannelTypeId::stereo,
        .sample_layout = iv::SampleStreamLayout::interleaved,
    };

    ConnectionAnalysisPlan connections;
    SampleConnectionPlan identity;
    identity.source_channel_timings = {SampleSourceChannelTimingPlan{}};
    identity.canonical_source_layout = mono;
    identity.target_layout = mono;
    identity.target_port = iv::NodeBundlePortId{2, iv::PortKind::sample, 0};
    connections.sample_connections.push_back(identity);

    SampleConnectionPlan converted_a;
    converted_a.source_channel_timings = {SampleSourceChannelTimingPlan{}};
    converted_a.canonical_source_layout = mono;
    converted_a.target_layout = stereo_interleaved;
    converted_a.target_port = iv::NodeBundlePortId{3, iv::PortKind::sample, 0};
    converted_a.requires_conversion = true;
    connections.sample_connections.push_back(converted_a);

    auto converted_b = converted_a;
    converted_b.target_port = iv::NodeBundlePortId{4, iv::PortKind::sample, 0};
    connections.sample_connections.push_back(converted_b);

    connections.schedule.bundle_execution_position.resize(5);
    connections.schedule.bundle_execution_position[1] = 0;
    connections.schedule.bundle_execution_position[2] = 1;
    connections.schedule.bundle_execution_position[3] = 2;
    connections.schedule.bundle_execution_position[4] = 3;

    SampleProducerGroupPlan group;
    group.canonical_source_layout = mono;
    group.connection_indices = {0, 1, 2};
    group.has_realtime_connections = true;
    group.storage_plan = iv::SampleConnectionStoragePlan{
        iv::RealtimeBufferStorageKind::transient_stack};
    group.live_interval = ConnectionLiveIntervalPlan{
        .begin = 0,
        .end = 3,
    };
    connections.sample_producer_groups.push_back(std::move(group));

    auto storage = build_sample_storage_plan(connections, 64);
    ASSERT_TRUE(storage.has_value())
        << (storage ? std::string{} : storage.error());
    ASSERT_EQ(storage->representations.size(), 1u);
    EXPECT_TRUE(storage->materializations.empty());
    ASSERT_EQ(storage->connection_representations.size(), 3u);
    ASSERT_EQ(storage->connection_channel_bindings.size(), 3u);
    ASSERT_TRUE(storage->producer_groups[0].has_value());
    auto const canonical =
        storage->producer_groups[0]->canonical_representation;
    ASSERT_TRUE(storage->connection_representations[0].has_value());
    EXPECT_EQ(*storage->connection_representations[0], canonical);
    EXPECT_FALSE(storage->connection_representations[1].has_value());
    EXPECT_FALSE(storage->connection_representations[2].has_value());
    ASSERT_TRUE(storage->connection_channel_bindings[1].has_value());
    ASSERT_TRUE(storage->connection_channel_bindings[2].has_value());
    for (auto connection_index : {1u, 2u}) {
        auto const& channels =
            *storage->connection_channel_bindings[connection_index];
        ASSERT_EQ(channels.size(), 2u);
        for (auto const& channel : channels) {
            EXPECT_EQ(channel.representation, canonical);
            EXPECT_EQ(channel.representation_channel, 0u);
            EXPECT_EQ(channel.frame_delay, 0u);
        }
    }
    EXPECT_TRUE(storage->representations[canonical]
                    .canonical_producer_representation);
    EXPECT_EQ(storage->representations[canonical].channel_layout, mono);
    EXPECT_EQ(storage->representations[canonical].live_interval.begin, 0u);
    EXPECT_EQ(storage->representations[canonical].live_interval.end, 3u);

    auto const canonical_allocation = storage->representations[canonical]
        .transient_allocation;
    ASSERT_LT(canonical_allocation, storage->transient_allocations.size());
    EXPECT_EQ(storage->transient_allocations[canonical_allocation].size_bytes,
        64u * sizeof(iv::Sample));
    EXPECT_EQ(storage->transient_arena_size, 64u * sizeof(iv::Sample));
}

TEST(GraphJitSampleStoragePlan, RealizesCompactPersistentCarryExactly)
{
    using namespace iv::graph_jit::detail;

    iv::ChannelLayout const mono{
        .channel_type = iv::ChannelTypeId::mono,
        .sample_layout = iv::SampleStreamLayout::planar,
    };
    ConnectionAnalysisPlan connections;
    SampleProducerGroupPlan group;
    group.canonical_source_layout = mono;
    group.has_realtime_connections = true;
    group.storage_requirements = iv::SampleConnectionStorageRequirements{
        .current_block_frames = 64,
        .retained_frames = 7,
        .channel_count = 1,
    };
    group.storage_plan = iv::SampleConnectionStoragePlan{
        iv::RealtimeBufferStorageKind::stack_with_persistent_carry};
    group.live_interval = ConnectionLiveIntervalPlan{
        .begin = 0,
        .end = 1,
        .crosses_kernel_invocations = true,
    };
    connections.sample_producer_groups.push_back(group);
    // The producer's carry runs at its first live position.
    connections.schedule.bundle_execution_position.resize(1);
    connections.schedule.bundle_execution_position[0] = 0;
    add_acyclic_test_regions(connections);

    auto storage = build_sample_storage_plan(connections, 64);
    ASSERT_TRUE(storage.has_value())
        << (storage ? std::string{} : storage.error());
    ASSERT_EQ(storage->representations.size(), 1u);
    ASSERT_EQ(storage->transient_allocations.size(), 1u);
    ASSERT_EQ(storage->persistent_allocations.size(), 1u);
    ASSERT_EQ(storage->carry_operations.size(), 1u);
    EXPECT_FALSE(storage->carry_operations[0].cyclic_region_scope);
    ASSERT_TRUE(storage->producer_groups[0].has_value());
    auto const representation_index =
        storage->producer_groups[0]->canonical_representation;
    auto const& representation = storage->representations[representation_index];
    EXPECT_EQ(representation.frame_capacity, 128u);
    EXPECT_NE(
        representation.transient_allocation,
        no_sample_transient_allocation);
    EXPECT_NE(
        representation.persistent_allocation,
        no_sample_persistent_allocation);

    auto const& persistent = storage->persistent_allocations[
        representation.persistent_allocation];
    EXPECT_EQ(persistent.kind, SamplePersistentStorageKind::compact_carry);
    EXPECT_EQ(persistent.retained_frames, 7u);
    EXPECT_EQ(persistent.frame_capacity, 128u);
    EXPECT_EQ(persistent.size_bytes, 7u * sizeof(iv::Sample));
    EXPECT_FALSE(persistent.migration_identity.empty());
    EXPECT_EQ(
        storage->carry_operations[0].persistent_allocation,
        representation.persistent_allocation);
    EXPECT_EQ(storage->carry_operations[0].retained_frames, 7u);

    iv::NodeLayoutBuilder builder(64);
    auto declared = declare_sample_storage(builder, *storage);
    ASSERT_TRUE(declared.has_value())
        << (declared ? std::string{} : declared.error());
    auto layout = std::move(builder).build();
    auto finalized = finalize_sample_storage(layout, *storage);
    ASSERT_TRUE(finalized.has_value())
        << (finalized ? std::string{} : finalized.error());
    ASSERT_EQ(layout.regions.size(), 2u);
    auto const& persistent_region = layout.regions[persistent.region.index];
    EXPECT_EQ(
        persistent_region.migration_identity,
        persistent.migration_identity);
    EXPECT_EQ(persistent_region.size, 7u * sizeof(iv::Sample));
}

TEST(GraphJitSampleStoragePlan,
    CyclicRegionCarriesStayLiveAcrossTheirBatchedReadyGroup)
{
    using namespace iv::graph_jit::detail;

    ConnectionAnalysisPlan connections;
    connections.nodes.resize(2);
    connections.schedule.regions = {
        SccRegionPlan{
            .nodes = {0},
            .execution_order = {0},
            .cyclic = true,
            .maximum_block_size = 1,
            .scc_feedback_latency = 1,
        },
        SccRegionPlan{
            .nodes = {1},
            .execution_order = {1},
            .cyclic = true,
            .maximum_block_size = 1,
            .scc_feedback_latency = 1,
        },
    };
    connections.schedule.region_order = {0, 1};
    connections.schedule.ready_groups = {{0, 1}};
    connections.schedule.bundle_to_region = {0, 1};
    connections.schedule.bundle_execution_position = {0, 1};

    iv::ChannelLayout const mono{
        .channel_type = iv::ChannelTypeId::mono,
        .sample_layout = iv::SampleStreamLayout::planar,
    };
    std::array const sinks{
        SampleSinkStorageRequest{
            .channel_layout = mono,
            .history = 1,
            .execution_position = 0,
            .migration_identity = "cyclic-carry-a",
        },
        SampleSinkStorageRequest{
            .channel_layout = mono,
            .history = 1,
            .execution_position = 1,
            .migration_identity = "cyclic-carry-b",
        },
    };

    auto storage = build_sample_storage_plan(connections, 64, sinks);
    ASSERT_TRUE(storage.has_value())
        << (storage ? std::string{} : storage.error());
    ASSERT_EQ(storage->carry_operations.size(), 2u);
    ASSERT_EQ(storage->transient_allocations.size(), 2u);
    for (std::size_t index = 0; index < 2; ++index) {
        ASSERT_TRUE(storage->carry_operations[index].cyclic_region_scope);
        EXPECT_EQ(*storage->carry_operations[index].cyclic_region_scope, index);
        auto const representation = storage->carry_operations[index]
            .representation_index;
        ASSERT_LT(representation, storage->representations.size());
        EXPECT_EQ(storage->representations[representation].live_interval.begin, 0u);
        EXPECT_EQ(storage->representations[representation].live_interval.end, 1u);
    }
    auto const& first = storage->transient_allocations[0];
    auto const& second = storage->transient_allocations[1];
    EXPECT_TRUE(
        first.region_relative_offset + first.size_bytes
            <= second.region_relative_offset
        || second.region_relative_offset + second.size_bytes
            <= first.region_relative_offset);
}

TEST(GraphJitSampleStoragePlan, RealizesLargeRetentionAsPersistentRing)
{
    using namespace iv::graph_jit::detail;

    iv::ChannelLayout const mono{
        .channel_type = iv::ChannelTypeId::mono,
        .sample_layout = iv::SampleStreamLayout::planar,
    };
    ConnectionAnalysisPlan connections;
    SampleProducerGroupPlan group;
    group.canonical_source_layout = mono;
    group.has_realtime_connections = true;
    group.storage_requirements = iv::SampleConnectionStorageRequirements{
        .current_block_frames = 64,
        .retained_frames = 5000,
        .channel_count = 1,
    };
    group.storage_plan = iv::SampleConnectionStoragePlan{
        iv::RealtimeBufferStorageKind::full_node_storage};
    group.live_interval = ConnectionLiveIntervalPlan{
        .begin = 0,
        .end = 1,
        .crosses_kernel_invocations = true,
    };
    connections.sample_producer_groups.push_back(group);

    auto first = build_sample_storage_plan(connections, 64);
    auto second = build_sample_storage_plan(connections, 64);
    ASSERT_TRUE(first.has_value())
        << (first ? std::string{} : first.error());
    ASSERT_TRUE(second.has_value())
        << (second ? std::string{} : second.error());
    ASSERT_EQ(first->representations.size(), 1u);
    ASSERT_TRUE(first->transient_allocations.empty());
    ASSERT_EQ(first->persistent_allocations.size(), 1u);
    EXPECT_TRUE(first->carry_operations.empty());
    auto const representation_index =
        first->producer_groups[0]->canonical_representation;
    auto const& storage = first->representations[representation_index];
    EXPECT_EQ(storage.frame_capacity, 8192u);
    EXPECT_EQ(
        storage.transient_allocation,
        no_sample_transient_allocation);
    ASSERT_NE(
        storage.persistent_allocation,
        no_sample_persistent_allocation);
    auto const& persistent = first->persistent_allocations[
        storage.persistent_allocation];
    EXPECT_EQ(persistent.kind, SamplePersistentStorageKind::ring);
    EXPECT_EQ(persistent.retained_frames, 5000u);
    EXPECT_EQ(persistent.frame_capacity, 8192u);
    EXPECT_EQ(persistent.size_bytes, 8192u * sizeof(iv::Sample));
    EXPECT_EQ(
        persistent.migration_identity,
        second->persistent_allocations[0].migration_identity);

    iv::NodeLayoutBuilder builder(64);
    auto declared = declare_sample_storage(builder, *first);
    ASSERT_TRUE(declared.has_value())
        << (declared ? std::string{} : declared.error());
    auto layout = std::move(builder).build();
    auto finalized = finalize_sample_storage(layout, *first);
    ASSERT_TRUE(finalized.has_value())
        << (finalized ? std::string{} : finalized.error());
    ASSERT_EQ(layout.regions.size(), 2u);
    auto const& persistent_region = layout.regions[persistent.region.index];
    EXPECT_EQ(persistent_region.size, 8192u * sizeof(iv::Sample));
    EXPECT_FALSE(persistent_region.migration_identity.empty());
}

TEST(GraphJitSampleStoragePlan, RealizesDetachedBranchAsPersistentFeedbackRing)
{
    using namespace iv::graph_jit::detail;

    iv::ChannelLayout const mono{
        .channel_type = iv::ChannelTypeId::mono,
        .sample_layout = iv::SampleStreamLayout::planar,
    };
    iv::SampleOutputChannelId const source_channel{
        .bundle = 1,
        .port = 0,
        .channel = 0,
    };
    iv::NodeBundlePortId const source_port{1, iv::PortKind::sample, 0};

    ConnectionAnalysisPlan connections;
    add_identified_test_nodes(connections, {1, 2});
    connections.schedule.bundle_execution_position.resize(3);
    connections.schedule.bundle_execution_position[1] = 0;
    connections.schedule.bundle_execution_position[2] = 1;

    SampleConnectionPlan feedback;
    feedback.source_type = iv::ChannelTypeId::mono;
    feedback.source_channels = {source_channel};
    feedback.source_channel_timings = {SampleSourceChannelTimingPlan{
        .source = source_channel,
        .source_layout = mono,
        .source_latency = 2,
        .read_latency = 2,
    }};
    feedback.canonical_source_port = source_port;
    feedback.canonical_source_layout = mono;
    feedback.target_type = iv::ChannelTypeId::mono;
    feedback.target_layout = mono;
    feedback.target_channels = {iv::SampleInputChannelId{
        .bundle = 2,
        .port = 0,
        .channel = 0,
    }};
    feedback.target_port = iv::NodeBundlePortId{2, iv::PortKind::sample, 0};
    feedback.source_latency = 2;
    feedback.read_latency = 2;
    feedback.target_history = 3;
    feedback.detach = iv::ConfiguredSampleConnectionDetach{
        .loop_extra_latency = 5,
        .initial_value_override = iv::Sample{0.25f},
    };
    feedback.detach_initial_value = sample_value_bytes(iv::Sample{0.25f});
    connections.sample_connections.push_back(feedback);

    SampleProducerGroupPlan group;
    group.source_port = source_port;
    group.source_type = iv::ChannelTypeId::mono;
    group.source_channels = {source_channel};
    group.canonical_source_layout = mono;
    group.connection_indices = {0};
    group.has_realtime_connections = true;
    group.storage_plan = iv::SampleConnectionStoragePlan{
        iv::RealtimeBufferStorageKind::transient_stack};
    group.live_interval = ConnectionLiveIntervalPlan{
        .begin = 0,
        .end = 1,
        .crosses_kernel_invocations = true,
    };
    connections.sample_producer_groups.push_back(group);

    auto storage = build_sample_storage_plan(connections, 8);
    ASSERT_TRUE(storage.has_value())
        << (storage ? std::string{} : storage.error());
    ASSERT_EQ(storage->representations.size(), 2u);
    ASSERT_EQ(storage->transient_allocations.size(), 1u);
    ASSERT_EQ(storage->persistent_allocations.size(), 1u);
    ASSERT_EQ(storage->feedback_timelines.size(), 1u);
    ASSERT_EQ(storage->producer_authored_extents.size(), 1u);
    EXPECT_EQ(
        storage->producer_authored_extents.front().maximum_revision_frames,
        2u);
    ASSERT_TRUE(storage->connection_representations[0].has_value());

    auto const canonical = storage->producer_groups[0]->canonical_representation;
    auto const ring = *storage->connection_representations[0];
    ASSERT_NE(canonical, ring);
    EXPECT_EQ(
        storage->representations[canonical].storage,
        iv::RealtimeBufferStorageKind::transient_stack);
    EXPECT_EQ(
        storage->representations[ring].storage,
        iv::RealtimeBufferStorageKind::full_node_storage);
    EXPECT_EQ(storage->representations[ring].frame_capacity, 32u);
    EXPECT_EQ(
        storage->representations[ring].transient_allocation,
        no_sample_transient_allocation);
    ASSERT_NE(
        storage->representations[ring].persistent_allocation,
        no_sample_persistent_allocation);

    auto const& persistent = storage->persistent_allocations[
        storage->representations[ring].persistent_allocation];
    EXPECT_EQ(persistent.kind, SamplePersistentStorageKind::ring);
    EXPECT_EQ(persistent.retained_frames, 10u);
    EXPECT_EQ(persistent.frame_capacity, 32u);
    EXPECT_EQ(persistent.size_bytes, 32u * sizeof(iv::Sample));
    ASSERT_TRUE(persistent.initialize_value.has_value());
    EXPECT_FLOAT_EQ(
        static_cast<float>(sample_value_from_bytes(
            *persistent.initialize_value)),
        0.25f);
    EXPECT_NE(
        persistent.migration_identity.find("graphjit.sample.feedback:"),
        std::string::npos);

    auto const& timeline = storage->feedback_timelines[0];
    EXPECT_EQ(timeline.connection_index, 0u);
    EXPECT_EQ(timeline.timeline_representation, ring);
    EXPECT_EQ(timeline.channel_layout, mono);
    EXPECT_EQ(timeline.retained_frames, 10u);
    EXPECT_EQ(timeline.loop_extra_latency, 5u);
    EXPECT_FLOAT_EQ(
        static_cast<float>(sample_value_from_bytes(timeline.initial_value)),
        0.25f);
    EXPECT_EQ(timeline.writer.kind, SampleFeedbackTimelineWriterKind::copy);
    EXPECT_EQ(timeline.writer.revision_frames, 2u);
    EXPECT_EQ(timeline.writer.source_representation, canonical);
    EXPECT_EQ(timeline.writer.after_execution_position, 0u);
    EXPECT_TRUE(timeline.writer.composition_contributions.empty());
    EXPECT_EQ(
        timeline.storage_plan.candidate_costs.full_node_storage.copied_bytes,
        10u * sizeof(iv::Sample));
    EXPECT_EQ(
        timeline.storage_plan.candidate_costs.full_node_storage
            .ring_addressed_bytes,
        10u * sizeof(iv::Sample));

    iv::NodeLayoutBuilder builder(8);
    auto declared = declare_sample_storage(builder, *storage);
    ASSERT_TRUE(declared.has_value())
        << (declared ? std::string{} : declared.error());
    auto layout = std::move(builder).build();
    auto finalized = finalize_sample_storage(layout, *storage);
    ASSERT_TRUE(finalized.has_value())
        << (finalized ? std::string{} : finalized.error());
    ASSERT_EQ(layout.regions.size(), 3u);
    auto const& persistent_region = layout.regions[persistent.region.index];
    EXPECT_EQ(persistent_region.size, 32u * sizeof(iv::Sample));
    EXPECT_EQ(
        persistent_region.migration_identity,
        persistent.migration_identity);
    ASSERT_NE(persistent_region.raw_initialize_fn, nullptr);
    EXPECT_EQ(persistent_region.raw_initialize_data.size(), sizeof(iv::Sample));

    iv::ResourceContext resources;
    auto node_storage = layout.create_storage(resources);
    node_storage.initialize();
    auto const bytes = node_storage.region_bytes(persistent.region);
    ASSERT_EQ(bytes.size(), 32u * sizeof(iv::Sample));
    auto const initialized = std::span<iv::Sample const>{
        reinterpret_cast<iv::Sample const*>(bytes.data()),
        bytes.size() / sizeof(iv::Sample)};
    for (auto const sample : initialized) {
        EXPECT_FLOAT_EQ(static_cast<float>(sample), 0.25f);
    }
}

TEST(GraphJitSampleStoragePlan, ZeroInitializedFeedbackUsesProducerHomeAndCopiesOnlyIncompatibleBranches)
{
    using namespace iv::graph_jit::detail;

    iv::ChannelLayout const mono{
        .channel_type = iv::ChannelTypeId::mono,
        .sample_layout = iv::SampleStreamLayout::planar,
    };
    iv::SampleOutputChannelId const source_channel{
        .bundle = 1,
        .port = 0,
        .channel = 0,
    };
    iv::NodeBundlePortId const source_port{1, iv::PortKind::sample, 0};

    ConnectionAnalysisPlan connections;
    add_identified_test_nodes(connections, {1, 2, 3, 4});
    connections.schedule.bundle_execution_position.resize(5);
    connections.schedule.bundle_execution_position[1] = 0;
    connections.schedule.bundle_execution_position[2] = 1;
    connections.schedule.bundle_execution_position[3] = 2;
    connections.schedule.bundle_execution_position[4] = 3;
    add_acyclic_test_regions(connections);

    auto make_feedback = [&](std::size_t target_bundle,
                             std::size_t latency,
                             iv::Sample initial_value) {
        SampleConnectionPlan feedback;
        feedback.source_type = iv::ChannelTypeId::mono;
        feedback.source_channels = {source_channel};
        feedback.source_channel_timings = {SampleSourceChannelTimingPlan{
            .source = source_channel,
            .source_layout = mono,
        }};
        feedback.canonical_source_port = source_port;
        feedback.canonical_source_layout = mono;
        feedback.target_type = iv::ChannelTypeId::mono;
        feedback.target_layout = mono;
        feedback.target_channels = {iv::SampleInputChannelId{
            .bundle = target_bundle,
            .port = 0,
            .channel = 0,
        }};
        feedback.target_port = iv::NodeBundlePortId{
            target_bundle, iv::PortKind::sample, 0};
        feedback.detach = iv::ConfiguredSampleConnectionDetach{
            .loop_extra_latency = latency,
            .initial_value_override = initial_value,
        };
        feedback.detach_initial_value = sample_value_bytes(initial_value);
        return feedback;
    };

    connections.sample_connections.push_back(
        make_feedback(2, 5, iv::Sample{0.0f}));
    connections.sample_connections.push_back(
        make_feedback(3, 7, iv::Sample{0.5f}));
    connections.sample_connections.push_back(
        make_feedback(4, 12, iv::Sample{0.0f}));

    SampleProducerGroupPlan group;
    group.source_port = source_port;
    group.source_type = iv::ChannelTypeId::mono;
    group.source_channels = {source_channel};
    group.canonical_source_layout = mono;
    group.connection_indices = {0, 1, 2};
    group.has_realtime_connections = true;
    group.storage_plan = iv::SampleConnectionStoragePlan{
        iv::RealtimeBufferStorageKind::transient_stack};
    group.live_interval = ConnectionLiveIntervalPlan{
        .begin = 0,
        .end = 3,
        .crosses_kernel_invocations = true,
    };
    connections.sample_producer_groups.push_back(group);

    auto storage = build_sample_storage_plan(connections, 8);
    ASSERT_TRUE(storage.has_value())
        << (storage ? std::string{} : storage.error());
    ASSERT_EQ(storage->representations.size(), 2u);
    ASSERT_EQ(storage->transient_allocations.size(), 1u);
    ASSERT_EQ(storage->persistent_allocations.size(), 2u);
    ASSERT_EQ(storage->feedback_timelines.size(), 3u);

    auto const canonical =
        storage->producer_groups[0]->canonical_representation;
    ASSERT_LT(canonical, storage->representations.size());
    EXPECT_TRUE(
        storage->representations[canonical].canonical_producer_representation);
    EXPECT_EQ(
        storage->representations[canonical].storage,
        iv::RealtimeBufferStorageKind::full_node_storage);
    EXPECT_EQ(storage->representations[canonical].frame_capacity, 32u);
    ASSERT_TRUE(storage->connection_representations[0].has_value());
    ASSERT_TRUE(storage->connection_representations[2].has_value());
    EXPECT_EQ(*storage->connection_representations[0], canonical);
    EXPECT_EQ(*storage->connection_representations[2], canonical);

    auto const copied_ring = *storage->connection_representations[1];
    ASSERT_NE(copied_ring, canonical);
    EXPECT_EQ(
        storage->representations[copied_ring].storage,
        iv::RealtimeBufferStorageKind::stack_with_persistent_carry);
    auto const copied_persistent_index =
        storage->representations[copied_ring].persistent_allocation;
    ASSERT_LT(copied_persistent_index, storage->persistent_allocations.size());
    EXPECT_EQ(
        storage->persistent_allocations[copied_persistent_index].kind,
        SamplePersistentStorageKind::compact_carry);
    EXPECT_EQ(
        storage->persistent_allocations[copied_persistent_index].retained_frames,
        7u);
    auto const copied = std::ranges::find_if(
        storage->feedback_timelines,
        [](SampleFeedbackTimelinePlan const& timeline) {
            return timeline.connection_index == 1;
        });
    ASSERT_NE(copied, storage->feedback_timelines.end());
    EXPECT_EQ(copied->timeline_representation, copied_ring);
    EXPECT_EQ(copied->writer.kind, SampleFeedbackTimelineWriterKind::copy);
    EXPECT_EQ(copied->writer.source_representation, canonical);
    EXPECT_EQ(copied->writer.after_execution_position, 0u);
    EXPECT_FLOAT_EQ(
        static_cast<float>(sample_value_from_bytes(copied->initial_value)),
        0.5f);

    auto const producer_home_count = std::ranges::count_if(
        storage->feedback_timelines,
        [](SampleFeedbackTimelinePlan const& timeline) {
            return timeline.writer.kind
                == SampleFeedbackTimelineWriterKind::producer_home;
        });
    EXPECT_EQ(producer_home_count, 2u);

    auto const home_persistent_index =
        storage->representations[canonical].persistent_allocation;
    ASSERT_LT(home_persistent_index, storage->persistent_allocations.size());
    auto const& home = storage->persistent_allocations[home_persistent_index];
    EXPECT_EQ(home.retained_frames, 12u);
    EXPECT_EQ(home.frame_capacity, 32u);
    ASSERT_TRUE(home.initialize_value.has_value());
    EXPECT_FLOAT_EQ(
        static_cast<float>(sample_value_from_bytes(*home.initialize_value)),
        0.0f);
    EXPECT_NE(home.migration_identity.find("graphjit.sample:"), std::string::npos);
    EXPECT_EQ(
        home.migration_identity.find("graphjit.sample.feedback:"),
        std::string::npos);

    iv::NodeLayoutBuilder builder(8);
    auto declared = declare_sample_storage(builder, *storage);
    ASSERT_TRUE(declared.has_value())
        << (declared ? std::string{} : declared.error());
    auto layout = std::move(builder).build();
    auto finalized = finalize_sample_storage(layout, *storage);
    ASSERT_TRUE(finalized.has_value())
        << (finalized ? std::string{} : finalized.error());
    auto const& home_region = layout.regions[
        storage->persistent_allocations[home_persistent_index].region.index];
    ASSERT_NE(home_region.raw_initialize_fn, nullptr);
    EXPECT_EQ(home_region.raw_initialize_data.size(), sizeof(iv::Sample));
}

TEST(GraphJitSampleStoragePlan, DetachedCompositionUsesPersistentShiftedTimeline)
{
    using namespace iv::graph_jit::detail;

    iv::ChannelLayout const mono{
        .channel_type = iv::ChannelTypeId::mono,
        .sample_layout = iv::SampleStreamLayout::planar,
    };
    iv::ChannelLayout const stereo{
        .channel_type = iv::ChannelTypeId::stereo,
        .sample_layout = iv::SampleStreamLayout::interleaved,
    };
    iv::SampleOutputChannelId const left_source{
        .bundle = 1,
        .port = 0,
        .channel = 0,
    };
    iv::SampleOutputChannelId const right_source{
        .bundle = 2,
        .port = 0,
        .channel = 0,
    };

    ConnectionAnalysisPlan connections;
    add_identified_test_nodes(connections, {1, 2, 3});
    connections.schedule.bundle_execution_position.resize(4);
    connections.schedule.bundle_execution_position[1] = 0;
    connections.schedule.bundle_execution_position[2] = 1;
    connections.schedule.bundle_execution_position[3] = 2;

    SampleConnectionPlan feedback;
    feedback.source_type = iv::ChannelTypeId::stereo;
    feedback.source_channels = {left_source, right_source};
    feedback.source_channel_timings = {
        SampleSourceChannelTimingPlan{
            .source = left_source,
            .source_layout = mono,
            .source_latency = 7,
            .read_latency = 7,
        },
        SampleSourceChannelTimingPlan{
            .source = right_source,
            .source_layout = mono,
            .source_latency = 2,
            .read_latency = 2,
        },
    };
    feedback.projection_contributions = {
        SampleProjectionContributionPlan{
            .source_type = iv::ChannelTypeId::mono,
            .source_channel_indices = {0},
            .target_type = iv::ChannelTypeId::mono,
            .target_channels = {0},
        },
        SampleProjectionContributionPlan{
            .source_type = iv::ChannelTypeId::mono,
            .source_channel_indices = {1},
            .target_type = iv::ChannelTypeId::mono,
            .target_channels = {1},
        },
    };
    feedback.target_type = iv::ChannelTypeId::stereo;
    feedback.target_layout = stereo;
    feedback.target_channels = {
        iv::SampleInputChannelId{.bundle = 3, .port = 0, .channel = 0},
        iv::SampleInputChannelId{.bundle = 3, .port = 0, .channel = 1},
    };
    feedback.target_port = iv::NodeBundlePortId{3, iv::PortKind::sample, 0};
    feedback.source_latency = 7;
    feedback.read_latency = 7;
    feedback.target_history = 3;
    feedback.requires_conversion = true;
    feedback.detach = iv::ConfiguredSampleConnectionDetach{
        .loop_extra_latency = 5,
        .initial_value_override = iv::Sample{0.25f},
    };
    feedback.detach_initial_value = sample_value_bytes(iv::Sample{0.25f});
    connections.sample_connections.push_back(feedback);

    auto append_group = [&](iv::SampleOutputChannelId source,
                            std::size_t begin) {
        SampleProducerGroupPlan group;
        group.source_port = iv::NodeBundlePortId{
            source.bundle, iv::PortKind::sample, source.port};
        group.source_type = iv::ChannelTypeId::mono;
        group.source_channels = {source};
        group.canonical_source_layout = mono;
        group.connection_indices = {0};
        group.has_realtime_connections = true;
        group.storage_plan = iv::SampleConnectionStoragePlan{
            iv::RealtimeBufferStorageKind::transient_stack};
        group.live_interval = ConnectionLiveIntervalPlan{
            .begin = begin,
            .end = 2,
            .crosses_kernel_invocations = true,
        };
        connections.sample_producer_groups.push_back(std::move(group));
    };
    append_group(left_source, 0);
    append_group(right_source, 1);

    auto storage = build_sample_storage_plan(connections, 8);
    ASSERT_TRUE(storage.has_value())
        << (storage ? std::string{} : storage.error());
    ASSERT_EQ(storage->producer_groups.size(), 2u);
    EXPECT_TRUE(storage->compositions.empty());
    ASSERT_EQ(storage->feedback_timelines.size(), 1u);
    ASSERT_EQ(storage->persistent_allocations.size(), 1u);
    ASSERT_TRUE(storage->connection_representations[0].has_value());

    auto const composed = *storage->connection_representations[0];
    ASSERT_LT(composed, storage->representations.size());
    auto const& representation = storage->representations[composed];
    EXPECT_FALSE(representation.canonical_producer_representation);
    EXPECT_EQ(
        representation.storage,
        iv::RealtimeBufferStorageKind::full_node_storage);
    EXPECT_EQ(representation.channel_layout, stereo);
    EXPECT_EQ(representation.frame_capacity, 32u);
    EXPECT_EQ(
        representation.transient_allocation,
        no_sample_transient_allocation);
    ASSERT_NE(
        representation.persistent_allocation,
        no_sample_persistent_allocation);

    auto const& persistent = storage->persistent_allocations[
        representation.persistent_allocation];
    EXPECT_EQ(persistent.kind, SamplePersistentStorageKind::ring);
    EXPECT_EQ(persistent.channel_layout, stereo);
    EXPECT_EQ(persistent.retained_frames, 15u);
    EXPECT_EQ(persistent.frame_capacity, 32u);
    ASSERT_TRUE(persistent.initialize_value.has_value());
    EXPECT_FLOAT_EQ(
        static_cast<float>(sample_value_from_bytes(
            *persistent.initialize_value)),
        0.25f);
    EXPECT_NE(
        persistent.migration_identity.find("graphjit.sample.composed_feedback:"),
        std::string::npos);

    auto const& timeline = storage->feedback_timelines.front();
    EXPECT_EQ(timeline.connection_index, 0u);
    EXPECT_EQ(timeline.timeline_representation, composed);
    EXPECT_EQ(timeline.channel_layout, stereo);
    EXPECT_EQ(timeline.retained_frames, 15u);
    EXPECT_EQ(timeline.loop_extra_latency, 5u);
    EXPECT_FLOAT_EQ(
        static_cast<float>(sample_value_from_bytes(timeline.initial_value)),
        0.25f);
    EXPECT_EQ(
        timeline.writer.kind,
        SampleFeedbackTimelineWriterKind::composition);
    EXPECT_EQ(timeline.writer.revision_frames, 7u);
    EXPECT_EQ(timeline.writer.after_execution_position, 1u);
    EXPECT_EQ(
        timeline.writer.source_representation,
        no_sample_representation);
    ASSERT_EQ(timeline.writer.composition_contributions.size(), 2u);
    auto const& left = timeline.writer.composition_contributions[0];
    EXPECT_EQ(left.source_layout.channel_type, iv::ChannelTypeId::mono);
    EXPECT_EQ(left.converted_layout.channel_type, iv::ChannelTypeId::mono);
    ASSERT_EQ(left.sources.size(), 1u);
    EXPECT_EQ(left.sources[0].source_channel, 0u);
    EXPECT_EQ(left.sources[0].read_latency, 7u);
    EXPECT_EQ(left.target_channels, std::vector<std::size_t>{0u});
    auto const& right = timeline.writer.composition_contributions[1];
    EXPECT_EQ(right.source_layout.channel_type, iv::ChannelTypeId::mono);
    EXPECT_EQ(right.converted_layout.channel_type, iv::ChannelTypeId::mono);
    ASSERT_EQ(right.sources.size(), 1u);
    EXPECT_EQ(right.sources[0].source_channel, 0u);
    EXPECT_EQ(right.sources[0].read_latency, 2u);
    EXPECT_EQ(right.target_channels, std::vector<std::size_t>{1u});

    iv::NodeLayoutBuilder builder(8);
    auto declared = declare_sample_storage(builder, *storage);
    ASSERT_TRUE(declared.has_value())
        << (declared ? std::string{} : declared.error());
    auto layout = std::move(builder).build();
    auto finalized = finalize_sample_storage(layout, *storage);
    ASSERT_TRUE(finalized.has_value())
        << (finalized ? std::string{} : finalized.error());
    auto const& region = layout.regions[persistent.region.index];
    ASSERT_NE(region.raw_initialize_fn, nullptr);

    iv::ResourceContext resources;
    auto node_storage = layout.create_storage(resources);
    node_storage.initialize();
    auto const bytes = node_storage.region_bytes(persistent.region);
    auto const initialized = std::span<iv::Sample const>{
        reinterpret_cast<iv::Sample const*>(bytes.data()),
        bytes.size() / sizeof(iv::Sample)};
    ASSERT_FALSE(initialized.empty());
    for (auto const sample : initialized) {
        EXPECT_FLOAT_EQ(static_cast<float>(sample), 0.25f);
    }
}

TEST(GraphJitSampleStoragePlan, DetachedMixingAlignsUnequalSourceLatencies)
{
    using namespace iv::graph_jit::detail;

    iv::ChannelLayout const mono{
        .channel_type = iv::ChannelTypeId::mono,
        .sample_layout = iv::SampleStreamLayout::planar,
    };
    iv::SampleOutputChannelId const slow_source{
        .bundle = 1,
        .port = 0,
        .channel = 0,
    };
    iv::SampleOutputChannelId const fast_source{
        .bundle = 2,
        .port = 0,
        .channel = 0,
    };

    ConnectionAnalysisPlan connections;
    connections.schedule.bundle_execution_position.resize(4);
    connections.schedule.bundle_execution_position[1] = 0;
    connections.schedule.bundle_execution_position[2] = 1;
    connections.schedule.bundle_execution_position[3] = 2;

    SampleConnectionPlan feedback;
    feedback.source_type = iv::ChannelTypeId::stereo;
    feedback.source_channels = {slow_source, fast_source};
    feedback.source_channel_timings = {
        SampleSourceChannelTimingPlan{
            .source = slow_source,
            .source_layout = mono,
            .source_latency = 7,
            .read_latency = 7,
        },
        SampleSourceChannelTimingPlan{
            .source = fast_source,
            .source_layout = mono,
            .source_latency = 2,
            .read_latency = 2,
        },
    };
    feedback.projection_contributions = {
        SampleProjectionContributionPlan{
            .source_type = iv::ChannelTypeId::stereo,
            .source_channel_indices = {0, 1},
            .target_type = iv::ChannelTypeId::mono,
            .target_channels = {0},
        },
    };
    feedback.target_type = iv::ChannelTypeId::mono;
    feedback.target_layout = mono;
    feedback.target_channels = {
        iv::SampleInputChannelId{.bundle = 3, .port = 0, .channel = 0},
    };
    feedback.target_port = iv::NodeBundlePortId{3, iv::PortKind::sample, 0};
    feedback.source_latency = 7;
    feedback.read_latency = 7;
    feedback.target_history = 3;
    feedback.requires_conversion = true;
    feedback.detach = iv::ConfiguredSampleConnectionDetach{
        .loop_extra_latency = 5,
        .initial_value_override = iv::Sample{0.25f},
    };
    feedback.detach_initial_value = sample_value_bytes(iv::Sample{0.25f});
    connections.sample_connections.push_back(feedback);

    auto append_group = [&](iv::SampleOutputChannelId source,
                            std::size_t begin) {
        SampleProducerGroupPlan group;
        group.source_port = iv::NodeBundlePortId{
            source.bundle, iv::PortKind::sample, source.port};
        group.source_type = iv::ChannelTypeId::mono;
        group.source_channels = {source};
        group.canonical_source_layout = mono;
        group.connection_indices = {0};
        group.has_realtime_connections = true;
        group.storage_plan = iv::SampleConnectionStoragePlan{
            iv::RealtimeBufferStorageKind::transient_stack};
        group.live_interval = ConnectionLiveIntervalPlan{
            .begin = begin,
            .end = 2,
            .crosses_kernel_invocations = true,
        };
        connections.sample_producer_groups.push_back(std::move(group));
    };
    append_group(slow_source, 0);
    append_group(fast_source, 1);

    auto storage = build_sample_storage_plan(connections, 8);
    ASSERT_TRUE(storage.has_value())
        << (storage ? std::string{} : storage.error());
    ASSERT_EQ(storage->feedback_timelines.size(), 1u);
    ASSERT_EQ(storage->persistent_allocations.size(), 2u);

    auto const& contribution = storage->feedback_timelines.front()
        .writer.composition_contributions.front();
    ASSERT_NE(
        contribution.feedback_alignment_representation,
        no_sample_representation);
    EXPECT_EQ(contribution.feedback_alignment_write_latency, 2u);
    ASSERT_LT(
        contribution.feedback_alignment_representation,
        storage->representations.size());
    auto const& alignment = storage->representations[
        contribution.feedback_alignment_representation];
    EXPECT_EQ(
        alignment.storage,
        iv::RealtimeBufferStorageKind::full_node_storage);
    EXPECT_EQ(alignment.channel_layout.channel_type, iv::ChannelTypeId::stereo);
    EXPECT_EQ(alignment.frame_capacity, 16u);
    ASSERT_LT(
        alignment.persistent_allocation,
        storage->persistent_allocations.size());
    auto const& alignment_allocation = storage->persistent_allocations[
        alignment.persistent_allocation];
    EXPECT_EQ(alignment_allocation.retained_frames, 5u);
    ASSERT_TRUE(alignment_allocation.initialize_value.has_value());
    EXPECT_FLOAT_EQ(
        static_cast<float>(sample_value_from_bytes(
            *alignment_allocation.initialize_value)),
        0.25f);

    iv::NodeLayoutBuilder builder(8);
    auto declared = declare_sample_storage(builder, *storage);
    ASSERT_TRUE(declared.has_value())
        << (declared ? std::string{} : declared.error());
    auto layout = std::move(builder).build();
    auto finalized = finalize_sample_storage(layout, *storage);
    ASSERT_TRUE(finalized.has_value())
        << (finalized ? std::string{} : finalized.error());

    auto const& alignment_region =
        layout.regions[alignment_allocation.region.index];
    ASSERT_NE(alignment_region.raw_initialize_fn, nullptr);
    EXPECT_EQ(
        alignment_region.raw_initialize_data.size(), sizeof(iv::Sample));

    iv::ResourceContext resources;
    auto node_storage = layout.create_storage(resources);
    node_storage.initialize();
    auto const alignment_bytes =
        node_storage.region_bytes(alignment_allocation.region);
    auto const initialized = std::span<iv::Sample const>{
        reinterpret_cast<iv::Sample const*>(alignment_bytes.data()),
        alignment_bytes.size() / sizeof(iv::Sample)};
    ASSERT_FALSE(initialized.empty());
    for (auto const sample : initialized) {
        EXPECT_FLOAT_EQ(static_cast<float>(sample), 0.25f);
    }
}

TEST(GraphJitSampleStoragePlan, ConvertedFeedbackUsesCompactCarryTimeline)
{
    using namespace iv::graph_jit::detail;

    iv::ChannelLayout const mono{
        .channel_type = iv::ChannelTypeId::mono,
        .sample_layout = iv::SampleStreamLayout::planar,
    };
    iv::ChannelLayout const stereo{
        .channel_type = iv::ChannelTypeId::stereo,
        .sample_layout = iv::SampleStreamLayout::interleaved,
    };
    iv::SampleOutputChannelId const source_channel{
        .bundle = 1,
        .port = 0,
        .channel = 0,
    };
    iv::NodeBundlePortId const source_port{1, iv::PortKind::sample, 0};

    ConnectionAnalysisPlan connections;
    connections.schedule.bundle_execution_position.resize(3);
    connections.schedule.bundle_execution_position[1] = 1;
    connections.schedule.bundle_execution_position[2] = 0;
    // Bundle 2 runs before bundle 1 and reads bundle 1's earlier values
    // through the detach, so the two form one cyclic region.
    connections.schedule.regions = {SccRegionPlan{
        .nodes = {1, 2},
        .execution_order = {2, 1},
        .cyclic = true,
        .maximum_block_size = 6,
        .scc_feedback_latency = 6,
    }};
    connections.schedule.region_order = {0};
    connections.schedule.ready_groups = {{0}};
    connections.schedule.bundle_to_region = {std::nullopt, 0, 0};

    SampleConnectionPlan feedback;
    feedback.source_type = iv::ChannelTypeId::mono;
    feedback.source_channels = {source_channel};
    feedback.source_channel_timings = {SampleSourceChannelTimingPlan{
        .source = source_channel,
        .source_layout = mono,
    }};
    feedback.canonical_source_port = source_port;
    feedback.canonical_source_layout = mono;
    feedback.target_type = iv::ChannelTypeId::stereo;
    feedback.target_layout = stereo;
    feedback.target_channels = {
        iv::SampleInputChannelId{.bundle = 2, .port = 0, .channel = 0},
        iv::SampleInputChannelId{.bundle = 2, .port = 0, .channel = 1},
    };
    feedback.target_port = iv::NodeBundlePortId{2, iv::PortKind::sample, 0};
    feedback.target_history = 3;
    feedback.requires_conversion = true;
    feedback.detach = iv::ConfiguredSampleConnectionDetach{
        .loop_extra_latency = 6,
        .initial_value_override = iv::Sample{-0.25f},
    };
    feedback.detach_initial_value = sample_value_bytes(iv::Sample{-0.25f});
    connections.sample_connections.push_back(feedback);

    SampleProducerGroupPlan group;
    group.source_port = source_port;
    group.source_type = iv::ChannelTypeId::mono;
    group.source_channels = {source_channel};
    group.canonical_source_layout = mono;
    group.connection_indices = {0};
    group.has_realtime_connections = true;
    group.storage_plan = iv::SampleConnectionStoragePlan{
        iv::RealtimeBufferStorageKind::transient_stack};
    group.live_interval = ConnectionLiveIntervalPlan{
        .begin = 0,
        .end = 1,
        .crosses_kernel_invocations = true,
    };
    connections.sample_producer_groups.push_back(group);

    auto storage = build_sample_storage_plan(connections, 64);
    ASSERT_TRUE(storage.has_value())
        << (storage ? std::string{} : storage.error());
    ASSERT_EQ(storage->representations.size(), 2u);
    ASSERT_EQ(storage->persistent_allocations.size(), 1u);
    ASSERT_EQ(storage->feedback_timelines.size(), 1u);
    EXPECT_TRUE(storage->materializations.empty());
    ASSERT_EQ(storage->carry_operations.size(), 1u);

    auto const canonical =
        storage->producer_groups[0]->canonical_representation;
    auto const& timeline = storage->feedback_timelines[0];
    EXPECT_EQ(timeline.writer.kind, SampleFeedbackTimelineWriterKind::copy);
    EXPECT_EQ(
        timeline.storage_plan.kind,
        iv::RealtimeBufferStorageKind::stack_with_persistent_carry);
    EXPECT_EQ(
        timeline.storage_plan.candidate_costs.stack_with_persistent_carry
            .copied_bytes,
        (64u + 2u * 9u) * sizeof(iv::Sample));
    EXPECT_EQ(
        timeline.storage_plan.candidate_costs.full_node_storage
            .ring_addressed_bytes,
        64u * sizeof(iv::Sample));
    auto const ring = timeline.timeline_representation;
    ASSERT_NE(canonical, ring);
    EXPECT_EQ(storage->representations[ring].channel_layout, mono);
    EXPECT_EQ(
        storage->representations[ring].storage,
        iv::RealtimeBufferStorageKind::stack_with_persistent_carry);
    EXPECT_FALSE(storage->connection_representations[0].has_value());
    ASSERT_TRUE(storage->connection_channel_bindings[0].has_value());
    auto const& converted_channels =
        *storage->connection_channel_bindings[0];
    ASSERT_EQ(converted_channels.size(), 2u);
    for (auto const& channel : converted_channels) {
        EXPECT_EQ(channel.representation, ring);
        EXPECT_EQ(channel.representation_channel, 0u);
        EXPECT_EQ(channel.frame_delay, 0u);
    }

    auto const persistent_index =
        storage->representations[ring].persistent_allocation;
    ASSERT_LT(persistent_index, storage->persistent_allocations.size());
    auto const& persistent = storage->persistent_allocations[persistent_index];
    EXPECT_EQ(persistent.kind, SamplePersistentStorageKind::compact_carry);
    EXPECT_EQ(persistent.channel_layout, mono);
    EXPECT_EQ(persistent.retained_frames, 9u);
    EXPECT_EQ(persistent.frame_capacity, 128u);
    EXPECT_EQ(persistent.size_bytes, 9u * sizeof(iv::Sample));

    auto const& carry = storage->carry_operations.front();
    EXPECT_EQ(carry.representation_index, ring);
    EXPECT_EQ(carry.persistent_allocation, persistent_index);
    EXPECT_EQ(carry.restore_execution_position, 0u);
    EXPECT_EQ(carry.commit_execution_position, 1u);
    EXPECT_EQ(carry.retained_frames, 9u);
    EXPECT_EQ(timeline.writer.after_execution_position, 1u);
    // Both endpoints lie in the cyclic region, so the carry surrounds the
    // complete sliced region and its working ring stays live across it.
    ASSERT_TRUE(carry.cyclic_region_scope);
    EXPECT_EQ(*carry.cyclic_region_scope, 0u);
    EXPECT_EQ(storage->representations[ring].live_interval.begin, 0u);
    EXPECT_EQ(storage->representations[ring].live_interval.end, 1u);
}

TEST(GraphJitSampleStoragePlan, AliasableConversionReadsRetainedProducerHistoryDirectly)
{
    using namespace iv::graph_jit::detail;

    iv::ChannelLayout const mono{
        .channel_type = iv::ChannelTypeId::mono,
        .sample_layout = iv::SampleStreamLayout::planar,
    };
    iv::ChannelLayout const stereo{
        .channel_type = iv::ChannelTypeId::stereo,
        .sample_layout = iv::SampleStreamLayout::interleaved,
    };

    ConnectionAnalysisPlan connections;
    SampleConnectionPlan converted;
    converted.source_channel_timings = {SampleSourceChannelTimingPlan{}};
    converted.canonical_source_layout = mono;
    converted.target_layout = stereo;
    converted.target_port = iv::NodeBundlePortId{2, iv::PortKind::sample, 0};
    converted.source_latency = 2;
    converted.read_latency = 2;
    converted.target_history = 5;
    converted.requires_conversion = true;
    connections.sample_connections.push_back(converted);
    connections.schedule.bundle_execution_position.resize(3);
    connections.schedule.bundle_execution_position[1] = 0;
    connections.schedule.bundle_execution_position[2] = 1;
    add_acyclic_test_regions(connections);

    SampleProducerGroupPlan group;
    group.canonical_source_layout = mono;
    group.connection_indices = {0};
    group.has_realtime_connections = true;
    group.storage_requirements = iv::SampleConnectionStorageRequirements{
        .current_block_frames = 64,
        .retained_frames = 7,
        .channel_count = 1,
    };
    group.storage_plan = iv::SampleConnectionStoragePlan{
        iv::RealtimeBufferStorageKind::stack_with_persistent_carry};
    group.live_interval = ConnectionLiveIntervalPlan{
        .begin = 0,
        .end = 1,
        .crosses_kernel_invocations = true,
    };
    connections.sample_producer_groups.push_back(group);

    auto storage = build_sample_storage_plan(connections, 64);
    ASSERT_TRUE(storage.has_value())
        << (storage ? std::string{} : storage.error());
    ASSERT_EQ(storage->representations.size(), 1u);
    EXPECT_TRUE(storage->materializations.empty());
    ASSERT_EQ(storage->carry_operations.size(), 1u);
    auto const canonical =
        storage->producer_groups[0]->canonical_representation;
    EXPECT_EQ(storage->representations[canonical].frame_capacity, 128u);
    EXPECT_FALSE(storage->connection_representations[0].has_value());
    ASSERT_TRUE(storage->connection_channel_bindings[0].has_value());
    auto const& channels = *storage->connection_channel_bindings[0];
    ASSERT_EQ(channels.size(), 2u);
    for (auto const& channel : channels) {
        EXPECT_EQ(channel.representation, canonical);
        EXPECT_EQ(channel.representation_channel, 0u);
        EXPECT_EQ(channel.frame_delay, 0u);
    }
    auto const canonical_allocation = storage->representations[canonical]
        .transient_allocation;
    ASSERT_LT(canonical_allocation, storage->transient_allocations.size());
    EXPECT_EQ(
        storage->transient_allocations[canonical_allocation].size_bytes,
        128u * sizeof(iv::Sample));
}

TEST(GraphJitSampleStoragePlan, AliasableConvertedFanoutSharesRetainedProducerTimeline)
{
    using namespace iv::graph_jit::detail;

    iv::ChannelLayout const mono{
        .channel_type = iv::ChannelTypeId::mono,
        .sample_layout = iv::SampleStreamLayout::planar,
    };
    iv::ChannelLayout const stereo{
        .channel_type = iv::ChannelTypeId::stereo,
        .sample_layout = iv::SampleStreamLayout::interleaved,
    };

    ConnectionAnalysisPlan connections;
    SampleConnectionPlan compensated;
    compensated.source_channel_timings = {SampleSourceChannelTimingPlan{}};
    compensated.canonical_source_layout = mono;
    compensated.target_layout = stereo;
    compensated.target_port = iv::NodeBundlePortId{2, iv::PortKind::sample, 0};
    compensated.read_latency = 7;
    compensated.requires_conversion = true;
    connections.sample_connections.push_back(compensated);

    SampleConnectionPlan current = compensated;
    current.target_port = iv::NodeBundlePortId{3, iv::PortKind::sample, 0};
    current.read_latency = 0;
    connections.sample_connections.push_back(current);

    connections.schedule.bundle_execution_position.resize(4);
    connections.schedule.bundle_execution_position[1] = 0;
    connections.schedule.bundle_execution_position[2] = 1;
    connections.schedule.bundle_execution_position[3] = 2;
    add_acyclic_test_regions(connections);

    SampleProducerGroupPlan group;
    group.canonical_source_layout = mono;
    group.connection_indices = {0, 1};
    group.has_realtime_connections = true;
    group.storage_requirements = iv::SampleConnectionStorageRequirements{
        .current_block_frames = 64,
        .retained_frames = 7,
        .channel_count = 1,
    };
    group.storage_plan = iv::SampleConnectionStoragePlan{
        iv::RealtimeBufferStorageKind::stack_with_persistent_carry};
    group.live_interval = ConnectionLiveIntervalPlan{
        .begin = 0,
        .end = 2,
        .crosses_kernel_invocations = true,
    };
    connections.sample_producer_groups.push_back(group);

    auto storage = build_sample_storage_plan(connections, 64);
    ASSERT_TRUE(storage.has_value())
        << (storage ? std::string{} : storage.error());
    ASSERT_EQ(storage->representations.size(), 1u);
    EXPECT_TRUE(storage->materializations.empty());
    ASSERT_EQ(storage->connection_representations.size(), 2u);
    EXPECT_FALSE(storage->connection_representations[0].has_value());
    EXPECT_FALSE(storage->connection_representations[1].has_value());
    ASSERT_TRUE(storage->connection_channel_bindings[0].has_value());
    ASSERT_TRUE(storage->connection_channel_bindings[1].has_value());
    auto const canonical =
        storage->producer_groups[0]->canonical_representation;
    EXPECT_EQ(storage->representations[canonical].frame_capacity, 128u);
    for (auto connection_index : {0u, 1u}) {
        auto const& channels =
            *storage->connection_channel_bindings[connection_index];
        ASSERT_EQ(channels.size(), 2u);
        for (auto const& channel : channels) {
            EXPECT_EQ(channel.representation, canonical);
            EXPECT_EQ(channel.representation_channel, 0u);
            EXPECT_EQ(channel.frame_delay, 0u);
        }
    }
}

TEST(GraphJitSampleStoragePlan, GeneratedConversionReadsAffectProducerStorageCost)
{
    using namespace iv::graph_jit::detail;

    iv::ChannelLayout const stereo{
        .channel_type = iv::ChannelTypeId::stereo,
        .sample_layout = iv::SampleStreamLayout::interleaved,
    };
    iv::ChannelLayout const mono{
        .channel_type = iv::ChannelTypeId::mono,
        .sample_layout = iv::SampleStreamLayout::planar,
    };
    iv::NodeBundlePortId const source_port{1, iv::PortKind::sample, 0};
    std::array const source_channels{
        iv::SampleOutputChannelId{.bundle = 1, .port = 0, .channel = 0},
        iv::SampleOutputChannelId{.bundle = 1, .port = 0, .channel = 1},
    };

    ConnectionAnalysisPlan connections;
    connections.schedule.bundle_execution_position.resize(3);
    connections.schedule.bundle_execution_position[1] = 0;
    connections.schedule.bundle_execution_position[2] = 1;
    add_acyclic_test_regions(connections);

    SampleConnectionPlan converted;
    converted.source_type = iv::ChannelTypeId::stereo;
    converted.source_channels.assign(source_channels.begin(), source_channels.end());
    converted.source_channel_timings = {
        SampleSourceChannelTimingPlan{
            .source = source_channels[0],
            .source_layout = stereo,
            .source_history = 1,
        },
        SampleSourceChannelTimingPlan{
            .source = source_channels[1],
            .source_layout = stereo,
            .source_history = 1,
        },
    };
    converted.canonical_source_port = source_port;
    converted.canonical_source_layout = stereo;
    converted.target_type = iv::ChannelTypeId::mono;
    converted.target_layout = mono;
    converted.target_channels = {
        iv::SampleInputChannelId{.bundle = 2, .port = 0, .channel = 0},
    };
    converted.target_port = iv::NodeBundlePortId{2, iv::PortKind::sample, 0};
    converted.requires_conversion = true;
    connections.sample_connections.push_back(converted);

    SampleProducerGroupPlan group;
    group.source_port = source_port;
    group.source_type = iv::ChannelTypeId::stereo;
    group.source_channels.assign(source_channels.begin(), source_channels.end());
    group.canonical_source_layout = stereo;
    group.connection_indices = {0};
    group.has_realtime_connections = true;
    group.storage_requirements = iv::SampleConnectionStorageRequirements{
        .current_block_frames = 64,
        .retained_frames = 1,
        .channel_count = 2,
        .value_size_bytes = sizeof(iv::Sample),
    };
    // Connection analysis has not yet seen the generated conversion reads. The
    // sample-buffer planner must replace this provisional choice using the work
    // it is about to emit.
    group.storage_plan = iv::SampleConnectionStoragePlan{
        iv::RealtimeBufferStorageKind::full_node_storage};
    group.live_interval = ConnectionLiveIntervalPlan{
        .begin = 0,
        .end = 1,
        .crosses_kernel_invocations = true,
    };
    connections.sample_producer_groups.push_back(group);

    auto storage = build_sample_storage_plan(
        connections,
        64,
        {},
        {},
        iv::RealtimeStorageCostModel{
            .copied_byte_weight = 0,
            .ring_addressed_byte_weight = 1,
            .stack_footprint_byte_weight = 0,
            .persistent_footprint_byte_weight = 0,
        });
    ASSERT_TRUE(storage.has_value())
        << (storage ? std::string{} : storage.error());
    ASSERT_TRUE(storage->producer_groups[0]);
    EXPECT_EQ(
        storage->producer_groups[0]->storage_plan.kind,
        iv::RealtimeBufferStorageKind::stack_with_persistent_carry);
    EXPECT_EQ(
        storage->producer_groups[0]
            ->storage_plan.candidate_costs.full_node_storage
            .ring_addressed_bytes,
        128u * sizeof(iv::Sample));
    ASSERT_EQ(storage->materializations.size(), 1u);
}

TEST(GraphJitSampleStoragePlan, RejectsTransientStorageThatCrossesKernelCalls)
{
    using namespace iv::graph_jit::detail;

    ConnectionAnalysisPlan connections;
    SampleProducerGroupPlan group;
    group.canonical_source_layout = iv::ChannelLayout{
        .channel_type = iv::ChannelTypeId::mono,
        .sample_layout = iv::SampleStreamLayout::planar,
    };
    group.has_realtime_connections = true;
    group.storage_plan = iv::SampleConnectionStoragePlan{
        iv::RealtimeBufferStorageKind::transient_stack};
    group.live_interval = ConnectionLiveIntervalPlan{
        .begin = 0,
        .end = 1,
        .crosses_kernel_invocations = true,
    };
    connections.sample_producer_groups.push_back(std::move(group));

    auto storage = build_sample_storage_plan(connections, 64);
    ASSERT_FALSE(storage.has_value());
    EXPECT_NE(
        storage.error().find("cross-kernel retained storage"),
        std::string::npos);
}

TEST(GraphJitBackgroundStoragePlan, SparseEventCountsSurviveConversionAndMixedFanin)
{
    iv::graph_jit::BackgroundStoragePlan plan;
    plan.ports.resize(4);
    for (auto& port : plan.ports) port.kind = iv::PortKind::event;
    plan.ports[0].max_event_count = 3;
    plan.ports[0].max_events_per_index = 100;
    plan.ports[1].max_events_per_index = .5;
    plan.ports[2].inputs = {0, 1};
    plan.ports[2].max_events_per_index = 100.5;
    plan.ports[3].inputs = {2}; // A conversion keeps the input's count bound.
    plan.ports[3].max_events_per_index = 100.5;
    auto bound = iv::graph_jit::detail::background_event_count_bound(plan, 3, 4);
    ASSERT_TRUE(bound);
    EXPECT_TRUE(bound->uses_finite_count);
    EXPECT_EQ(bound->count, 5u); // three finite points plus ceil(.5 * 4)
    auto empty = iv::graph_jit::detail::background_event_count_bound(plan, 3, 0);
    ASSERT_TRUE(empty);
    EXPECT_EQ(empty->count, 0u);
    plan.ports[3].inputs = {3};
    EXPECT_FALSE(iv::graph_jit::detail::background_event_count_bound(plan, 3, 4));
}

} // namespace iv::test::graph_jit_tests
