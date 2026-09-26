#pragma once

#include <intravenous/linker_event.h>
#include <intravenous/runtime/iv_module_instance_types.h>
#include <intravenous/runtime/runtime_project_api_types.h>

#include <filesystem>
#include <optional>
#include <vector>

namespace iv {
class IvModuleInstancesSourceFileFilterBuilder {
    std::optional<std::vector<IvModuleInstanceInfo>> result;

public:
    void succeed(std::vector<IvModuleInstanceInfo> value);
    [[nodiscard]] bool has_response() const;
    [[nodiscard]] std::vector<IvModuleInstanceInfo> build() const;
};

using IvModuleSourceIntrospectionNodesUpdatedEvent =
    void (*)(ProjectVirtualNodesNotification const&);
using IvModuleInstancesSourceFileFilterEvent =
    void (*)(std::filesystem::path const&,
             std::vector<IvModuleInstanceInfo> const&,
             IvModuleInstancesSourceFileFilterBuilder&);

IV_DECLARE_LINKER_EVENT(
    IvModuleSourceIntrospectionNodesUpdatedEvent,
    iv_runtime_iv_module_source_introspection_nodes_updated_event);
IV_DECLARE_LINKER_EVENT(
    IvModuleInstancesSourceFileFilterEvent,
    iv_runtime_iv_module_instances_source_file_filter_event);
} // namespace iv
