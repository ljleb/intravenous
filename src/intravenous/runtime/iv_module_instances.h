#pragma once

#include <intravenous/graph/build_types.h>
#include <intravenous/runtime/iv_module_instance_types.h>
#include <intravenous/runtime/iv_module_definitions.h>
#include <intravenous/runtime/lane_graph.h>

#include <filesystem>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace iv {
class IvModuleSources;

struct IvModuleRequiredDefinition {
    std::string definition_id{};
    std::filesystem::path module_root{};
};

struct IvModuleRequiredDefinitionsChanged {
    std::vector<IvModuleRequiredDefinition> created{};
    std::vector<IvModuleRequiredDefinition> updated{};
    std::vector<std::string> deleted_definition_ids{};
};

struct GraphInputLanesRebuildRequested;

struct IvModuleInstance {
    std::string instance_id{};
    std::string definition_id{};
    std::filesystem::path module_root{};
    std::string module_id{};
    GraphIntrospectionMetadata introspection{};
    std::optional<size_t> default_silence_ttl_samples{};
};

struct IvModuleInstancesChanged {
    std::vector<IvModuleInstance> created{};
    std::vector<IvModuleInstance> updated{};
    std::vector<std::string> deleted_instance_ids{};
};

struct IvModuleInstanceBuilderRef {
    IvModuleInstance const *instance = nullptr;
    GraphBuilder *builder = nullptr;
    // Keeps the binary generation that owns the builder's type-erased code live
    // until consumers have replaced and released their previous execution graph.
    std::vector<ModuleRef> module_refs {};
    std::vector<LaneId> prerequisite_lanes {};
    std::optional<size_t> default_silence_ttl_samples {};
};

struct IvModuleInstanceBuildersChanged {
    std::uint64_t version_index = 0;
    std::vector<IvModuleInstanceBuilderRef> created {};
    std::vector<IvModuleInstanceBuilderRef> updated {};
    std::vector<std::string> deleted_instance_ids {};
};

class IvModuleInstances {
    struct DesiredInstance {
        std::string instance_id{};
        std::string definition_id{};
        std::filesystem::path module_root{};
        std::optional<size_t> default_silence_ttl_samples{};
    };

    mutable std::mutex mutex;
    std::unordered_map<std::string, DesiredInstance> desired_instances_by_id;
    std::unordered_map<std::string, IvModuleRequiredDefinition> required_definitions_by_id;
    std::unordered_map<std::string, IvModuleInstance> realized_instances_by_id;
    std::unordered_map<std::string, std::vector<ModuleRef>> realized_module_refs_by_id;
    std::unordered_map<std::string, GraphBuilder> realized_builders_by_id;

public:
    IvModuleInstances() = default;

    std::string create_instance(
        std::string_view definition_id,
        std::filesystem::path module_root,
        std::optional<std::string> instance_id = std::nullopt);
    void remove_instance(std::string const &instance_id);
    void set_default_silence_ttl_samples(
        std::string const &instance_id,
        size_t default_silence_ttl_samples);
    void refresh_source_roots(IvModuleSources const &sources);
    [[nodiscard]] std::vector<IvModuleInstanceInfo> list_instances() const;

    void handle_iv_module_definitions_changed(
        IvModuleDefinitionsChanged const &diff);
    void handle_graph_input_lanes_rebuild_requested(
        GraphInputLanesRebuildRequested const &request);
};
} // namespace iv
