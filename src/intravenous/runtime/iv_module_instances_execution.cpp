#include <intravenous/runtime/iv_module_instances_execution.h>

namespace iv {
namespace {
GraphBuilder::RootNodeBuildResult build_execution_root(GraphBuilder &builder)
{
    auto const has_public_sample_inputs =
        !builder.public_sample_input_families().families.empty();
    auto const has_public_event_inputs =
        !builder.public_event_inputs().empty();
    auto const has_public_sample_outputs =
        !builder.public_sample_output_families().families.empty();
    auto const has_public_event_outputs =
        !builder.public_event_outputs().empty();

    if (!has_public_sample_inputs &&
        !has_public_event_inputs &&
        !has_public_sample_outputs &&
        !has_public_event_outputs) {
        return builder.build_root_node();
    }
    return builder.build_execution_root_node();
}
}

std::unique_ptr<BlockNodeExecutor> IvModuleInstancesExecution::make_executor(
    GraphBuilder &builder,
    size_t block_size,
    std::optional<size_t> default_silence_ttl_samples)
{
    return std::make_unique<BlockNodeExecutor>(
        BlockNodeExecutor::create(
            TypeErasedNode(build_execution_root(builder).graph),
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

VersionedTaskGraphUpdate IvModuleInstancesExecution::handle_instance_builders_changed(
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
        state.default_silence_ttl_samples = created.default_silence_ttl_samples;
        state.module_refs = created.module_refs;
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
        depends_on.reserve(created.prerequisite_lanes.size());
        for (auto const lane : created.prerequisite_lanes) {
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
        auto const ttl_changed =
            state.default_silence_ttl_samples != changed.default_silence_ttl_samples;
        state.default_silence_ttl_samples = changed.default_silence_ttl_samples;
        if (state.executor && !ttl_changed) {
            state.executor->reload(
                TypeErasedNode(build_execution_root(*changed.builder).graph));
        } else {
            state.executor = make_executor(
                *changed.builder,
                block_size_,
                state.default_silence_ttl_samples);
        }
        // Release the old graph before allowing its loaded binary generation to
        // go away. Its type-erased lifecycle functions live in that binary.
        state.module_refs = changed.module_refs;
        state.next_block_index = 0;
        auto &callback = callback_contexts_[changed.instance->instance_id];
        if (!callback) {
            callback = std::make_unique<InstanceTaskContext>();
        }
        callback->execution = this;
        callback->instance_id = changed.instance->instance_id;

        std::vector<std::string> depends_on;
        depends_on.reserve(changed.prerequisite_lanes.size());
        for (auto const lane : changed.prerequisite_lanes) {
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

    return VersionedTaskGraphUpdate{
        .version_index = diff.version_index,
        .update = std::move(update),
    };
}

void IvModuleInstancesExecution::resume(size_t start_index)
{
    std::scoped_lock lock(mutex_);
    for (auto &[instance_id, state] : instances_by_id_) {
        (void)instance_id;
        state.next_block_index = start_index;
    }
}
}
