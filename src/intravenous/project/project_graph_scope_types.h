#pragma once

#include <intravenous/channel_type.h>
#include <intravenous/node/address.h>
#include <intravenous/node/manual_control.h>
#include <intravenous/ports.h>
#include <intravenous/project/iv_module_instance_types.h>
#include <intravenous/project/node_instance_interface.h>
#include <intravenous/project/project_connection_types.h>
#include <intravenous/project/project_presentation.h>

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
struct ProjectGraphScopeSource {
    std::string file_path{};
    std::uint32_t line = 0;
    std::uint32_t byte_offset = 0;
};
// One child of a graph editor scope. In the project root scope, each child is
// one desired project instance.
struct ProjectGraphScopeNode {
    NodeAddress address{};
    std::optional<NodeAddress> membership_address{};
    bool virtual_node = false;
    std::optional<std::size_t> virtual_member_count{};
    // The project instance owns the address. For a source-owned child, the
    // selector path identifies the row within that instance.
    IvModuleInstanceInfo instance{};
    std::optional<std::string> child_display_name{};
    std::optional<std::string> child_source_label{};
    std::optional<ProjectGraphScopeSource> declaration_source{};
    // A diagnostic from the latest configuration pass, not merely a pending
    // realization. The previous executable generation may still be running.
    std::string error_message{};
    ProjectGraphScopeNodeState load_state = ProjectGraphScopeNodeState::waiting_for_definition;
    // The desired configuration's interface. A failed build can supply this
    // independently of the previous executable generation.
    NodeInstanceInterface interface{};
    std::vector<ProjectGraphScopeManualControl> manual_controls{};
    // Construction values of the accepted placement, never a parse of the
    // desired source text. These remain from the previous generation on failure.
    std::string construction_definition_id{};
    std::optional<std::string> construction_arguments_json{};
    std::string construction_error{};
};

struct ProjectGraphScopeConnection {
    ProjectConnection connection{};
    // The current generation applied the connection; false while one of its
    // endpoints is missing or the connection has not been realized yet.
    bool applied = false;
    bool source_owned = false;
};

struct ProjectGraphChildScope {
    NodeAddress subject{};
    std::vector<ProjectGraphScopeNode> nodes{};
    bool has_boundary = false;
};

// The project root, its requested child scopes, and their connections share
// one configured graph and project revision.
struct ProjectGraphScope {
    // The successful configured-generation identity. Executable-only probe
    // and control variants do not change it. A failed desired configuration
    // can still supply diagnostic interfaces without committing a successor.
    std::optional<std::uint64_t> generation{};
    std::optional<std::size_t> sample_rate{};
    std::string execution_phase = "unconfigured";
    std::string execution_message{};
    std::optional<std::uint64_t> prepared_generation{};
    std::optional<std::uint64_t> active_generation{};
    bool editable = true;
    std::vector<ProjectGraphScopeNode> nodes{};
    std::vector<ProjectGraphChildScope> child_scopes{};
    std::vector<ProjectGraphScopeConnection> connections{};
    std::vector<NodeAddress> timeline_nodes{};
    std::vector<NodeAddress> unavailable_lane_parents{};
    ProjectPresentation presentation{};
};
} // namespace iv
