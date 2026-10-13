#include <intravenous/graph_jit/realtime_port_state_transition.h>

#include <gtest/gtest.h>

#include <cstring>
#include <optional>
#include <string>
#include <utility>

namespace {

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
            .constant_value = iv::Sample{value},
        },
        .retained_access = iv::graph_jit::SampleRealtimePortStateAccess::
            immutable_constant,
    };
}

iv::graph_jit::SampleRealtimePortStateRealization direct_sample(
    std::size_t requirement_index,
    std::size_t storage_offset,
    std::size_t channel = 0)
{
    iv::graph_jit::SampleRealtimePortStateRealization result{
        .requirement_index = requirement_index,
        .retained_access =
            iv::graph_jit::SampleRealtimePortStateAccess::direct,
    };
    result.retained_sources.push_back(
        iv::graph_jit::SampleRealtimePortStateStorageView{
            .storage = iv::graph_jit::SampleRealtimePortStateStorage::ring,
            .representation_channel = channel,
            .working_frame_capacity = 8,
            .storage_frame_count = 8,
            .node_storage_offset = storage_offset,
            .storage_size_bytes = 8 * sizeof(iv::Sample),
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

TEST(RealtimePortStateTransition, DescribesDirectEventSteadyStorageTransfer)
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
    EXPECT_EQ(transfer.target_begin, -32);
    EXPECT_EQ(transfer.target_end, 0);
    EXPECT_EQ(transfer.inherited_begin, -32);
    EXPECT_EQ(transfer.inherited_end, 0);
}

TEST(RealtimePortStateTransition, EventTypeChangeRequiresTransitionStorage)
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
        iv::graph_jit::InheritedRealtimePortStateRealization::
            transition_only_storage);
    EXPECT_TRUE(planned->event_transfers.empty());
}

TEST(RealtimePortStateTransition, ExecutesDirectSampleRingTransferAtCutover)
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
    for (std::size_t semantic = 6; semantic < 10; ++semantic) {
        write_sample(previous, semantic % 8, static_cast<float>(semantic));
    }
    for (std::size_t frame = 0; frame < 8; ++frame) {
        write_sample(current, frame, 99.0f);
    }
    auto view = iv::graph_jit::SampleRealtimePortStateStorageView{
        .storage = iv::graph_jit::SampleRealtimePortStateStorage::ring,
        .channel_layout = {
            .channel_type = iv::ChannelTypeId::mono,
            .sample_layout = iv::SampleStreamLayout::planar,
        },
        .working_frame_capacity = 8,
        .storage_frame_count = 8,
        .node_storage_offset = 0,
        .storage_size_bytes = 8 * sizeof(iv::Sample),
    };
    iv::graph_jit::RealtimePortStateTransitionPlan plan{
        .sample_transfers = {{
            .source_access =
                iv::graph_jit::SampleRealtimePortStateAccess::direct,
            .source_storage = {view},
            .target_storage = view,
            .target_begin = -5,
            .target_end = 0,
            .inherited_begin = -4,
            .inherited_end = 0,
        }},
    };

    auto executed = iv::graph_jit::execute_realtime_port_state_transfers(
        plan, previous, current, 10);

    ASSERT_TRUE(executed.has_value())
        << (executed ? std::string{} : executed.error());
    EXPECT_EQ(read_sample(current, 5), 0.0f);
    for (std::size_t semantic = 6; semantic < 10; ++semantic) {
        EXPECT_EQ(read_sample(current, semantic % 8), semantic);
    }
}

TEST(RealtimePortStateTransition, ExecutesDirectSampleCompactCarryTransfer)
{
    std::vector<std::byte> previous(8 * sizeof(iv::Sample));
    std::vector<std::byte> current(6 * sizeof(iv::Sample));
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
        .timeline_offset_frames = -2,
        .working_frame_capacity = 16,
        .storage_frame_count = 8,
        .carry_future_frames = 2,
        .node_storage_offset = 0,
        .storage_size_bytes = previous.size(),
    };
    auto const target = iv::graph_jit::SampleRealtimePortStateStorageView{
        .storage =
            iv::graph_jit::SampleRealtimePortStateStorage::compact_carry,
        .channel_layout = {
            .channel_type = iv::ChannelTypeId::mono,
            .sample_layout = iv::SampleStreamLayout::planar,
        },
        .timeline_offset_frames = -1,
        .working_frame_capacity = 8,
        .storage_frame_count = 6,
        .carry_future_frames = 1,
        .node_storage_offset = 0,
        .storage_size_bytes = current.size(),
    };
    iv::graph_jit::RealtimePortStateTransitionPlan plan{
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

    auto executed = iv::graph_jit::execute_realtime_port_state_transfers(
        plan, previous, current, 20);

    ASSERT_TRUE(executed.has_value())
        << (executed ? std::string{} : executed.error());
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
    constexpr std::size_t events_offset = 3 * sizeof(std::size_t);
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
    for (auto const [cursor, time] :
         {std::pair{3u, 96u}, std::pair{4u, 100u}, std::pair{5u, 104u}}) {
        iv::TimedEvent event{.time = time};
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
        .events_relative_offset = events_offset,
        .node_storage_offset = 0,
    };
    iv::graph_jit::RealtimePortStateTransitionPlan plan{
        .event_transfers = {{
            .source_access =
                iv::graph_jit::EventRealtimePortStateAccess::direct,
            .source_storage = view,
            .target_storage = view,
            .inherited_begin = -8,
            .inherited_end = 0,
        }},
    };

    auto executed = iv::graph_jit::execute_realtime_port_state_transfers(
        plan, previous, current, 108);

    ASSERT_TRUE(executed.has_value())
        << (executed ? std::string{} : executed.error());
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
        EXPECT_EQ(event.time, 100u + index * 4);
    }
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

} // namespace
