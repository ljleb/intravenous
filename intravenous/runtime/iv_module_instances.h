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
struct RuntimeIvModuleRequiredDefinition {
    std::string definition_id{};
    std::filesystem::path module_root{};
};

struct RuntimeIvModuleRequiredDefinitionsChanged {
    std::vector<RuntimeIvModuleRequiredDefinition> created{};
    std::vector<RuntimeIvModuleRequiredDefinition> updated{};
    std::vector<std::string> deleted_definition_ids{};
};

struct RuntimeIvModuleInstance {
    std::string instance_id{};
    std::string definition_id{};
    std::filesystem::path module_root{};
    std::string module_id{};
    GraphIntrospectionMetadata introspection{};
};

struct RuntimeIvModuleInstancesChanged {
    std::vector<RuntimeIvModuleInstance> created{};
    std::vector<RuntimeIvModuleInstance> updated{};
    std::vector<std::string> deleted_instance_ids{};
};

class RuntimeIvModuleInstances {
    struct DesiredInstance {
        std::string instance_id{};
        std::string definition_id{};
        std::filesystem::path module_root{};
    };

    mutable std::mutex mutex;
    std::unordered_map<std::string, DesiredInstance> desired_instances_by_id;
    std::unordered_map<std::string, RuntimeIvModuleRequiredDefinition> required_definitions_by_id;
    std::unordered_map<std::string, RuntimeIvModuleInstance> realized_instances_by_id;
    size_t next_instance_id = 1;

public:
    RuntimeIvModuleInstances() = default;

    std::string create_instance(std::filesystem::path module_root);
    void remove_instance(std::string const &instance_id);
    [[nodiscard]] std::vector<RuntimeIvModuleInstanceInfo> list_instances() const;

    void handle_iv_module_definitions_changed(
        RuntimeIvModuleDefinitionsChanged const &diff);
};
} // namespace iv
