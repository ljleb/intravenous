#pragma once

#include <intravenous/channel_type.h>
#include <intravenous/node/address.h>
#include <intravenous/node/manual_control.h>
#include <intravenous/ports.h>
#include <intravenous/runtime/iv_module_instance_types.h>
#include <intravenous/runtime/node_instance_interface.h>
#include <intravenous/runtime/project_connection_types.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace iv {
enum class ProjectGraphScopeNodeState {
    waiting_for_definition,
    waiting_for_graph,
    ready,
    failed,
};
struct ProjectGraphScopeManualControl {
    ProjectManualControl control{};
    bool available = false;
    bool participates = false;
    // The latest value of an open gesture, which is not yet durable. A reader
    // shows it until a later read reports the gesture closed.
    std::optional<Sample> live_value{};
};
// One child of a graph editor scope. In the project root scope, each child is
// one desired project instance.
struct ProjectGraphScopeNode {
    NodeAddress address{};
    IvModuleInstanceInfo instance{};
    // A diagnostic from the latest configuration pass, not merely a pending
    // realization. The previous executable generation may still be running.
    std::string error_message{};
    ProjectGraphScopeNodeState load_state = ProjectGraphScopeNodeState::waiting_for_definition;
    // The desired configuration's interface. A failed build can supply this
    // independently of the previous executable generation.
    NodeInstanceInterface interface{};
    std::vector<ProjectGraphScopeManualControl> manual_controls{};
};

struct ProjectGraphScopeConnection {
    ProjectConnection connection{};
    // The current generation applied the connection; false while one of its
    // endpoints is missing or the connection has not been realized yet.
    bool applied = false;
};

// The graph editor's read model of one scope: its children, their ports, and
// the connections among them, read under ProjectGraph's lock so that they
// describe one revision.
struct ProjectGraphScope {
    // The current successful ProjectGraph generation, which can differ from
    // the desired configuration whose interfaces are displayed.
    std::optional<std::uint64_t> generation{};
    bool editable = true;
    std::vector<ProjectGraphScopeNode> nodes{};
    std::vector<ProjectGraphScopeConnection> connections{};
};
} // namespace iv
