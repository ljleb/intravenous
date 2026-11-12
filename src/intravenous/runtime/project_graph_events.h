#pragma once

#include <intravenous/linker_event.h>
#include <intravenous/runtime/graph_connections.h>
#include <intravenous/runtime/graph_jit.h>
#include <intravenous/runtime/graph_executor.h>
#include <intravenous/runtime/iv_module_source_introspection_events.h>
#include <intravenous/runtime/node_instances.h>

namespace iv {
using ProjectGraphNodeInstancesRequestedEvent =
    void (*)(NodeInstancesProjectGraphRequest&);
using ProjectGraphConnectionsRequestedEvent =
    void (*)(GraphConnectionsProjectGraphRequest&);
using ProjectGraphSourceIntrospectionRequestedEvent =
    void (*)(IvModuleSourceIntrospectionProjectGraphRequest&);
using ProjectGraphGraphJitRequestedEvent =
    GraphJitCompileResult (*)(ProjectGraphGraphJitRequest const&);
using ProjectGraphGraphExecutorRequestedEvent =
    ProjectGraphGraphExecutorResult (*)(
        std::shared_ptr<CompiledGraph const> const&);
using ProjectGraphRealtimeCodeRequestedEvent =
    GraphExecutorStageResult (*)(std::shared_ptr<CompiledGraph const>);
using ProjectGraphManualControlTargetRequestedEvent = bool (*)(
    std::uint64_t, ManualControlId const&, Sample);

IV_DECLARE_LINKER_EVENT(
    ProjectGraphNodeInstancesRequestedEvent,
    iv_runtime_project_graph_node_instances_requested_event);
IV_DECLARE_LINKER_EVENT(
    ProjectGraphConnectionsRequestedEvent,
    iv_runtime_project_graph_connections_requested_event);
IV_DECLARE_LINKER_EVENT(
    ProjectGraphSourceIntrospectionRequestedEvent,
    iv_runtime_project_graph_source_introspection_requested_event);
IV_DECLARE_SINGLETON_EVENT(
    ProjectGraphGraphJitRequestedEvent,
    iv_runtime_project_graph_graph_jit_requested_event);
IV_DECLARE_SINGLETON_EVENT(
    ProjectGraphGraphExecutorRequestedEvent,
    iv_runtime_project_graph_graph_executor_requested_event);
IV_DECLARE_SINGLETON_EVENT(
    ProjectGraphRealtimeCodeRequestedEvent,
    iv_runtime_project_graph_realtime_code_requested_event);
IV_DECLARE_SINGLETON_EVENT(
    ProjectGraphManualControlTargetRequestedEvent,
    iv_runtime_project_graph_manual_control_target_requested_event);
} // namespace iv
