#pragma once

#include <intravenous/node/block_executor.h>
#include <intravenous/runtime/iv_module_instances.h>
#include <intravenous/runtime/task_ids.h>
#include <intravenous/runtime/task_runner.h>
#include <intravenous/runtime/timeline_events.h>

#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace iv {
class IvModuleInstancesExecution {
    struct InstanceTaskState {
        IvModuleInstance const *instance = nullptr;
        GraphBuilder *builder = nullptr;
        std::vector<LaneId> prerequisite_lanes {};
        std::optional<size_t> default_silence_ttl_samples {};
        std::unique_ptr<BlockNodeExecutor> executor {};
        size_t next_block_index = 0;
    };

    struct InstanceTaskContext {
        IvModuleInstancesExecution *execution = nullptr;
        std::string instance_id {};
    };

    size_t block_size_ = 256;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, InstanceTaskState> instances_by_id_;
    std::unordered_map<std::string, std::unique_ptr<InstanceTaskContext>> callback_contexts_;

    static void invoke_instance_task(void *);
    static std::unique_ptr<BlockNodeExecutor> make_executor(
        GraphBuilder &builder,
        size_t block_size,
        std::optional<size_t> default_silence_ttl_samples);

public:
    explicit IvModuleInstancesExecution(size_t block_size = 256)
      : block_size_(block_size)
    {}

    VersionedTaskGraphUpdate handle_instance_builders_changed(
        IvModuleInstanceBuildersChanged const &diff);
    VersionedTaskGraphUpdate handle_timeline_batch(TimelineLaneBatchUpdate const &batch);
};
}
