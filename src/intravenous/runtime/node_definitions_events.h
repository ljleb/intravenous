#pragma once

#include <intravenous/linker_event.h>
#include <intravenous/runtime/node_definitions.h>
#include <intravenous/runtime/project_graph_publication.h>

namespace iv {
// One definitions publication. `package_definitions` is what it changed among
// module and leaf definitions. ProjectGraph rebuilds from the snapshot and
// returns the build's read-model changes in `publication`.
struct NodeDefinitionsSnapshotChanged {
    std::shared_ptr<NodeDefinitionsSnapshot const> snapshot{};
    IvPackageDefinitionsChanged package_definitions{};
    ProjectGraphPublication publication{};
};
using NodeDefinitionsSnapshotChangedEvent = void (*)(NodeDefinitionsSnapshotChanged&);

IV_DECLARE_LINKER_EVENT(
    NodeDefinitionsSnapshotChangedEvent,
    iv_runtime_node_definitions_snapshot_changed_event);
} // namespace iv
