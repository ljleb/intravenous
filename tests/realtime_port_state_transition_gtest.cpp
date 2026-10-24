#include <intravenous/graph_jit/realtime_port_state_transition.h>
#include <intravenous/graph_jit/event_conversion_runtime.h>
#include <intravenous/graph_jit/event_retention_runtime.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

iv::GlobalIndex global_index(std::uint64_t bits)
{
    return iv::GlobalIndex::from_bits(bits);
}

TEST(RealtimePortStateTransition,
     StagedEventWindowKeepsRetainedHistoryAndSelectsOnlyCurrentEvents)
{
    std::array<iv::TimedEvent, 8> target{};
    target[6].time = global_index(98);
    target[7].time = global_index(99);
    std::array<iv::TimedEvent, 8> source{};
    source[6].time = global_index(99);
    source[7].time = global_index(100);
    source[0].time = global_index(101);
    source[1].time = global_index(102);
    source[2].time = global_index(103);

    auto const write =
        iv::graph_jit::detail::iv_graph_jit_merge_staged_event_window(
            target.data(),
            target.size(),
            6,
            8,
            source.data(),
            source.size(),
            6,
            11,
            global_index(100),
            3,
            2);

    ASSERT_EQ(write, 11u);
    std::array<std::uint64_t, 5> expected{98, 99, 100, 101, 102};
    for (std::size_t offset = 0; offset < expected.size(); ++offset) {
        EXPECT_EQ(
            target[(6 + offset) & (target.size() - 1)].time.bits(),
            expected[offset]);
    }
}

std::vector<std::byte> sample_value_bytes(iv::Sample value)
{
    std::vector<std::byte> result(sizeof(value));
    std::memcpy(result.data(), &value, sizeof(value));
    return result;
}

void write_sequential_cursor(
    std::vector<std::byte>& storage,
    std::size_t offset,
    std::uint64_t ordinal)
{
    ASSERT_LE(offset, storage.size());
    ASSERT_LE(sizeof(ordinal), storage.size() - offset);
    std::memcpy(storage.data() + offset, &ordinal, sizeof(ordinal));
}

iv::graph_jit::RealtimePortStateRequirement requirement(
    std::size_t bundle,
    std::size_t extent,
    iv::PortKind kind,
    iv::graph_jit::PortDirection direction,
    iv::graph_jit::RealtimePortStateRole role,
    std::string virtual_node,
    std::string port_name,
    std::optional<std::size_t> channel)
{
    return {
        .configured_port = iv::NodeBundlePortId{
            .node_bundle_handle = bundle,
            .port_kind = kind,
            .port_index = 3,
        },
        .direction = direction,
        .channel = channel,
        .role = role,
        .extent_samples = extent,
        .stable_identity = iv::graph_jit::StableRealtimePortStateId{
            .node = iv::graph_jit::StableConcreteNodeId{
                .graph = "root",
                .virtual_node = std::move(virtual_node),
                .direct_member = 2,
            },
            .direction = direction,
            .kind = kind,
            .port_name = std::move(port_name),
            .port_index = 3,
            .channel = channel,
            .role = role,
        },
    };
}

iv::graph_jit::RealtimePortStateRealizations realizations_for(
    iv::graph_jit::RealtimePortStateRequirements const& requirements)
{
    iv::graph_jit::RealtimePortStateRealizations result;
    for (std::size_t index = 0; index < requirements.states.size(); ++index) {
        if (requirements.states[index].configured_port.port_kind
            == iv::PortKind::sample) {
            result.sample_states.push_back(
                iv::graph_jit::SampleRealtimePortStateRealization{
                    .requirement_index = index,
                });
        } else {
            result.event_states.push_back(
                iv::graph_jit::EventRealtimePortStateRealization{
                    .requirement_index = index,
                });
        }
    }
    return result;
}

iv::graph_jit::SampleRealtimePortStateRealization constant_sample(
    std::size_t requirement_index, float value)
{
    return {
        .requirement_index = requirement_index,
        .authored_storage = iv::graph_jit::SampleRealtimePortStateStorageView{
            .storage = iv::graph_jit::SampleRealtimePortStateStorage::
                immutable_constant,
            .constant_value = sample_value_bytes(iv::Sample{value}),
        },
        .retained_access = iv::graph_jit::SampleRealtimePortStateAccess::
            immutable_constant,
    };
}

iv::graph_jit::SampleRealtimePortStateRealization direct_sample(
    std::size_t requirement_index,
    std::size_t storage_offset,
    std::size_t channel = 0,
    std::size_t cursor_offset = 0)
{
    iv::graph_jit::SampleRealtimePortStateRealization result{
        .requirement_index = requirement_index,
        .retained_access =
            iv::graph_jit::SampleRealtimePortStateAccess::direct,
    };
    result.retained_sources.push_back(
        iv::graph_jit::SampleRealtimePortStateStorageView{
            .storage = iv::graph_jit::SampleRealtimePortStateStorage::ring,
            .channel_layout = {
                .channel_type = iv::ChannelTypeId::stereo,
                .sample_layout = iv::SampleStreamLayout::planar,
            },
            .sequential_cursor_node_storage_offset = cursor_offset,
            .representation_channel = channel,
            .working_frame_capacity = 8,
            .storage_frame_count = 8,
            .node_storage_offset = storage_offset,
            .storage_size_bytes = 2 * 8 * sizeof(iv::Sample),
        });
    return result;
}

iv::graph_jit::EventRealtimePortStateRealization empty_event(
    std::size_t requirement_index)
{
    return {
        .requirement_index = requirement_index,
        .retained_access =
            iv::graph_jit::EventRealtimePortStateAccess::immutable_empty,
    };
}

iv::graph_jit::EventRealtimePortStateRealization direct_event(
    std::size_t requirement_index,
    std::size_t storage_offset,
    iv::EventTypeId type = iv::EventTypeId::midi)
{
    return {
        .requirement_index = requirement_index,
        .authored_storage =
            iv::graph_jit::EventRealtimePortStateStorageView{
                .type = type,
            },
        .retained_access =
            iv::graph_jit::EventRealtimePortStateAccess::direct,
        .retained_storage =
            iv::graph_jit::EventRealtimePortStateStorageView{
                .storage = iv::graph_jit::EventRealtimePortStateStorage::ring,
                .type = type,
                .event_capacity = 8,
                .size_bytes = 256,
                .alignment = alignof(iv::TimedEvent),
                .read_index_relative_offset = 0,
                .write_index_relative_offset = sizeof(std::size_t),
                .has_sequential_timeline = true,
                .timeline_initialized_relative_offset =
                    2 * sizeof(std::size_t),
                .expected_next_time_relative_offset =
                    3 * sizeof(std::size_t),
                .events_relative_offset = 4 * sizeof(std::size_t),
                .node_storage_offset = storage_offset,
            },
    };
}

TEST(RealtimePortStateTransition, MatchesStableStateAcrossGenerationLocalPorts)
{
    iv::graph_jit::RealtimePortStateRequirements previous{
        .states = {
            requirement(
                11,
                8,
                iv::PortKind::sample,
                iv::graph_jit::PortDirection::input,
                iv::graph_jit::RealtimePortStateRole::sequential_input_history,
                "node",
                "in",
                0),
            requirement(
                12,
                2,
                iv::PortKind::sample,
                iv::graph_jit::PortDirection::output,
                iv::graph_jit::RealtimePortStateRole::tick_output_latency,
                "node",
                "out",
                1),
        },
    };
    iv::graph_jit::RealtimePortStateRequirements current{
        .states = {
            requirement(
                91,
                5,
                iv::PortKind::sample,
                iv::graph_jit::PortDirection::input,
                iv::graph_jit::RealtimePortStateRole::sequential_input_history,
                "node",
                "in",
                0),
            requirement(
                92,
                6,
                iv::PortKind::sample,
                iv::graph_jit::PortDirection::output,
                iv::graph_jit::RealtimePortStateRole::tick_output_latency,
                "node",
                "out",
                1),
        },
    };

    auto planned = iv::graph_jit::plan_realtime_port_state_transition(
        previous,
        realizations_for(previous),
        current,
        realizations_for(current));

    ASSERT_TRUE(planned.has_value())
        << (planned ? std::string{} : planned.error());
    ASSERT_EQ(planned->sample_states.size(), 2u);
    auto const& history = planned->sample_states[0];
    EXPECT_EQ(history.previous_requirement_index, 0u);
    EXPECT_EQ(history.current_requirement_index, 0u);
    EXPECT_EQ(history.inherited_begin, -5);
    EXPECT_EQ(history.inherited_end, 0);
    EXPECT_EQ(history.inherited_extent_samples, 5u);
    EXPECT_EQ(history.newly_exposed_extent_samples, 0u);
    EXPECT_EQ(history.discarded_extent_samples, 3u);
    EXPECT_EQ(
        history.inherited_state_realization,
        iv::graph_jit::InheritedRealtimePortStateRealization::
            predecessor_state_unavailable);

    auto const& latency = planned->sample_states[1];
    EXPECT_EQ(latency.inherited_begin, 0);
    EXPECT_EQ(latency.inherited_end, 2);
    EXPECT_EQ(latency.inherited_extent_samples, 2u);
    EXPECT_EQ(latency.newly_exposed_extent_samples, 4u);
    EXPECT_EQ(latency.discarded_extent_samples, 0u);
    EXPECT_EQ(
        latency.inherited_state_realization,
        iv::graph_jit::InheritedRealtimePortStateRealization::
            predecessor_state_unavailable);
}

TEST(RealtimePortStateTransition, SeparatesEventMatchesAndOmitsNewIdentity)
{
    iv::graph_jit::RealtimePortStateRequirements previous{
        .states = {requirement(
            1,
            64,
            iv::PortKind::event,
            iv::graph_jit::PortDirection::input,
            iv::graph_jit::RealtimePortStateRole::sequential_input_history,
            "survivor",
            "events",
            std::nullopt)},
    };
    iv::graph_jit::RealtimePortStateRequirements current{
        .states = {
            requirement(
                2,
                32,
                iv::PortKind::event,
                iv::graph_jit::PortDirection::input,
                iv::graph_jit::RealtimePortStateRole::sequential_input_history,
                "survivor",
                "events",
                std::nullopt),
            requirement(
                3,
                16,
                iv::PortKind::event,
                iv::graph_jit::PortDirection::input,
                iv::graph_jit::RealtimePortStateRole::sequential_input_history,
                "new-node",
                "events",
                std::nullopt),
        },
    };

    auto planned = iv::graph_jit::plan_realtime_port_state_transition(
        previous,
        realizations_for(previous),
        current,
        realizations_for(current));

    ASSERT_TRUE(planned.has_value())
        << (planned ? std::string{} : planned.error());
    EXPECT_TRUE(planned->sample_states.empty());
    ASSERT_EQ(planned->event_states.size(), 1u);
    EXPECT_EQ(planned->event_states[0].inherited_begin, -32);
    EXPECT_EQ(planned->event_states[0].inherited_end, 0);
    EXPECT_EQ(
        planned->event_states[0].inherited_state_realization,
        iv::graph_jit::InheritedRealtimePortStateRealization::
            predecessor_state_unavailable);
}

TEST(RealtimePortStateTransition, SelectsImmutableAndPrivateSampleRealizations)
{
    iv::graph_jit::RealtimePortStateRequirements previous{
        .states = {
            requirement(
                1,
                8,
                iv::PortKind::sample,
                iv::graph_jit::PortDirection::input,
                iv::graph_jit::RealtimePortStateRole::sequential_input_history,
                "constant",
                "in",
                0),
            requirement(
                2,
                8,
                iv::PortKind::sample,
                iv::graph_jit::PortDirection::input,
                iv::graph_jit::RealtimePortStateRole::sequential_input_history,
                "stored",
                "in",
                0),
        },
    };
    auto current = previous;
    current.states[0].configured_port.node_bundle_handle = 11;
    current.states[1].configured_port.node_bundle_handle = 12;
    iv::graph_jit::RealtimePortStateRealizations previous_realizations{
        .sample_states = {
            constant_sample(0, 0.25f),
            direct_sample(1, 64),
        },
    };
    iv::graph_jit::RealtimePortStateRealizations current_realizations{
        .sample_states = {
            constant_sample(0, 0.25f),
            direct_sample(1, 512),
        },
    };

    auto planned = iv::graph_jit::plan_realtime_port_state_transition(
        previous,
        previous_realizations,
        current,
        current_realizations);

    ASSERT_TRUE(planned.has_value())
        << (planned ? std::string{} : planned.error());
    ASSERT_EQ(planned->sample_states.size(), 2u);
    EXPECT_EQ(
        planned->sample_states[0].inherited_state_realization,
        iv::graph_jit::InheritedRealtimePortStateRealization::
            already_represented);
    EXPECT_EQ(
        planned->sample_states[1].inherited_state_realization,
        iv::graph_jit::InheritedRealtimePortStateRealization::
            transfer_to_steady_storage);
    ASSERT_EQ(planned->sample_transfers.size(), 1u);
    auto const& transfer = planned->sample_transfers.front();
    EXPECT_EQ(transfer.transition_index, 1u);
    EXPECT_EQ(
        transfer.source_access,
        iv::graph_jit::SampleRealtimePortStateAccess::direct);
    ASSERT_EQ(transfer.source_storage.size(), 1u);
    ASSERT_TRUE(transfer.source_storage[0].node_storage_offset.has_value());
    ASSERT_TRUE(transfer.target_storage.node_storage_offset.has_value());
    EXPECT_EQ(*transfer.source_storage[0].node_storage_offset, 64u);
    EXPECT_EQ(*transfer.target_storage.node_storage_offset, 512u);
    EXPECT_EQ(transfer.target_begin, -8);
    EXPECT_EQ(transfer.target_end, 0);
    EXPECT_EQ(transfer.inherited_begin, -8);
    EXPECT_EQ(transfer.inherited_end, 0);
}

TEST(RealtimePortStateTransition, NewStateSharingSampleTargetRequiresTransitionStorage)
{
    iv::graph_jit::RealtimePortStateRequirements previous{
        .states = {requirement(
            1,
            8,
            iv::PortKind::sample,
            iv::graph_jit::PortDirection::input,
            iv::graph_jit::RealtimePortStateRole::sequential_input_history,
            "survivor",
            "in",
            0)},
    };
    iv::graph_jit::RealtimePortStateRequirements current{
        .states = {
            previous.states.front(),
            requirement(
                2,
                8,
                iv::PortKind::sample,
                iv::graph_jit::PortDirection::input,
                iv::graph_jit::RealtimePortStateRole::sequential_input_history,
                "new-state",
                "in",
                0),
        },
    };
    iv::graph_jit::RealtimePortStateRealizations previous_realizations{
        .sample_states = {direct_sample(0, 64)},
    };
    iv::graph_jit::RealtimePortStateRealizations current_realizations{
        .sample_states = {
            direct_sample(0, 512),
            direct_sample(1, 512),
        },
    };

    auto planned = iv::graph_jit::plan_realtime_port_state_transition(
        previous,
        previous_realizations,
        current,
        current_realizations);

    ASSERT_TRUE(planned.has_value())
        << (planned ? std::string{} : planned.error());
    ASSERT_EQ(planned->sample_states.size(), 1u);
    EXPECT_EQ(
        planned->sample_states[0].inherited_state_realization,
        iv::graph_jit::InheritedRealtimePortStateRealization::
            transition_only_storage);
    EXPECT_TRUE(planned->sample_transfers.empty());

    iv::NodeLayoutBuilder layout_builder(8);
    (void)layout_builder.declare_raw_region(1024);
    auto const steady_layout = std::move(layout_builder).build();
    auto realized =
        iv::graph_jit::realize_realtime_port_state_transition_storage(
            steady_layout,
            std::move(*planned),
            previous_realizations,
            current,
            current_realizations);
    ASSERT_TRUE(realized.has_value())
        << (realized ? std::string{} : realized.error());
    EXPECT_EQ(realized->node_layout.steady_storage_size, 1024u);
    EXPECT_GT(realized->node_layout.storage_size, 1024u);
    ASSERT_EQ(realized->plan.sample_transfers.size(), 1u);
    EXPECT_TRUE(
        realized->plan.sample_transfers.front().target_transition_only);
    auto const& target =
        realized->plan.sample_transfers.front().target_storage;
    ASSERT_TRUE(target.node_storage_offset.has_value());
    EXPECT_GE(*target.node_storage_offset, 1024u);
    auto const target_offset = *target.node_storage_offset;

    std::vector<std::byte> previous_storage(128);
    std::vector<std::byte> current_storage(
        realized->node_layout.storage_size);
    for (std::size_t i = 0; i < 8; ++i) {
        auto const value = iv::Sample{static_cast<float>(i + 1)};
        std::memcpy(
            previous_storage.data() + 64 + i * sizeof(value),
            &value,
            sizeof(value));
    }
    write_sequential_cursor(previous_storage, 0, 8);
    write_sequential_cursor(current_storage, 0, 8);
    auto prepared = iv::graph_jit::prepare_realtime_port_state_transfers(
        std::move(realized->plan), previous_storage, current_storage);
    ASSERT_TRUE(prepared.has_value())
        << (prepared ? std::string{} : prepared.error());
    iv::graph_jit::execute_realtime_port_state_transfers(
        *prepared, previous_storage, current_storage, global_index(8));
    EXPECT_EQ(
        std::memcmp(
            previous_storage.data() + 64,
            current_storage.data() + target_offset,
            8 * sizeof(iv::Sample)),
        0);
}

TEST(RealtimePortStateTransition, MaterializesInheritedSampleStateIntoSteadyStorage)
{
    iv::graph_jit::RealtimePortStateRequirements previous{
        .states = {requirement(
            1,
            4,
            iv::PortKind::sample,
            iv::graph_jit::PortDirection::input,
            iv::graph_jit::RealtimePortStateRole::sequential_input_history,
            "survivor",
            "in",
            0)},
    };
    auto current = previous;
    current.states.front().configured_port.node_bundle_handle = 2;

    auto stereo_source = [](std::size_t channel) {
        return iv::graph_jit::SampleRealtimePortStateStorageView{
            .storage = iv::graph_jit::SampleRealtimePortStateStorage::ring,
            .channel_layout = {
                .channel_type = iv::ChannelTypeId::stereo,
                .sample_layout = iv::SampleStreamLayout::planar,
            },
            .sequential_cursor_node_storage_offset =
                2 * 8 * sizeof(iv::Sample),
            .representation_channel = channel,
            .working_frame_capacity = 8,
            .storage_frame_count = 8,
            .node_storage_offset = 0,
            .storage_size_bytes = 2 * 8 * sizeof(iv::Sample),
        };
    };
    auto mono_target = iv::graph_jit::SampleRealtimePortStateStorageView{
        .storage = iv::graph_jit::SampleRealtimePortStateStorage::ring,
        .channel_layout = {
            .channel_type = iv::ChannelTypeId::mono,
            .sample_layout = iv::SampleStreamLayout::planar,
        },
        .sequential_cursor_node_storage_offset = 8 * sizeof(iv::Sample),
        .working_frame_capacity = 8,
        .storage_frame_count = 8,
        .node_storage_offset = 0,
        .storage_size_bytes = 8 * sizeof(iv::Sample),
    };
    iv::graph_jit::RealtimePortStateRealizations previous_realizations{
        .sample_states = {{
            .requirement_index = 0,
            .authored_storage = iv::graph_jit::SampleRealtimePortStateStorageView{
                .storage = iv::graph_jit::SampleRealtimePortStateStorage::
                    callback_transient,
                .channel_layout = mono_target.channel_layout,
            },
            .retained_access =
                iv::graph_jit::SampleRealtimePortStateAccess::materialized,
            .retained_sources = {stereo_source(0), stereo_source(1)},
            .materialization =
                iv::graph_jit::SampleRealtimePortStateMaterialization{
                    .source_layout = stereo_source(0).channel_layout,
                    .target_layout = mono_target.channel_layout,
                    .retained_before = 4,
                },
        }},
    };
    iv::graph_jit::RealtimePortStateRealizations current_realizations{
        .sample_states = {{
            .requirement_index = 0,
            .authored_storage = mono_target,
            .retained_access =
                iv::graph_jit::SampleRealtimePortStateAccess::direct,
            .retained_sources = {mono_target},
        }},
    };

    auto planned = iv::graph_jit::plan_realtime_port_state_transition(
        previous,
        previous_realizations,
        current,
        current_realizations);
    ASSERT_TRUE(planned.has_value())
        << (planned ? std::string{} : planned.error());
    ASSERT_EQ(planned->sample_states.size(), 1u);
    EXPECT_EQ(
        planned->sample_states.front().inherited_state_realization,
        iv::graph_jit::InheritedRealtimePortStateRealization::
            transfer_to_steady_storage);

    iv::NodeLayoutBuilder layout_builder(8);
    (void)layout_builder.declare_raw_region(
        mono_target.storage_size_bytes, alignof(iv::Sample));
    (void)layout_builder.declare_raw_region(
        sizeof(std::uint64_t), alignof(std::uint64_t));
    auto realized =
        iv::graph_jit::realize_realtime_port_state_transition_storage(
            std::move(layout_builder).build(),
            std::move(*planned),
            previous_realizations,
            current,
            current_realizations);
    ASSERT_TRUE(realized.has_value())
        << (realized ? std::string{} : realized.error());
    ASSERT_EQ(realized->plan.sample_transfers.size(), 1u);
    auto const& transfer = realized->plan.sample_transfers.front();
    EXPECT_EQ(
        transfer.source_access,
        iv::graph_jit::SampleRealtimePortStateAccess::materialized);
    ASSERT_EQ(transfer.source_storage.size(), 2u);
    ASSERT_TRUE(transfer.source_materialization);
    ASSERT_TRUE(transfer.target_storage.node_storage_offset);
    auto const target_offset = *transfer.target_storage.node_storage_offset;

    std::vector<std::byte> predecessor(
        2 * 8 * sizeof(iv::Sample) + sizeof(std::uint64_t));
    for (std::size_t frame = 0; frame < 8; ++frame) {
        auto const left = iv::Sample{static_cast<float>(frame)};
        auto const right = iv::Sample{static_cast<float>(100 + frame)};
        std::memcpy(
            predecessor.data() + frame * sizeof(left),
            &left,
            sizeof(left));
        std::memcpy(
            predecessor.data() + (8 + frame) * sizeof(right),
            &right,
            sizeof(right));
    }
    write_sequential_cursor(
        predecessor, 2 * 8 * sizeof(iv::Sample), 8);
    std::vector<std::byte> successor(realized->node_layout.storage_size);
    write_sequential_cursor(successor, 8 * sizeof(iv::Sample), 8);
    auto prepared = iv::graph_jit::prepare_realtime_port_state_transfers(
        std::move(realized->plan), predecessor, successor);
    ASSERT_TRUE(prepared.has_value())
        << (prepared ? std::string{} : prepared.error());
    iv::graph_jit::execute_realtime_port_state_transfers(
        *prepared, predecessor, successor, global_index(8));

    for (std::size_t frame = 4; frame < 8; ++frame) {
        iv::Sample sample{};
        std::memcpy(
            &sample,
            successor.data() + target_offset + frame * sizeof(sample),
            sizeof(sample));
        EXPECT_FLOAT_EQ(sample.value, static_cast<float>(50 + frame));
    }
}

TEST(RealtimePortStateTransition, DynamicValueShapeWithoutTypeMigrationStartsFresh)
{
    iv::graph_jit::RealtimePortStateRequirements previous{
        .states = {requirement(
            1,
            4,
            iv::PortKind::sample,
            iv::graph_jit::PortDirection::input,
            iv::graph_jit::RealtimePortStateRole::sequential_input_history,
            "node",
            "spectrum",
            0)},
    };
    auto current = previous;
    current.states[0].configured_port.node_bundle_handle = 9;
    auto previous_state = direct_sample(0, 64);
    auto current_state = direct_sample(0, 512);
    auto const previous_geometry = iv::continuous_value_storage_geometry(
        iv::ContinuousValueTypeId::fft_block, 4);
    auto const current_geometry = iv::continuous_value_storage_geometry(
        iv::ContinuousValueTypeId::fft_block, 8);
    ASSERT_TRUE(previous_geometry);
    ASSERT_TRUE(current_geometry);
    previous_state.authored_storage.geometry = *previous_geometry;
    previous_state.retained_sources[0].geometry = *previous_geometry;
    previous_state.authored_storage.storage_size_bytes =
        8 * previous_geometry->value_size_bytes;
    previous_state.retained_sources[0].storage_size_bytes =
        8 * previous_geometry->value_size_bytes;
    current_state.authored_storage.geometry = *current_geometry;
    current_state.retained_sources[0].geometry = *current_geometry;
    current_state.authored_storage.storage_size_bytes =
        8 * current_geometry->value_size_bytes;
    current_state.retained_sources[0].storage_size_bytes =
        8 * current_geometry->value_size_bytes;

    auto planned = iv::graph_jit::plan_realtime_port_state_transition(
        previous,
        iv::graph_jit::RealtimePortStateRealizations{
            .sample_states = {previous_state}},
        current,
        iv::graph_jit::RealtimePortStateRealizations{
            .sample_states = {current_state}});

    ASSERT_TRUE(planned.has_value())
        << (planned ? std::string{} : planned.error());
    ASSERT_EQ(planned->sample_states.size(), 1u);
    EXPECT_EQ(
        planned->sample_states[0].inherited_state_realization,
        iv::graph_jit::InheritedRealtimePortStateRealization::fresh_state);
    EXPECT_TRUE(planned->sample_transfers.empty());
}

TEST(RealtimePortStateTransition, EmptyEventsNeedNoTransferIntoPrivateStorage)
{
    iv::graph_jit::RealtimePortStateRequirements previous{
        .states = {requirement(
            1,
            8,
            iv::PortKind::event,
            iv::graph_jit::PortDirection::input,
            iv::graph_jit::RealtimePortStateRole::sequential_input_history,
            "node",
            "events",
            std::nullopt)},
    };
    auto current = previous;
    iv::graph_jit::RealtimePortStateRealizations previous_realizations{
        .event_states = {empty_event(0)},
    };
    iv::graph_jit::RealtimePortStateRealizations current_realizations{
        .event_states = {direct_event(0, 256)},
    };

    auto planned = iv::graph_jit::plan_realtime_port_state_transition(
        previous,
        previous_realizations,
        current,
        current_realizations);

    ASSERT_TRUE(planned.has_value())
        << (planned ? std::string{} : planned.error());
    ASSERT_EQ(planned->event_states.size(), 1u);
    EXPECT_EQ(
        planned->event_states[0].inherited_state_realization,
        iv::graph_jit::InheritedRealtimePortStateRealization::
            already_represented);
    EXPECT_TRUE(planned->event_transfers.empty());
}

TEST(
    RealtimePortStateTransition,
    RoundsTransitionOnlyEventCapacityUpToARingCapacity)
{
    iv::graph_jit::RealtimePortStateRequirements previous{
        .states = {requirement(
            1,
            8,
            iv::PortKind::event,
            iv::graph_jit::PortDirection::input,
            iv::graph_jit::RealtimePortStateRole::sequential_input_history,
            "node",
            "events",
            std::nullopt)},
    };
    auto current = previous;
    current.states.front().configured_port.node_bundle_handle = 2;
    iv::graph_jit::RealtimePortStateRealizations previous_realizations{
        .event_states = {direct_event(0, 64)},
    };
    iv::graph_jit::RealtimePortStateRealizations current_realizations{
        .event_states = {{
            .requirement_index = 0,
            .authored_storage = {
                .storage = iv::graph_jit::EventRealtimePortStateStorage::
                    callback_transient,
                .type = iv::EventTypeId::midi,
                .event_capacity = 3,
            },
            .retained_access =
                iv::graph_jit::EventRealtimePortStateAccess::callback_only,
        }},
    };

    auto planned = iv::graph_jit::plan_realtime_port_state_transition(
        previous,
        previous_realizations,
        current,
        current_realizations);
    ASSERT_TRUE(planned.has_value())
        << (planned ? std::string{} : planned.error());
    ASSERT_EQ(planned->event_states.size(), 1u);
    EXPECT_EQ(
        planned->event_states.front().inherited_state_realization,
        iv::graph_jit::InheritedRealtimePortStateRealization::
            transition_only_storage);

    iv::NodeLayoutBuilder layout_builder(8);
    (void)layout_builder.declare_raw_region(64);
    auto realized =
        iv::graph_jit::realize_realtime_port_state_transition_storage(
            std::move(layout_builder).build(),
            std::move(*planned),
            previous_realizations,
            current,
            current_realizations);
    ASSERT_TRUE(realized.has_value())
        << (realized ? std::string{} : realized.error());
    EXPECT_EQ(realized->node_layout.steady_storage_size, 64u);
    EXPECT_GT(realized->node_layout.storage_size, 64u);
    ASSERT_EQ(realized->plan.event_transfers.size(), 1u);
    auto const& transfer = realized->plan.event_transfers.front();
    EXPECT_TRUE(transfer.target_transition_only);
    EXPECT_EQ(transfer.target_storage.event_capacity, 4u);
    ASSERT_TRUE(transfer.target_storage.node_storage_offset);
    EXPECT_GE(*transfer.target_storage.node_storage_offset, 64u);
}

TEST(RealtimePortStateTransition, DescribesIndexedEventSteadyStorageTransfer)
{
    iv::graph_jit::RealtimePortStateRequirements previous{
        .states = {requirement(
            1,
            32,
            iv::PortKind::event,
            iv::graph_jit::PortDirection::input,
            iv::graph_jit::RealtimePortStateRole::sequential_input_history,
            "node",
            "events",
            std::nullopt)},
    };
    auto current = previous;
    current.states[0].configured_port.node_bundle_handle = 9;
    iv::graph_jit::RealtimePortStateRealizations previous_realizations{
        .event_states = {direct_event(0, 128)},
    };
    iv::graph_jit::RealtimePortStateRealizations current_realizations{
        .event_states = {direct_event(0, 640)},
    };
    previous_realizations.event_states[0].retained_storage->storage =
        iv::graph_jit::EventRealtimePortStateStorage::compact_carry;
    current_realizations.event_states[0].retained_storage->storage =
        iv::graph_jit::EventRealtimePortStateStorage::compact_carry;
    current_realizations.event_states[0].retained_access =
        iv::graph_jit::EventRealtimePortStateAccess::indexed_merged_source;
    current_realizations.event_states[0].retained_storage->has_source_indices =
        true;
    current_realizations.event_states[0].retained_storage
        ->source_indices_relative_offset = 160;
    current_realizations.event_states[0].retained_source_index = 4;

    auto planned = iv::graph_jit::plan_realtime_port_state_transition(
        previous,
        previous_realizations,
        current,
        current_realizations);

    ASSERT_TRUE(planned.has_value())
        << (planned ? std::string{} : planned.error());
    ASSERT_EQ(planned->event_states.size(), 1u);
    EXPECT_EQ(
        planned->event_states[0].inherited_state_realization,
        iv::graph_jit::InheritedRealtimePortStateRealization::
            transfer_to_steady_storage);
    ASSERT_EQ(planned->event_transfers.size(), 1u);
    auto const& transfer = planned->event_transfers.front();
    EXPECT_EQ(transfer.transition_index, 0u);
    EXPECT_EQ(
        transfer.source_access,
        iv::graph_jit::EventRealtimePortStateAccess::direct);
    ASSERT_TRUE(transfer.source_storage.node_storage_offset.has_value());
    ASSERT_TRUE(transfer.target_storage.node_storage_offset.has_value());
    EXPECT_EQ(*transfer.source_storage.node_storage_offset, 128u);
    EXPECT_EQ(*transfer.target_storage.node_storage_offset, 640u);
    EXPECT_EQ(
        transfer.source_storage.storage,
        iv::graph_jit::EventRealtimePortStateStorage::compact_carry);
    EXPECT_EQ(
        transfer.target_storage.storage,
        iv::graph_jit::EventRealtimePortStateStorage::compact_carry);
    ASSERT_TRUE(transfer.target_index.has_value());
    EXPECT_EQ(*transfer.target_index, 4u);
    EXPECT_EQ(transfer.target_begin, -32);
    EXPECT_EQ(transfer.target_end, 0);
    EXPECT_EQ(transfer.inherited_begin, -32);
    EXPECT_EQ(transfer.inherited_end, 0);
}

TEST(
    RealtimePortStateTransition,
    MergesDistinctProducersIntoSharedIndexedEventSteadyStorage)
{
    iv::graph_jit::RealtimePortStateRequirements previous{
        .states = {
            requirement(
                1,
                16,
                iv::PortKind::event,
                iv::graph_jit::PortDirection::output,
                iv::graph_jit::RealtimePortStateRole::tick_output_history,
                "first",
                "events",
                std::nullopt),
            requirement(
                2,
                16,
                iv::PortKind::event,
                iv::graph_jit::PortDirection::output,
                iv::graph_jit::RealtimePortStateRole::tick_output_history,
                "second",
                "events",
                std::nullopt),
        },
    };
    auto current = previous;
    current.states[0].configured_port.node_bundle_handle = 11;
    current.states[1].configured_port.node_bundle_handle = 12;
    iv::graph_jit::RealtimePortStateRealizations previous_realizations{
        .event_states = {direct_event(0, 128), direct_event(1, 384)},
    };
    auto first_target = direct_event(0, 640);
    auto second_target = direct_event(1, 640);
    for (auto* target : {&first_target, &second_target}) {
        target->retained_access =
            iv::graph_jit::EventRealtimePortStateAccess::indexed_merged_source;
        target->retained_storage->has_source_indices = true;
        target->retained_storage->source_indices_relative_offset = 160;
    }
    first_target.retained_source_index = 0;
    second_target.retained_source_index = 1;
    iv::graph_jit::RealtimePortStateRealizations current_realizations{
        .event_states = {std::move(first_target), std::move(second_target)},
    };

    auto planned = iv::graph_jit::plan_realtime_port_state_transition(
        previous,
        previous_realizations,
        current,
        current_realizations);

    ASSERT_TRUE(planned.has_value())
        << (planned ? std::string{} : planned.error());
    ASSERT_EQ(planned->event_states.size(), 2u);
    ASSERT_EQ(planned->event_transfers.size(), 2u);
    EXPECT_EQ(
        planned->event_states[0].inherited_state_realization,
        iv::graph_jit::InheritedRealtimePortStateRealization::
            transfer_to_steady_storage);
    EXPECT_EQ(
        planned->event_states[1].inherited_state_realization,
        iv::graph_jit::InheritedRealtimePortStateRealization::
            transfer_to_steady_storage);
    ASSERT_TRUE(planned->event_transfers[0].target_index.has_value());
    ASSERT_TRUE(planned->event_transfers[1].target_index.has_value());
    EXPECT_EQ(*planned->event_transfers[0].target_index, 0u);
    EXPECT_EQ(*planned->event_transfers[1].target_index, 1u);
    EXPECT_EQ(
        planned->event_transfers[0].target_storage.node_storage_offset,
        planned->event_transfers[1].target_storage.node_storage_offset);
}

TEST(RealtimePortStateTransition, UnsupportedEventTypeChangeStartsFresh)
{
    iv::graph_jit::RealtimePortStateRequirements previous{
        .states = {requirement(
            1,
            32,
            iv::PortKind::event,
            iv::graph_jit::PortDirection::input,
            iv::graph_jit::RealtimePortStateRole::sequential_input_history,
            "node",
            "events",
            std::nullopt)},
    };
    auto current = previous;
    current.states[0].configured_port.node_bundle_handle = 9;
    iv::graph_jit::RealtimePortStateRealizations previous_realizations{
        .event_states = {direct_event(0, 128, iv::EventTypeId::trigger)},
    };
    iv::graph_jit::RealtimePortStateRealizations current_realizations{
        .event_states = {direct_event(0, 640, iv::EventTypeId::midi)},
    };

    auto planned = iv::graph_jit::plan_realtime_port_state_transition(
        previous,
        previous_realizations,
        current,
        current_realizations);

    ASSERT_TRUE(planned.has_value())
        << (planned ? std::string{} : planned.error());
    ASSERT_EQ(planned->event_states.size(), 1u);
    EXPECT_EQ(
        planned->event_states[0].inherited_state_realization,
        iv::graph_jit::InheritedRealtimePortStateRealization::fresh_state);
    EXPECT_TRUE(planned->event_transfers.empty());
}

TEST(RealtimePortStateTransition, UsesPaceCursorsAcrossPaceChangeAndGlobalJump)
{
    std::vector<std::byte> previous(64);
    std::vector<std::byte> current(64);
    auto write_sample = [](std::vector<std::byte>& bytes,
                           std::size_t frame,
                           float value) {
        iv::Sample sample{value};
        std::memcpy(bytes.data() + frame * sizeof(sample), &sample, sizeof(sample));
    };
    auto read_sample = [](std::vector<std::byte> const& bytes,
                          std::size_t frame) {
        iv::Sample sample{};
        std::memcpy(&sample, bytes.data() + frame * sizeof(sample), sizeof(sample));
        return sample.value;
    };
    constexpr std::uint64_t source_cursor = 106;
    constexpr std::uint64_t target_cursor = 501;
    for (std::size_t relative = 4; relative != 0; --relative) {
        auto const ordinal = source_cursor - relative;
        write_sample(previous, ordinal % 8, static_cast<float>(ordinal));
    }
    for (std::size_t frame = 0; frame < 8; ++frame) {
        write_sample(current, frame, 99.0f);
    }
    auto source_view = iv::graph_jit::SampleRealtimePortStateStorageView{
        .storage = iv::graph_jit::SampleRealtimePortStateStorage::ring,
        .channel_layout = {
            .channel_type = iv::ChannelTypeId::mono,
            .sample_layout = iv::SampleStreamLayout::planar,
        },
        .absolute_pace_exponent = -1,
        .sequential_cursor_node_storage_offset = 8 * sizeof(iv::Sample),
        .working_frame_capacity = 8,
        .storage_frame_count = 8,
        .node_storage_offset = 0,
        .storage_size_bytes = 8 * sizeof(iv::Sample),
    };
    auto target_view = source_view;
    target_view.absolute_pace_exponent = 1;
    write_sequential_cursor(
        previous,
        *source_view.sequential_cursor_node_storage_offset,
        source_cursor);
    write_sequential_cursor(
        current,
        *target_view.sequential_cursor_node_storage_offset,
        target_cursor);
    iv::graph_jit::RealtimePortStateTransitionPlan plan{
        .sample_states = {{
            .inherited_state_realization =
                iv::graph_jit::InheritedRealtimePortStateRealization::
                    transfer_to_steady_storage,
        }},
        .sample_transfers = {{
            .source_access =
                iv::graph_jit::SampleRealtimePortStateAccess::direct,
            .source_storage = {source_view},
            .target_storage = target_view,
            .target_begin = -5,
            .target_end = 0,
            .inherited_begin = -4,
            .inherited_end = 0,
        }},
    };

    auto prepared = iv::graph_jit::prepare_realtime_port_state_transfers(
        plan, previous, current);
    ASSERT_TRUE(prepared.has_value())
        << (prepared ? std::string{} : prepared.error());
    iv::graph_jit::execute_realtime_port_state_transfers(
        *prepared, previous, current, global_index(1'000'000));
    EXPECT_EQ(read_sample(current, (target_cursor - 5) % 8), 0.0f);
    for (std::size_t relative = 4; relative != 0; --relative) {
        EXPECT_EQ(
            read_sample(current, (target_cursor - relative) % 8),
            source_cursor - relative);
    }
}

TEST(RealtimePortStateTransition, ExecutesDirectSampleCompactCarryTransfer)
{
    constexpr auto previous_sample_bytes = 8 * sizeof(iv::Sample);
    constexpr auto current_sample_bytes = 6 * sizeof(iv::Sample);
    std::vector<std::byte> previous(
        previous_sample_bytes + sizeof(std::uint64_t));
    std::vector<std::byte> current(
        current_sample_bytes + sizeof(std::uint64_t));
    auto write_sample = [](std::vector<std::byte>& bytes,
                           std::size_t frame,
                           float value) {
        iv::Sample sample{value};
        std::memcpy(bytes.data() + frame * sizeof(sample), &sample, sizeof(sample));
    };
    auto read_sample = [](std::vector<std::byte> const& bytes,
                          std::size_t frame) {
        iv::Sample sample{};
        std::memcpy(&sample, bytes.data() + frame * sizeof(sample), sizeof(sample));
        return sample.value;
    };
    write_sample(previous, 1, 17.0f);
    write_sample(previous, 2, 18.0f);
    write_sample(previous, 3, 19.0f);
    for (std::size_t frame = 0; frame < 6; ++frame) {
        write_sample(current, frame, 99.0f);
    }
    auto const source = iv::graph_jit::SampleRealtimePortStateStorageView{
        .storage =
            iv::graph_jit::SampleRealtimePortStateStorage::compact_carry,
        .channel_layout = {
            .channel_type = iv::ChannelTypeId::mono,
            .sample_layout = iv::SampleStreamLayout::planar,
        },
        .sequential_cursor_node_storage_offset = previous_sample_bytes,
        .timeline_offset_frames = -2,
        .working_frame_capacity = 16,
        .storage_frame_count = 8,
        .carry_future_frames = 2,
        .node_storage_offset = 0,
        .storage_size_bytes = previous_sample_bytes,
    };
    auto const target = iv::graph_jit::SampleRealtimePortStateStorageView{
        .storage =
            iv::graph_jit::SampleRealtimePortStateStorage::compact_carry,
        .channel_layout = {
            .channel_type = iv::ChannelTypeId::mono,
            .sample_layout = iv::SampleStreamLayout::planar,
        },
        .sequential_cursor_node_storage_offset = current_sample_bytes,
        .timeline_offset_frames = -1,
        .working_frame_capacity = 8,
        .storage_frame_count = 6,
        .carry_future_frames = 1,
        .node_storage_offset = 0,
        .storage_size_bytes = current_sample_bytes,
    };
    write_sequential_cursor(previous, previous_sample_bytes, 20);
    write_sequential_cursor(current, current_sample_bytes, 100);
    iv::graph_jit::RealtimePortStateTransitionPlan plan{
        .sample_states = {{
            .inherited_state_realization =
                iv::graph_jit::InheritedRealtimePortStateRealization::
                    transfer_to_steady_storage,
        }},
        .sample_transfers = {{
            .source_access =
                iv::graph_jit::SampleRealtimePortStateAccess::direct,
            .source_storage = {source},
            .target_storage = target,
            .target_begin = -4,
            .target_end = 0,
            .inherited_begin = -3,
            .inherited_end = 0,
        }},
    };

    auto prepared = iv::graph_jit::prepare_realtime_port_state_transfers(
        plan, previous, current);
    ASSERT_TRUE(prepared.has_value())
        << (prepared ? std::string{} : prepared.error());
    iv::graph_jit::execute_realtime_port_state_transfers(
        *prepared, previous, current, global_index(7'000));
    EXPECT_EQ(read_sample(current, 0), 0.0f);
    EXPECT_EQ(read_sample(current, 1), 17.0f);
    EXPECT_EQ(read_sample(current, 2), 18.0f);
    EXPECT_EQ(read_sample(current, 3), 19.0f);
}

TEST(RealtimePortStateTransition, PlansDirectCompactCarrySteadyStorageTransfer)
{
    iv::graph_jit::RealtimePortStateRequirements previous{
        .states = {requirement(
            1,
            3,
            iv::PortKind::sample,
            iv::graph_jit::PortDirection::input,
            iv::graph_jit::RealtimePortStateRole::sequential_input_history,
            "node",
            "in",
            0)},
    };
    auto current = previous;
    current.states[0].configured_port.node_bundle_handle = 9;
    current.states[0].extent_samples = 5;
    auto previous_state = direct_sample(0, 64);
    previous_state.retained_sources[0].storage =
        iv::graph_jit::SampleRealtimePortStateStorage::compact_carry;
    previous_state.retained_sources[0].carry_future_frames = 2;
    auto current_state = direct_sample(0, 512);
    current_state.retained_sources[0].storage =
        iv::graph_jit::SampleRealtimePortStateStorage::compact_carry;
    current_state.retained_sources[0].carry_future_frames = 1;
    iv::graph_jit::RealtimePortStateRealizations previous_realizations{
        .sample_states = {std::move(previous_state)},
    };
    iv::graph_jit::RealtimePortStateRealizations current_realizations{
        .sample_states = {std::move(current_state)},
    };

    auto planned = iv::graph_jit::plan_realtime_port_state_transition(
        previous,
        previous_realizations,
        current,
        current_realizations);

    ASSERT_TRUE(planned.has_value())
        << (planned ? std::string{} : planned.error());
    ASSERT_EQ(planned->sample_states.size(), 1u);
    EXPECT_EQ(
        planned->sample_states[0].inherited_state_realization,
        iv::graph_jit::InheritedRealtimePortStateRealization::
            transfer_to_steady_storage);
    ASSERT_EQ(planned->sample_transfers.size(), 1u);
    EXPECT_EQ(planned->sample_transfers[0].target_begin, -5);
    EXPECT_EQ(planned->sample_transfers[0].target_end, 0);
    EXPECT_EQ(
        planned->sample_transfers[0].source_storage[0].storage,
        iv::graph_jit::SampleRealtimePortStateStorage::compact_carry);
    EXPECT_EQ(
        planned->sample_transfers[0].target_storage.storage,
        iv::graph_jit::SampleRealtimePortStateStorage::compact_carry);
}

TEST(RealtimePortStateTransition, ExecutesDirectEventRingTransferAtCutover)
{
    constexpr std::size_t read_offset = sizeof(std::size_t);
    constexpr std::size_t write_offset = 2 * sizeof(std::size_t);
    constexpr std::size_t initialized_offset = 3 * sizeof(std::size_t);
    constexpr std::size_t expected_offset = 4 * sizeof(std::size_t);
    constexpr std::size_t events_offset = 5 * sizeof(std::size_t);
    constexpr std::size_t capacity = 4;
    constexpr std::size_t ring_size =
        events_offset + capacity * sizeof(iv::TimedEvent);
    std::vector<std::byte> previous(ring_size);
    std::vector<std::byte> current(ring_size);
    auto write_size = [](std::vector<std::byte>& bytes,
                         std::size_t offset,
                         std::size_t value) {
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    };
    write_size(previous, read_offset, 3);
    write_size(previous, write_offset, 6);
    previous[initialized_offset] = std::byte{1};
    auto const expected_position = global_index(108);
    std::memcpy(
        previous.data() + expected_offset,
        &expected_position,
        sizeof(expected_position));
    for (auto const [cursor, time] :
         {std::pair{3u, 96u}, std::pair{4u, 100u}, std::pair{5u, 104u}}) {
        iv::TimedEvent event{.time = global_index(time)};
        std::memcpy(
            previous.data() + events_offset
                + (cursor % capacity) * sizeof(event),
            &event,
            sizeof(event));
    }
    auto view = iv::graph_jit::EventRealtimePortStateStorageView{
        .storage = iv::graph_jit::EventRealtimePortStateStorage::ring,
        .type = iv::EventTypeId::empty,
        .event_capacity = capacity,
        .size_bytes = ring_size,
        .alignment = alignof(iv::TimedEvent),
        .read_index_relative_offset = read_offset,
        .write_index_relative_offset = write_offset,
        .has_sequential_timeline = true,
        .timeline_initialized_relative_offset = initialized_offset,
        .expected_next_time_relative_offset = expected_offset,
        .events_relative_offset = events_offset,
        .node_storage_offset = 0,
    };
    iv::graph_jit::RealtimePortStateTransitionPlan plan{
        .event_states = {{
            .inherited_state_realization =
                iv::graph_jit::InheritedRealtimePortStateRealization::
                    transfer_to_steady_storage,
        }},
        .event_transfers = {{
            .source_access =
                iv::graph_jit::EventRealtimePortStateAccess::direct,
            .source_storage = view,
            .target_storage = view,
            .target_begin = -8,
            .target_end = 0,
            .inherited_begin = -8,
            .inherited_end = 0,
        }},
    };

    auto prepared = iv::graph_jit::prepare_realtime_port_state_transfers(
        plan, previous, current);
    ASSERT_TRUE(prepared.has_value())
        << (prepared ? std::string{} : prepared.error());
    iv::graph_jit::execute_realtime_port_state_transfers(
        *prepared, previous, current, global_index(1008));
    std::size_t read = 1;
    std::size_t write = 0;
    std::memcpy(&read, current.data() + read_offset, sizeof(read));
    std::memcpy(&write, current.data() + write_offset, sizeof(write));
    EXPECT_EQ(read, 0u);
    EXPECT_EQ(write, 2u);
    for (std::size_t index = 0; index < write; ++index) {
        iv::TimedEvent event{};
        std::memcpy(
            &event,
            current.data() + events_offset + index * sizeof(event),
            sizeof(event));
        EXPECT_EQ(event.time, global_index(1000u + index * 4));
    }
    EXPECT_EQ(current[initialized_offset], std::byte{1});
    iv::GlobalIndex target_expected{};
    std::memcpy(
        &target_expected,
        current.data() + expected_offset,
        sizeof(target_expected));
    EXPECT_EQ(target_expected, global_index(1008));
}

TEST(
    RealtimePortStateTransition,
    CombinesCompactEventRolesIntoOneSteadySequenceAtCutover)
{
    constexpr std::size_t count_offset = 0;
    constexpr std::size_t initialized_offset = sizeof(std::size_t);
    constexpr std::size_t expected_offset = 2 * sizeof(std::size_t);
    constexpr std::size_t events_offset = 3 * sizeof(std::size_t);
    constexpr std::size_t capacity = 4;
    constexpr std::size_t source_indices_offset =
        events_offset + capacity * sizeof(iv::TimedEvent);
    constexpr std::size_t storage_size =
        source_indices_offset + capacity * sizeof(std::size_t);
    std::vector<std::byte> previous(storage_size);
    std::vector<std::byte> current(storage_size);
    std::size_t const source_count = 3;
    std::memcpy(
        previous.data() + count_offset,
        &source_count,
        sizeof(source_count));
    previous[initialized_offset] = std::byte{1};
    auto const expected_position = global_index(108);
    std::memcpy(
        previous.data() + expected_offset,
        &expected_position,
        sizeof(expected_position));
    for (std::size_t index = 0; index < source_count; ++index) {
        iv::TimedEvent event{.time = global_index(100 + index * 4)};
        std::memcpy(
            previous.data() + events_offset + index * sizeof(event),
            &event,
            sizeof(event));
    }
    auto const view = iv::graph_jit::EventRealtimePortStateStorageView{
        .storage =
            iv::graph_jit::EventRealtimePortStateStorage::compact_carry,
        .type = iv::EventTypeId::empty,
        .event_capacity = capacity,
        .size_bytes = storage_size,
        .alignment = alignof(iv::TimedEvent),
        .count_relative_offset = count_offset,
        .has_sequential_timeline = true,
        .timeline_initialized_relative_offset = initialized_offset,
        .expected_next_time_relative_offset = expected_offset,
        .events_relative_offset = events_offset,
        .has_source_indices = true,
        .source_indices_relative_offset = source_indices_offset,
        .node_storage_offset = 0,
    };
    iv::graph_jit::RealtimePortStateTransitionPlan plan{
        .event_states = {
            {.inherited_state_realization =
                 iv::graph_jit::InheritedRealtimePortStateRealization::
                     transfer_to_steady_storage},
            {.inherited_state_realization =
                 iv::graph_jit::InheritedRealtimePortStateRealization::
                     transfer_to_steady_storage},
        },
        .event_transfers = {
            {
                .transition_index = 0,
                .source_access =
                    iv::graph_jit::EventRealtimePortStateAccess::direct,
                .source_storage = view,
                .target_storage = view,
                .target_index = 1,
                .target_begin = -8,
                .target_end = 0,
                .inherited_begin = -8,
                .inherited_end = 0,
            },
            {
                .transition_index = 1,
                .source_access =
                    iv::graph_jit::EventRealtimePortStateAccess::direct,
                .source_storage = view,
                .target_storage = view,
                .target_index = 0,
                .target_begin = 0,
                .target_end = 8,
                .inherited_begin = 0,
                .inherited_end = 8,
            },
        },
    };

    auto prepared = iv::graph_jit::prepare_realtime_port_state_transfers(
        plan, previous, current);
    ASSERT_TRUE(prepared.has_value())
        << (prepared ? std::string{} : prepared.error());
    iv::graph_jit::execute_realtime_port_state_transfers(
        *prepared, previous, current, global_index(1008));

    std::size_t target_count = 0;
    std::memcpy(
        &target_count,
        current.data() + count_offset,
        sizeof(target_count));
    ASSERT_EQ(target_count, 3u);
    for (std::size_t index = 0; index < target_count; ++index) {
        iv::TimedEvent event{};
        std::memcpy(
            &event,
            current.data() + events_offset + index * sizeof(event),
            sizeof(event));
        EXPECT_EQ(event.time, global_index(1000 + index * 4));
    }
    for (std::size_t index = 0; index < target_count; ++index) {
        std::size_t source_index = 0;
        std::memcpy(
            &source_index,
            current.data() + source_indices_offset
                + index * sizeof(source_index),
            sizeof(source_index));
        EXPECT_EQ(source_index, index < 2 ? 1u : 0u);
    }
    EXPECT_EQ(current[initialized_offset], std::byte{1});
    iv::GlobalIndex target_expected{};
    std::memcpy(
        &target_expected,
        current.data() + expected_offset,
        sizeof(target_expected));
    EXPECT_EQ(target_expected, global_index(1008));
}

TEST(EventSequentialRetention, RebasesOnlyLiveRingEventsAcrossSeek)
{
    std::array<iv::TimedEvent, 4> events{};
    events[3].time = global_index(96);
    events[0].time = global_index(100);
    events[1].time = global_index(104);

    iv::graph_jit::detail::iv_graph_jit_rebase_event_ring(
        events.data(),
        events.size(),
        3,
        6,
        global_index(108),
        global_index(1008));

    EXPECT_EQ(events[3].time, global_index(996));
    EXPECT_EQ(events[0].time, global_index(1000));
    EXPECT_EQ(events[1].time, global_index(1004));
    EXPECT_EQ(events[2].time, global_index(0));
}

TEST(RealtimePortStateTransition, RejectsDuplicateStableIdentity)
{
    auto duplicated = requirement(
        1,
        8,
        iv::PortKind::sample,
        iv::graph_jit::PortDirection::input,
        iv::graph_jit::RealtimePortStateRole::sequential_input_history,
        "node",
        "in",
        0);
    iv::graph_jit::RealtimePortStateRequirements previous{
        .states = {duplicated, duplicated},
    };
    iv::graph_jit::RealtimePortStateRequirements current{
        .states = {duplicated},
    };

    auto planned = iv::graph_jit::plan_realtime_port_state_transition(
        previous,
        realizations_for(previous),
        current,
        realizations_for(current));

    ASSERT_FALSE(planned.has_value());
    EXPECT_NE(planned.error().find("duplicate stable identity"), std::string::npos);
}

TEST(RealtimePortStateTransition, RejectsStableIdentityRequirementMismatch)
{
    auto invalid = requirement(
        1,
        8,
        iv::PortKind::sample,
        iv::graph_jit::PortDirection::input,
        iv::graph_jit::RealtimePortStateRole::sequential_input_history,
        "node",
        "in",
        0);
    invalid.stable_identity->port_index = 4;
    iv::graph_jit::RealtimePortStateRequirements previous{
        .states = {invalid},
    };
    iv::graph_jit::RealtimePortStateRequirements current{};

    auto planned = iv::graph_jit::plan_realtime_port_state_transition(
        previous,
        realizations_for(previous),
        current,
        realizations_for(current));

    ASSERT_FALSE(planned.has_value());
    EXPECT_NE(
        planned.error().find("stable identity disagrees"),
        std::string::npos);
}

TEST(RealtimePortStateTransition,
     MixedEventMergeOrdersHistoryBeforeCurrentAcrossSources)
{
    // Realtime events carry TriggerEvent so the merged order shows which
    // source supplied each event. Both sources hold history before the
    // callback position and events after it.
    std::array<iv::TimedEvent, 4> realtime{};
    realtime[0] = {.time = global_index(98), .value = iv::TriggerEvent{}};
    realtime[1] = {.time = global_index(101), .value = iv::TriggerEvent{}};
    std::array<iv::TimedEvent, 4> supplied{};
    supplied[0].time = global_index(99);
    supplied[1].time = global_index(100);
    supplied[2].time = global_index(101);
    std::array<std::size_t, 4> supplied_indices{0, 0, 0, 0};
    std::array<iv::TimedEvent, 8> merged{};

    auto const count =
        iv::graph_jit::detail::iv_graph_jit_merge_mixed_event_sequences(
            merged.data(),
            merged.size(),
            realtime.data(),
            nullptr,
            1,
            realtime.size(),
            0,
            2,
            supplied.data(),
            supplied_indices.data(),
            std::numeric_limits<std::size_t>::max(),
            supplied.size(),
            0,
            3,
            global_index(100));

    ASSERT_EQ(count, 5u);
    std::array<std::pair<std::uint64_t, bool>, 5> const expected{{
        {98, true},
        {99, false},
        {100, false},
        // Equal times order by semantic source index: supplied 0 before
        // realtime 1.
        {101, false},
        {101, true},
    }};
    for (std::size_t index = 0; index < expected.size(); ++index) {
        EXPECT_EQ(merged[index].time.bits(), expected[index].first);
        EXPECT_EQ(
            std::holds_alternative<iv::TriggerEvent>(merged[index].value),
            expected[index].second);
    }
}

} // namespace
