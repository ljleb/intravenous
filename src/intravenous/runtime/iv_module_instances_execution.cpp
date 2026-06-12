#include <intravenous/runtime/iv_module_instances_execution.h>

namespace iv {
std::unique_ptr<BlockNodeExecutor> IvModuleInstancesExecution::make_executor(
    GraphBuilder &builder,
    size_t block_size,
    std::optional<size_t> default_silence_ttl_samples)
{
    return std::make_unique<BlockNodeExecutor>(
        BlockNodeExecutor::create(
            TypeErasedNode(builder.build_root_node().graph),
            block_size,
            {},
            default_silence_ttl_samples));
}

void IvModuleInstancesExecution::invoke_instance_task(void *raw_context)
{
    auto *context = static_cast<InstanceTaskContext *>(raw_context);
    if (!context || !context->execution) {
        return;
    }

    std::scoped_lock lock(context->execution->mutex_);
    auto it = context->execution->instances_by_id_.find(context->instance_id);
    if (it == context->execution->instances_by_id_.end()) {
        return;
    }
    auto &state = it->second;
    if (!state.executor) {
        return;
    }
    state.executor->tick_block(state.next_block_index);
    state.next_block_index += context->execution->block_size_;
}

TaskGraphUpdate IvModuleInstancesExecution::handle_instance_builders_changed(
    IvModuleInstanceBuildersChanged const &diff)
{
    std::scoped_lock lock(mutex_);
    TaskGraphUpdate update;

    for (auto const &created : diff.created) {
        if (!created.instance || !created.builder) {
            continue;
        }
        auto &state = instances_by_id_[created.instance->instance_id];
        state.instance = created.instance;
        state.builder = created.builder;
        state.prerequisite_lanes = created.prerequisite_lanes;
        state.default_silence_ttl_samples = created.default_silence_ttl_samples;
        state.executor = make_executor(
            *created.builder,
            block_size_,
            state.default_silence_ttl_samples);
        state.next_block_index = 0;
        auto &callback = callback_contexts_[created.instance->instance_id];
        if (!callback) {
            callback = std::make_unique<InstanceTaskContext>();
        }
        callback->execution = this;
        callback->instance_id = created.instance->instance_id;

        std::vector<std::string> depends_on;
        depends_on.reserve(state.prerequisite_lanes.size());
        for (auto const lane : state.prerequisite_lanes) {
            depends_on.push_back(timeline_lane_task_id(lane));
        }

        update.to_create.push_back(TaskRecord {
            .id = iv_module_instance_dsp_task_id(created.instance->instance_id),
            .depends_on = std::move(depends_on),
            .callback = TaskCallback {
                .invoke = &IvModuleInstancesExecution::invoke_instance_task,
                .context = callback.get(),
            },
        });
    }

    for (auto const &changed : diff.updated) {
        if (!changed.instance || !changed.builder) {
            continue;
        }
        auto &state = instances_by_id_[changed.instance->instance_id];
        state.instance = changed.instance;
        state.builder = changed.builder;
        state.prerequisite_lanes = changed.prerequisite_lanes;
        auto const ttl_changed =
            state.default_silence_ttl_samples != changed.default_silence_ttl_samples;
        state.default_silence_ttl_samples = changed.default_silence_ttl_samples;
        if (state.executor && !ttl_changed) {
            state.executor->reload(TypeErasedNode(changed.builder->build_root_node().graph));
        } else {
            state.executor = make_executor(
                *changed.builder,
                block_size_,
                state.default_silence_ttl_samples);
        }
        state.next_block_index = 0;
        auto &callback = callback_contexts_[changed.instance->instance_id];
        if (!callback) {
            callback = std::make_unique<InstanceTaskContext>();
        }
        callback->execution = this;
        callback->instance_id = changed.instance->instance_id;

        std::vector<std::string> depends_on;
        depends_on.reserve(state.prerequisite_lanes.size());
        for (auto const lane : state.prerequisite_lanes) {
            depends_on.push_back(timeline_lane_task_id(lane));
        }

        update.to_update.push_back(TaskUpdateRecord {
            .id = iv_module_instance_dsp_task_id(changed.instance->instance_id),
            .depends_on = std::move(depends_on),
            .callback = TaskCallback {
                .invoke = &IvModuleInstancesExecution::invoke_instance_task,
                .context = callback.get(),
            },
        });
    }

    for (auto const &deleted : diff.deleted_instance_ids) {
        instances_by_id_.erase(deleted);
        callback_contexts_.erase(deleted);
        update.to_delete.push_back(iv_module_instance_dsp_task_id(deleted));
    }

    return update;
}

TaskGraphUpdate IvModuleInstancesExecution::handle_graph_input_lanes_dsp_task_dependencies_changed(
    GraphInputLanesDspTaskDependenciesChanged const &changed)
{
    std::scoped_lock lock(mutex_);
    TaskGraphUpdate update;

    for (auto const &deleted : changed.deleted_instance_ids) {
        if (!instances_by_id_.contains(deleted)) {
            continue;
        }
        instances_by_id_[deleted].prerequisite_lanes.clear();
        update.to_update.push_back(TaskUpdateRecord {
            .id = iv_module_instance_dsp_task_id(deleted),
            .depends_on = std::vector<std::string> {},
        });
    }

    for (auto const &entry : changed.created_or_updated) {
        auto it = instances_by_id_.find(entry.instance_id);
        if (it == instances_by_id_.end()) {
            continue;
        }
        it->second.prerequisite_lanes = entry.prerequisite_lanes;
        std::vector<std::string> depends_on;
        depends_on.reserve(entry.prerequisite_lanes.size());
        for (auto const lane : entry.prerequisite_lanes) {
            depends_on.push_back(timeline_lane_task_id(lane));
        }
        update.to_update.push_back(TaskUpdateRecord {
            .id = iv_module_instance_dsp_task_id(entry.instance_id),
            .depends_on = std::move(depends_on),
        });
    }

    return update;
}
}
