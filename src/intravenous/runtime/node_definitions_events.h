#pragma once

#include <intravenous/linker_event.h>
#include <intravenous/runtime/node_definitions.h>


namespace iv {
using IvPackageDeclarationsChangedEvent =
    void (*)(IvPackageDeclarationsChanged const &);
using IvPackageDefinitionsChangedEvent =
    void (*)(IvPackageDefinitionsChanged const &);
struct NodeDefinitionsSnapshotChanged {
    std::shared_ptr<NodeDefinitionsSnapshot const> snapshot{};
};
using NodeDefinitionsSnapshotChangedEvent =
    void (*)(NodeDefinitionsSnapshotChanged const &);
struct IvPackageCatalogChanged {};
using IvPackageCatalogChangedEvent =
    void (*)(IvPackageCatalogChanged const &);

IV_DECLARE_LINKER_EVENT(
    IvPackageDeclarationsChangedEvent,
    iv_runtime_iv_package_declarations_changed_event);
IV_DECLARE_LINKER_EVENT(
    IvPackageDefinitionsChangedEvent,
    iv_runtime_iv_package_definitions_changed_event);
IV_DECLARE_LINKER_EVENT(
    NodeDefinitionsSnapshotChangedEvent,
    iv_runtime_node_definitions_snapshot_changed_event);
IV_DECLARE_LINKER_EVENT(
    IvPackageCatalogChangedEvent,
    iv_runtime_iv_package_catalog_changed_event);
} // namespace iv
