#include "runtime/graph_input_lanes.h"

#include "graph/build_types.h"
#include "linker_event.h"
#include "runtime/graph_input_lanes_events.h"
#include "runtime/iv_module_instances.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <vector>

namespace {
struct GraphInputLanesWitness {
    std::vector<iv::GraphInputPortDescriptor> last_ports {};
};

GraphInputLanesWitness *g_graph_input_lanes_witness = nullptr;

std::vector<iv::GraphInputPortDescriptor> const &recorded_ports()
{
    if (g_graph_input_lanes_witness == nullptr) {
        throw std::runtime_error("graph input lanes witness not installed");
    }
    return g_graph_input_lanes_witness->last_ports;
}

iv::RuntimeIvModuleInstance make_instance_with_ports()
{
    iv::RuntimeIvModuleInstance instance {};
    instance.instance_id = "instance:1";
    instance.definition_id = "definition:1";
    instance.module_root = std::filesystem::path("/tmp/module");
    instance.module_id = "iv.test.module";

    iv::IntrospectionLogicalNode node {};
    node.id = "node-1";
    node.kind = "TestNode";
    node.sample_inputs.push_back(iv::LogicalPortInfo {
        .name = "frequency",
        .type = "sample",
        .connectivity = iv::LogicalPortConnectivity::disconnected,
        .ordinal = 0,
        .default_value = 440.0f,
    });
    node.event_inputs.push_back(iv::LogicalPortInfo {
        .name = "trigger",
        .type = "event",
        .connectivity = iv::LogicalPortConnectivity::disconnected,
        .ordinal = 1,
    });

    instance.introspection.logical_nodes.push_back(std::move(node));
    return instance;
}

IV_SUBSCRIBE_LINKER_EVENT(
    iv::RuntimeGraphInputLanesPortsChangedEvent,
    iv_runtime_graph_input_lanes_ports_changed_event,
    +[](iv::RuntimeGraphInputLanesPortsChangedRequest const &request,
        iv::RuntimeGraphInputLanesAckBuilder &builder) {
        if (g_graph_input_lanes_witness != nullptr) {
            g_graph_input_lanes_witness->last_ports = request.ports;
        }
        builder.succeed();
    });

IV_SUBSCRIBE_LINKER_EVENT(
    iv::RuntimeGraphInputLanesLaneBindingsRequestedEvent,
    iv_runtime_graph_input_lanes_lane_bindings_requested_event,
    +[](iv::RuntimeGraphInputLanesPortsChangedRequest const &,
        iv::RuntimeGraphInputLanesLaneBindingsBuilder &builder) {
        auto const &ports = recorded_ports();

        iv::GraphInputLaneBindings bindings {};
        uint64_t next_lane_id = 10;
        for (auto const &port : ports) {
            if (port.port_kind == iv::PortKind::sample) {
                bindings.logical_sample_knobs.push_back(iv::GraphInputLaneBinding {
                    .port = port,
                    .knob_lane = iv::LaneId {next_lane_id++},
                });
                bindings.sample_inputs.push_back(iv::GraphInputLaneBinding {
                    .port = port,
                    .knob_lane = iv::LaneId {next_lane_id++},
                    .graph_input_lane = iv::LaneId {next_lane_id++},
                    .logical_knob_lane = iv::LaneId {next_lane_id - 3},
                });
                continue;
            }

            bindings.event_inputs.push_back(iv::GraphInputLaneBinding {
                .port = port,
                .graph_input_lane = iv::LaneId {next_lane_id++},
            });
        }

        builder.succeed(std::move(bindings));
    });

IV_SUBSCRIBE_LINKER_EVENT(
    iv::RuntimeGraphInputLanesLaneOutputsRequestedEvent,
    iv_runtime_graph_input_lanes_lane_outputs_requested_event,
    +[](std::vector<iv::LaneId> const &,
        iv::RuntimeGraphInputLanesLaneOutputsBuilder &builder) {
        builder.succeed({});
    });

class RuntimeGraphInputLanesTest : public ::testing::Test {
protected:
    GraphInputLanesWitness witness {};

    void SetUp() override
    {
        g_graph_input_lanes_witness = &witness;
    }

    void TearDown() override
    {
        g_graph_input_lanes_witness = nullptr;
    }
};
} // namespace

TEST_F(RuntimeGraphInputLanesTest, QueryLanesReturnsGraphInputSubset)
{
    iv::RuntimeGraphInputLanes lanes;

    lanes.handle_iv_module_instances_changed(iv::RuntimeIvModuleInstancesChanged {
        .created = {make_instance_with_ports()},
    });

    auto const result = lanes.query_lanes(iv::LaneQueryFilter {.kind = "graphInputs"});

    ASSERT_EQ(witness.last_ports.size(), 2u);
    EXPECT_EQ(result.total_lane_count, 3u);
    ASSERT_EQ(result.lanes.size(), 3u);
    for (auto const &lane : result.lanes) {
        EXPECT_FALSE(lane.graph_input_port.logical_node_id.empty());
    }
}

TEST_F(RuntimeGraphInputLanesTest, QueryLanesRejectsUnsupportedFilter)
{
    iv::RuntimeGraphInputLanes lanes;

    lanes.handle_iv_module_instances_changed(iv::RuntimeIvModuleInstancesChanged {
        .created = {make_instance_with_ports()},
    });

    EXPECT_THROW(
        (void)lanes.query_lanes(iv::LaneQueryFilter {.kind = "notGraphInputs"}),
        std::runtime_error);
}
