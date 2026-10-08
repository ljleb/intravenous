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

    auto const& latency = planned->sample_states[1];
    EXPECT_EQ(latency.inherited_begin, 0);
    EXPECT_EQ(latency.inherited_end, 2);
    EXPECT_EQ(latency.inherited_extent_samples, 2u);
    EXPECT_EQ(latency.newly_exposed_extent_samples, 4u);
    EXPECT_EQ(latency.discarded_extent_samples, 0u);
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
