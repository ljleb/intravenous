#include <intravenous/runtime/node_definitions_events.h>

namespace iv {
IV_DEFINE_LINKER_EVENT(
    IvPackageDefinitionsChangedEvent,
    iv_runtime_iv_package_definitions_changed_event);
IV_DEFINE_LINKER_EVENT(
    NodeDefinitionsSnapshotChangedEvent,
    iv_runtime_node_definitions_snapshot_changed_event);
} // namespace iv
