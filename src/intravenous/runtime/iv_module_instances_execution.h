#pragma once

#include <intravenous/node/block_executor.h>
#include <intravenous/runtime/graph_input_lanes_events.h>
#include <intravenous/runtime/iv_module_instances.h>
#include <intravenous/runtime/task_ids.h>
#include <intravenous/runtime/task_runner.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace iv {
struct PauseRequest;
struct ResumeRequest;
struct TasksRunnerAfterPass;
struct TimelineExecutionResumed;

class IvModuleInstancesExecution {
    struct InstanceTaskContext {
        std::shared_ptr<BlockNodeExecutor> executor {};
        std::atomic<size_t> next_block_index {0};
    };

    struct InstanceTaskState {
        struct PendingReload {
            // Destruction is reverse declaration order: prepared graph state
            // must be gone before its binary generations are released.
            std::vector<ModuleRef> module_refs {};
            std::shared_ptr<GraphRuntimeBindings> runtime_bindings {};
            IvModuleInstance const* instance = nullptr;
            WeakTypeErasedNode root {};
            std::optional<size_t> default_silence_ttl_samples {};
            BlockNodeExecutor::PreparedReload prepared;
        };

        IvModuleInstance const *instance = nullptr;
        WeakTypeErasedNode root {};
        std::optional<size_t> default_silence_ttl_samples {};
        std::vector<ModuleRef> module_refs {};
        std::shared_ptr<GraphRuntimeBindings> runtime_bindings {};
        std::shared_ptr<BlockNodeExecutor> executor {};
        std::optional<PendingReload> pending_reload {};
        std::unique_ptr<InstanceTaskContext> active_context {};
        std::unique_ptr<InstanceTaskContext> pending_context {};
        bool pending_delete = false;
    };

    struct RetiredGraph {
        std::uint64_t retired_from_revision = 0;
        // Destruction is reverse declaration order: graph state must be gone
        // before the binary generations containing its callbacks are released.
        std::vector<ModuleRef> module_refs {};
        std::shared_ptr<GraphRuntimeBindings> runtime_bindings {};
        std::unique_ptr<InstanceTaskContext> active_context {};
        std::unique_ptr<InstanceTaskContext> pending_context {};
        std::shared_ptr<BlockNodeExecutor> executor {};
        BlockNodeExecutor::RetiredState graph {};
    };

    size_t block_size_ = 256;
    size_t sample_rate_ = 48000;
    bool follows_transport_playhead_ = true;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, InstanceTaskState> instances_by_id_;
    std::vector<RetiredGraph> retired_graphs_;
    std::atomic<std::uint64_t> last_completed_graph_revision_ {0};

    void collect_retired_graphs();

    static void invoke_instance_task(void *);
    static std::shared_ptr<BlockNodeExecutor> make_executor(
        WeakTypeErasedNode root,
        std::shared_ptr<GraphRuntimeBindings> const& runtime_bindings,
        size_t block_size,
        size_t sample_rate,
        std::optional<size_t> default_silence_ttl_samples);

public:
    explicit IvModuleInstancesExecution(
        size_t block_size = 256,
        bool initially_follows_transport_playhead = true,
        size_t sample_rate = 48000)
      : block_size_(block_size),
        sample_rate_(sample_rate),
        follows_transport_playhead_(initially_follows_transport_playhead)
    {}

    VersionedTaskGraphUpdate handle_instance_builders_changed(
        IvModuleInstanceBuildersChanged const &diff);
    VersionedTaskGraphUpdate handle_runtime_dependencies_changed(
        GraphInputLanesRuntimeDependenciesChanged const &changed);
    void handle_iv_module_instance_builders_completed(
        IvModuleInstanceBuildersChanged const &changed);
    void handle_graph_input_lanes_runtime_dependencies_changed(
        GraphInputLanesRuntimeDependenciesChanged const &changed);
    void handle_pause(PauseRequest const &request);
    void handle_resume(ResumeRequest const &request);
    void handle_timeline_execution_resumed(TimelineExecutionResumed const &resumed);
    void handle_task_runner_after_pass(TasksRunnerAfterPass const &pass);
    void observe_completed_graph_revision(std::uint64_t graph_revision);
    void commit_prepared_reloads(std::uint64_t graph_revision = 0);
    void resume(size_t start_index);
    void set_follows_transport_playhead(bool follows);
    void synchronize_transport_playhead(size_t start_index);
};
}
