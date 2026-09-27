#include <intravenous/runtime/graph_executor.h>

#include <intravenous/node/layout.h>

#include <stdexcept>
#include <utility>

namespace iv {

GraphExecutor::Realization::Realization(
    std::shared_ptr<CompiledGraph const> compiled_graph,
    ResourceContext const& resources)
    : graph(std::move(compiled_graph))
    , storage(graph->node_layout.create_storage(resources))
    , coverage(graph->background_evaluation_plan.accumulators.output_change_count)
    , propagation(
        graph->background_evaluation_plan,
        graph->specialization.sample_rate)
{}

GraphExecutor::GraphExecutor(ResourceContext resources)
    : resources_(std::move(resources))
{}

GraphExecutor::Realization& GraphExecutor::active_realization()
{
    if (!active_) throw std::logic_error("GraphExecutor has no active generation");
    return *realizations_[*active_];
}

GraphExecutor::Realization const& GraphExecutor::active_realization() const
{
    if (!active_) throw std::logic_error("GraphExecutor has no active generation");
    return *realizations_[*active_];
}

GraphExecutorStageResult GraphExecutor::stage(
    std::shared_ptr<CompiledGraph const> compiled_graph)
{
    if (!compiled_graph) {
        throw std::invalid_argument("GraphExecutor cannot stage an empty compiled graph");
    }
    auto const newest_generation = pending_
        ? realizations_[*pending_]->graph->project_generation
        : active_ ? realizations_[*active_]->graph->project_generation
                  : std::uint64_t{0};
    if ((active_ || pending_)
        && compiled_graph->project_generation <= newest_generation) {
        return GraphExecutorStageResult::ignored_stale;
    }

    auto const index = active_ ? 1 - *active_ : std::size_t{0};
    pending_.reset();
    realizations_[index].emplace(std::move(compiled_graph), resources_);
    if (!active_) {
        realizations_[index]->storage.initialize();
        realizations_[index]->initialized = true;
    }
    pending_ = index;
    return GraphExecutorStageResult::staged;
}

bool GraphExecutor::activate_pending()
{
    if (!pending_) return false;
    auto& next = *realizations_[*pending_];
    if (!next.initialized) {
        if (active_) {
            auto migration = next.storage.migration_from(
                active_realization().storage);
            migration.commit();
        } else {
            next.storage.initialize();
        }
        next.initialized = true;
    }
    auto const previous = active_;
    active_ = pending_;
    pending_.reset();
    if (previous) realizations_[*previous].reset();
    return true;
}

std::optional<std::uint64_t> GraphExecutor::active_generation() const noexcept
{
    if (!active_) return std::nullopt;
    return realizations_[*active_]->graph->project_generation;
}

std::optional<std::uint64_t> GraphExecutor::pending_generation() const noexcept
{
    if (!pending_) return std::nullopt;
    return realizations_[*pending_]->graph->project_generation;
}

std::shared_ptr<CompiledGraph const> GraphExecutor::active_graph() const noexcept
{
    return active_ ? realizations_[*active_]->graph : nullptr;
}

void GraphExecutor::tick_block(std::size_t sample_index, std::size_t block_size)
{
    auto& realization = active_realization();
    if (block_size == 0
        || block_size > realization.graph->specialization.block_size) {
        throw std::invalid_argument(
            "GraphExecutor tick block size is outside the compiled specialization");
    }
    realization.graph->root_operations.tick_block(
        realization.storage.buffer().data(), sample_index, block_size);
}

} // namespace iv
