#pragma once

#include <intravenous/channel_type.h>
#include <intravenous/node/address.h>
#include <intravenous/ports.h>
#include <intravenous/runtime/iv_module_instance_types.h>
#include <intravenous/runtime/project_connection_types.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace iv {
// One port of a scope child. `index` counts ports of the same kind and
// direction, as a ProjectPortMatcher's index does, so a connection can name
// the port by either. A sample port has a channel type; an event port has an
// event type.
struct ProjectGraphScopePort {
    std::string name{};
    std::size_t index = 0;
    std::optional<ChannelTypeId> channel_type{};
    std::optional<EventTypeId> event_type{};
    std::vector<ContinuousValueIntrospectionMetadata> value_types{};
};

// One child of a graph editor scope. In the project root scope, each child is
// one desired project instance.
struct ProjectGraphScopeNode {
    NodeAddress address{};
    IvModuleInstanceInfo instance{};
    // The realized ports of the instance's root in the current generation.
    // Empty while the instance has no placement in that generation.
    std::vector<ProjectGraphScopePort> sample_inputs{};
    std::vector<ProjectGraphScopePort> sample_outputs{};
    std::vector<ProjectGraphScopePort> event_inputs{};
    std::vector<ProjectGraphScopePort> event_outputs{};
    // The instance's root is a subgraph, which an editor can open as its root.
    bool has_children = false;
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
    // The ProjectGraph generation the ports come from; absent before the first
    // successful configuration pass.
    std::optional<std::uint64_t> generation{};
    bool editable = true;
    std::vector<ProjectGraphScopeNode> nodes{};
    std::vector<ProjectGraphScopeConnection> connections{};
};
} // namespace iv
