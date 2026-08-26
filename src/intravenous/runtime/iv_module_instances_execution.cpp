#include <intravenous/runtime/iv_module_instances_execution.h>

namespace iv {
namespace {
ResourceContext resources_for(
    std::shared_ptr<GraphRuntimeBindings> const& runtime_bindings)
{
    ResourceContext resources;
    if (runtime_bindings)
        resources.runtime_bindings = runtime_bindings->resources();
    return resources;
}
} // namespace

std::shared_ptr<BlockNodeExecutor> IvModuleInstancesExecution::make_executor(
    WeakTypeErasedNode root,
    std::shared_ptr<GraphRuntimeBindings> const& runtime_bindings,
    size_t block_size,
    size_t sample_rate,
    std::optional<size_t> default_silence_ttl_samples)
{
    return std::make_shared<BlockNodeExecutor>(
        BlockNodeExecutor::create(
            TypeErasedNode(root),
            block_size,
            resources_for(runtime_bindings),
            default_silence_ttl_samples,
            DEFAULT_EVENT_PORT_BUFFER_BASE_MULTIPLIER,
            sample_rate));
}

void IvModuleInstancesExecution::invoke_instance_task(void *raw_context)
{
    auto *context = static_cast<InstanceTaskContext *>(raw_context);
    if (!context || !context->executor) return;
    auto const block_index = context->next_block_index.fetch_add(
        context->executor->block_size(), std::memory_order_relaxed);
    context->executor->tick_block(block_index);
}

VersionedTaskGraphUpdate IvModuleInstancesExecution::handle_instance_builders_changed(
    IvModuleInstanceBuildersChanged const &diff)
{
    collect_retired_graphs();
    TaskGraphUpdate update;

    {
        std::scoped_lock lock(mutex_);
        retired_graphs_.reserve(
            retired_graphs_.size() + diff.updated.size() +
            diff.deleted_instance_ids.size());
    }

    for (auto const &created : diff.created) {
        if (!created.instance || !created.root) {
            continue;
        }
        auto executor = make_executor(
            created.root,
            created.instance->runtime_bindings,
            block_size_,
            sample_rate_,
            created.default_silence_ttl_samples);
        InstanceTaskContext* callback_ptr = nullptr;
        {
            std::scoped_lock lock(mutex_);
            auto &state = instances_by_id_[created.instance->instance_id];
            state.instance = created.instance;
            state.root = created.root;
            state.default_silence_ttl_samples =
                created.default_silence_ttl_samples;
            state.module_refs = created.module_refs;
            state.runtime_bindings = created.instance->runtime_bindings;
            state.executor = std::move(executor);
            state.pending_delete = false;
            state.active_context = std::make_unique<InstanceTaskContext>();
            state.active_context->executor = state.executor;
            callback_ptr = state.active_context.get();
        }

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
                .context = callback_ptr,
            },
        });
    }

    for (auto const &changed : diff.updated) {
        if (!changed.instance || !changed.root) {
            continue;
        }
        std::shared_ptr<BlockNodeExecutor> active_executor;
        {
            std::scoped_lock lock(mutex_);
            auto const it = instances_by_id_.find(changed.instance->instance_id);
            if (it != instances_by_id_.end()) active_executor = it->second.executor;
        }

        auto root = TypeErasedNode(changed.root);
        std::optional<BlockNodeExecutor::PreparedReload> prepared;
        std::shared_ptr<BlockNodeExecutor> replacement;
        if (active_executor) {
            prepared.emplace(active_executor->prepare_reload(
                std::move(root),
                changed.default_silence_ttl_samples,
                resources_for(changed.instance->runtime_bindings)));
        } else {
            replacement = std::make_shared<BlockNodeExecutor>(
                BlockNodeExecutor::create(
                    std::move(root),
                    block_size_,
                    resources_for(changed.instance->runtime_bindings),
                    changed.default_silence_ttl_samples,
                    DEFAULT_EVENT_PORT_BUFFER_BASE_MULTIPLIER,
                    sample_rate_));
        }

        auto pending_context = std::make_unique<InstanceTaskContext>();
        pending_context->executor = active_executor
            ? active_executor
            : replacement;
        auto* callback_ptr = pending_context.get();
        {
            std::scoped_lock lock(mutex_);
            auto &state = instances_by_id_[changed.instance->instance_id];
            if (active_executor && state.executor != active_executor)
                throw std::runtime_error(
                    "IV module executor changed while preparing reload");
            if (prepared) {
                state.pending_reload.emplace(InstanceTaskState::PendingReload{
                    .module_refs = changed.module_refs,
                    .runtime_bindings = changed.instance->runtime_bindings,
                    .instance = changed.instance,
                    .root = changed.root,
                    .default_silence_ttl_samples =
                        changed.default_silence_ttl_samples,
                    .prepared = std::move(*prepared),
                });
            } else {
                state.executor = std::move(replacement);
                state.instance = changed.instance;
                state.root = changed.root;
                state.default_silence_ttl_samples =
                    changed.default_silence_ttl_samples;
                state.module_refs = changed.module_refs;
                state.runtime_bindings = changed.instance->runtime_bindings;
                state.active_context = std::move(pending_context);
            }
            state.pending_delete = false;
            if (prepared)
                state.pending_context = std::move(pending_context);
        }

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
                .context = callback_ptr,
            },
        });
    }

    for (auto const &deleted : diff.deleted_instance_ids) {
        {
            std::scoped_lock lock(mutex_);
            auto const it = instances_by_id_.find(deleted);
            if (it != instances_by_id_.end())
                it->second.pending_delete = true;
        }
        update.to_delete.push_back(iv_module_instance_dsp_task_id(deleted));
    }

    return VersionedTaskGraphUpdate{
        .version_index = diff.version_index,
        .update = std::move(update),
        .activation_deferred = true,
    };
}

void IvModuleInstancesExecution::observe_completed_graph_revision(
    std::uint64_t graph_revision)
{
    auto observed = last_completed_graph_revision_.load(
        std::memory_order_relaxed);
    while (observed < graph_revision &&
           !last_completed_graph_revision_.compare_exchange_weak(
               observed, graph_revision,
               std::memory_order_release,
               std::memory_order_relaxed)) {
    }
}

void IvModuleInstancesExecution::commit_prepared_reloads(
    std::uint64_t graph_revision)
{
    observe_completed_graph_revision(graph_revision);
    std::scoped_lock lock(mutex_);
    for (auto& [instance_id, state] : instances_by_id_) {
        (void)instance_id;
        if (state.pending_delete) {
            retired_graphs_.push_back(RetiredGraph{
                .retired_from_revision = graph_revision,
                .module_refs = std::move(state.module_refs),
                .runtime_bindings = std::move(state.runtime_bindings),
                .active_context = std::move(state.active_context),
                .pending_context = std::move(state.pending_context),
                .executor = std::move(state.executor),
            });
            state.pending_delete = false;
            continue;
        }
        if (state.pending_reload && state.executor) {
            auto pending = std::move(*state.pending_reload);
            state.pending_reload.reset();
            auto retired_graph = state.executor->commit_reload(
                std::move(pending.prepared));
            retired_graphs_.push_back(RetiredGraph{
                .retired_from_revision = graph_revision,
                .module_refs = std::move(state.module_refs),
                .runtime_bindings = std::move(state.runtime_bindings),
                .active_context = std::move(state.active_context),
                .graph = std::move(retired_graph),
            });
            state.module_refs = std::move(pending.module_refs);
            state.runtime_bindings = std::move(pending.runtime_bindings);
            state.instance = pending.instance;
            state.root = pending.root;
            state.default_silence_ttl_samples =
                pending.default_silence_ttl_samples;
            state.active_context = std::move(state.pending_context);
        }
    }
}

void IvModuleInstancesExecution::collect_retired_graphs()
{
    std::vector<RetiredGraph> retired;
    {
        std::scoped_lock lock(mutex_);
        std::vector<RetiredGraph> still_referenced;
        still_referenced.reserve(retired_graphs_.size());
        retired.reserve(retired_graphs_.size());
        for (auto& graph : retired_graphs_) {
            if (graph.retired_from_revision <
                last_completed_graph_revision_.load(
                    std::memory_order_acquire))
                retired.push_back(std::move(graph));
            else
                still_referenced.push_back(std::move(graph));
        }
        retired_graphs_.swap(still_referenced);
        for (auto it = instances_by_id_.begin();
             it != instances_by_id_.end();) {
            if (!it->second.executor) it = instances_by_id_.erase(it);
            else ++it;
        }
    }
}

VersionedTaskGraphUpdate
IvModuleInstancesExecution::handle_runtime_dependencies_changed(
    GraphInputLanesRuntimeDependenciesChanged const& changed)
{
    std::scoped_lock lock(mutex_);
    TaskGraphUpdate update;
    update.to_update.reserve(changed.instances.size());
    for (auto const& instance : changed.instances) {
        if (!instances_by_id_.contains(instance.instance_id)) continue;
        std::vector<std::string> depends_on;
        depends_on.reserve(instance.prerequisite_lanes.size());
        for (auto const lane : instance.prerequisite_lanes)
            depends_on.push_back(timeline_lane_task_id(lane));
        update.to_update.push_back(TaskUpdateRecord{
            .id = iv_module_instance_dsp_task_id(instance.instance_id),
            .depends_on = std::move(depends_on),
        });
    }
    return VersionedTaskGraphUpdate{
        .version_index = changed.version_index,
        .update = std::move(update),
    };
}

void IvModuleInstancesExecution::resume(size_t start_index)
{
    std::scoped_lock lock(mutex_);
    for (auto &[instance_id, state] : instances_by_id_) {
        (void)instance_id;
        if (state.active_context)
            state.active_context->next_block_index.store(
                start_index, std::memory_order_relaxed);
        if (state.pending_context)
            state.pending_context->next_block_index.store(
                start_index, std::memory_order_relaxed);
    }
}

void IvModuleInstancesExecution::set_follows_transport_playhead(bool follows)
{
    std::scoped_lock lock(mutex_);
    follows_transport_playhead_ = follows;
}

void IvModuleInstancesExecution::synchronize_transport_playhead(size_t start_index)
{
    std::scoped_lock lock(mutex_);
    if (!follows_transport_playhead_) return;
    for (auto &[instance_id, state] : instances_by_id_) {
        (void)instance_id;
        if (state.active_context)
            state.active_context->next_block_index.store(
                start_index, std::memory_order_relaxed);
        if (state.pending_context)
            state.pending_context->next_block_index.store(
                start_index, std::memory_order_relaxed);
    }
}
}
