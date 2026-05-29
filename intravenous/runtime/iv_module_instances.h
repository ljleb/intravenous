#pragma once

#include "graph/build_types.h"
#include "runtime/iv_module_instance_types.h"
#include "runtime/iv_module_definitions.h"

#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace iv {
struct IvModuleRequiredDefinition {
    std::string definition_id{};
    std::filesystem::path module_root{};
};

struct IvModuleRequiredDefinitionsChanged {
    std::vector<IvModuleRequiredDefinition> created{};
    std::vector<IvModuleRequiredDefinition> updated{};
    std::vector<std::string> deleted_definition_ids{};
};

struct IvModuleInstance {
    std::string instance_id{};
    std::string definition_id{};
    std::filesystem::path module_root{};
    std::string module_id{};
    GraphIntrospectionMetadata introspection{};
};

struct IvModuleInstancesChanged {
    std::vector<IvModuleInstance> created{};
    std::vector<IvModuleInstance> updated{};
    std::vector<std::string> deleted_instance_ids{};
};

struct IvModuleInstanceBuilder {
    IvModuleInstance instance {};
    GraphBuilder builder {};
};

struct IvModuleInstanceBuildersChanged {
    std::vector<IvModuleInstanceBuilder> created {};
    std::vector<IvModuleInstanceBuilder> updated {};
    std::vector<std::string> deleted_instance_ids {};
};

class IvModuleInstances {
    struct DesiredInstance {
        std::string instance_id{};
        std::string definition_id{};
        std::filesystem::path module_root{};
    };

    mutable std::mutex mutex;
    std::unordered_map<std::string, DesiredInstance> desired_instances_by_id;
    std::unordered_map<std::string, IvModuleRequiredDefinition> required_definitions_by_id;
    std::unordered_map<std::string, IvModuleInstance> realized_instances_by_id;
    size_t next_instance_id = 1;

public:
    IvModuleInstances() = default;

    std::string create_instance(std::filesystem::path module_root);
    void remove_instance(std::string const &instance_id);
    [[nodiscard]] std::vector<IvModuleInstanceInfo> list_instances() const;

    void handle_iv_module_definitions_changed(
        IvModuleDefinitionsChanged const &diff);
};
} // namespace iv
