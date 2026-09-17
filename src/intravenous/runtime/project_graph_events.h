#pragma once

#include <intravenous/linker_event.h>
#include <intravenous/runtime/graph_connections.h>
#include <intravenous/runtime/graph_jit.h>
#include <intravenous/runtime/node_instances.h>

namespace iv {
using ProjectGraphNodeInstancesRequestedEvent =
    void (*)(NodeInstancesProjectGraphRequest&);
using ProjectGraphConnectionsRequestedEvent =
    void (*)(GraphConnectionsProjectGraphRequest&);
using ProjectGraphGraphJitRequestedEvent =
    GraphJitCompileResult (*)(GraphJitCompileRequest const&);

IV_DECLARE_LINKER_EVENT(
    ProjectGraphNodeInstancesRequestedEvent,
    iv_runtime_project_graph_node_instances_requested_event);
IV_DECLARE_LINKER_EVENT(
    ProjectGraphConnectionsRequestedEvent,
    iv_runtime_project_graph_connections_requested_event);
IV_DECLARE_SINGLETON_EVENT(
    ProjectGraphGraphJitRequestedEvent,
    iv_runtime_project_graph_graph_jit_requested_event);
} // namespace iv
