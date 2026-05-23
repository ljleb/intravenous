#pragma once

#include "graph/build_types.h"
#include "runtime/iv_module_definitions.h"

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace iv {
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
    mutable std::mutex mutex;
    std::unordered_map<std::string, RuntimeIvModuleInstance> instances_by_definition_id;

public:
    RuntimeIvModuleInstances() = default;

    void handle_iv_module_definitions_changed(
        RuntimeIvModuleDefinitionsChanged const &diff);
};
} // namespace iv
