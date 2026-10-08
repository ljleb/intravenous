#include <intravenous/graph_jit/realtime_port_state_transition.h>

#include <gtest/gtest.h>

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
