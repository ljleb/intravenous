#pragma once

#include <intravenous/runtime/iv_module_instance_types.h>
#include <intravenous/runtime/node_definitions.h>

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace iv {
class ProjectAckBuilder;
class ProjectPersistenceBuilder;
class ProjectStringBuilder;
class SocketRpcAckResponseBuilder;
class SocketRpcCreateIvModuleInstanceResultBuilder;
class SocketRpcIvModuleInstancesResultBuilder;
struct ProjectCreateIvModuleInstanceRequest;
struct ProjectDeleteIvModuleInstanceRequest;
struct ProjectUpdateIvModuleInstancesRequest;
struct CreateIvModuleInstanceRequest;
struct DeleteIvModuleInstanceRequest;
struct GetIvModuleInstancesRequest;
struct UpdateIvModuleInstancesRequest;

struct IvModuleRequiredDefinition {
    std::string definition_id{};
    std::filesystem::path package_root{};
};

struct IvModuleRequiredDefinitionsChanged {
    std::vector<IvModuleRequiredDefinition> created{};
    std::vector<IvModuleRequiredDefinition> updated{};
    std::vector<std::string> deleted_definition_ids{};
};

// Durable instance metadata only. Executable roots, runtime bindings, scheduling
// state and lane prerequisites belong to the future project graph executor.
struct IvModuleInstance {
    std::string instance_id{};
    std::string definition_id{};
    std::string display_name{};
    std::filesystem::path package_root{};
    std::string module_id{};
};

struct IvModuleInstancesChanged {
    std::vector<IvModuleInstance> created{};
    std::vector<IvModuleInstance> updated{};
    std::vector<std::string> deleted_instance_ids{};
};

class IvModuleInstances {
public:
    struct Update {
        std::string instance_id{};
        std::optional<std::string> display_name{};
    };

private:
    struct DesiredInstance {
        std::string instance_id{};
        std::string definition_id{};
        std::string display_name{};
        std::filesystem::path package_root{};
    };

    mutable std::mutex mutex_;
    std::unordered_map<std::string, DesiredInstance> desired_instances_by_id_;
    std::unordered_map<std::string, IvModuleRequiredDefinition> required_definitions_by_id_;
    std::unordered_map<std::string, ModuleNodeDefinition> definitions_by_id_;
    std::unordered_map<std::string, IvModuleInstance> published_instances_by_id_;

    bool publish_instance_locked(
        std::string const &instance_id,
        ModuleNodeDefinition const &definition,
        IvModuleInstancesChanged &diff);
    void publish_instance_changes(IvModuleInstancesChanged diff, bool list_changed) const;
    void publish_instance_declarations_changed() const;

public:
    IvModuleInstances() = default;

    std::string create_instance(
        std::string_view definition_id,
        std::filesystem::path package_root,
        std::optional<std::string> instance_id = std::nullopt,
        std::optional<std::string> display_name = std::nullopt);
    void remove_instance(std::string const &instance_id);
    void update_instances(std::vector<Update> updates);
    [[nodiscard]] std::vector<IvModuleInstanceInfo> list_instances() const;

    void handle_iv_package_definitions_changed(IvPackageDefinitionsChanged const &diff);
    void handle_project_create_iv_module_instance(
        ProjectCreateIvModuleInstanceRequest const &request,
        ProjectStringBuilder &builder);
    void handle_project_delete_iv_module_instance(
        ProjectDeleteIvModuleInstanceRequest const &request,
        ProjectAckBuilder &builder);
    void handle_project_update_iv_module_instances(
        ProjectUpdateIvModuleInstancesRequest const &request,
        ProjectAckBuilder &builder);
    void handle_project_persistence_collect_state(ProjectPersistenceBuilder &builder) const;
    void handle_socket_rpc_create_iv_module_instance(
        CreateIvModuleInstanceRequest const &request,
        SocketRpcCreateIvModuleInstanceResultBuilder &builder);
    void handle_socket_rpc_delete_iv_module_instance(
        DeleteIvModuleInstanceRequest const &request,
        SocketRpcAckResponseBuilder &builder);
    void handle_socket_rpc_update_iv_module_instances(
        UpdateIvModuleInstancesRequest const &request,
        SocketRpcAckResponseBuilder &builder);
    void handle_socket_rpc_get_iv_module_instances(
        GetIvModuleInstancesRequest const &request,
        SocketRpcIvModuleInstancesResultBuilder &builder) const;
};
} // namespace iv
