#pragma once

#include <intravenous/linker_event.h>
#include <intravenous/runtime/node_definitions.h>

namespace iv {
using IvPackageDefinitionsChangedEvent = void (*)(IvPackageDefinitionsChanged const&);
struct NodeDefinitionsSnapshotChanged {
    std::shared_ptr<NodeDefinitionsSnapshot const> snapshot{};
};
using NodeDefinitionsSnapshotChangedEvent = void (*)(NodeDefinitionsSnapshotChanged const&);

IV_DECLARE_LINKER_EVENT(
    IvPackageDefinitionsChangedEvent,
    iv_runtime_iv_package_definitions_changed_event);
IV_DECLARE_LINKER_EVENT(
    NodeDefinitionsSnapshotChangedEvent,
    iv_runtime_node_definitions_snapshot_changed_event);
} // namespace iv
