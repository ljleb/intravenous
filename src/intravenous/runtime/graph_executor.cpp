#include <intravenous/runtime/graph_executor.h>

#include <intravenous/node/layout.h>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace iv {

GraphExecutor::Realization::Realization(
    std::shared_ptr<CompiledGraph const> compiled_graph,
    ResourceContext const& resources,
    PersistedTickCaptureRegistry& captures)
    : graph(std::move(compiled_graph))
    , storage(graph->node_layout.create_storage(resources))
    , coverage(graph->background_evaluation_plan.accumulators.output_change_count)
    , propagation(
        graph->background_evaluation_plan,
        graph->specialization.sample_rate)
    , tick_invocation(
        graph->background_evaluation_plan,
        graph->project_generation,
        graph->specialization.block_size,
        &captures)
{}

GraphExecutor::GraphExecutor(
    ResourceContext resources,
    TickCaptureAllocatorConfig tick_capture_allocator)
    : resources_(std::move(resources))
    , tick_capture_allocator_(tick_capture_allocator)
    , tick_page_reader_(persisted_pages_.register_reader())
    , tick_materialization_reader_(tick_materializations_.register_reader())
{
    if (tick_capture_allocator_.low_watermark_callbacks == 0
        || tick_capture_allocator_.high_watermark_callbacks
            <= tick_capture_allocator_.low_watermark_callbacks
        || tick_capture_allocator_.slab_allocation_granularity == 0) {
        throw std::invalid_argument(
            "GraphExecutor has an invalid Tick capture allocator policy");
    }
}

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

std::size_t GraphExecutor::maximum_capture_blocks_per_callback() const noexcept
{
    std::size_t maximum = 0;
    if (active_) {
        maximum = realizations_[*active_]
            ->tick_invocation.maximum_capture_blocks_per_callback();
    }
    if (pending_) {
        maximum = std::max(
            maximum,
            realizations_[*pending_]
                ->tick_invocation.maximum_capture_blocks_per_callback());
    }
    return maximum;
}

TickCaptureReservePolicy GraphExecutor::tick_capture_reserve_policy(
    std::size_t maximum_blocks_per_callback) const
{
    if (maximum_blocks_per_callback == 0) {
        return {
            .slab_allocation_granularity =
                tick_capture_allocator_.slab_allocation_granularity,
        };
    }
    if (maximum_blocks_per_callback
            > std::numeric_limits<std::size_t>::max()
                / tick_capture_allocator_.high_watermark_callbacks) {
        throw std::length_error(
            "Tick capture allocator watermarks are too large");
    }
    return {
        .maximum_blocks_per_callback = maximum_blocks_per_callback,
        .low_watermark = maximum_blocks_per_callback
            * tick_capture_allocator_.low_watermark_callbacks,
        .high_watermark = maximum_blocks_per_callback
            * tick_capture_allocator_.high_watermark_callbacks,
        .slab_allocation_granularity =
            tick_capture_allocator_.slab_allocation_granularity,
    };
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
    realizations_[index].emplace(
        std::move(compiled_graph), resources_, persisted_tick_captures_);
    auto const maximum_blocks_per_callback = std::max(
        active_ ? active_realization().tick_invocation
                      .maximum_capture_blocks_per_callback()
                : std::size_t{0},
        realizations_[index]->tick_invocation
            .maximum_capture_blocks_per_callback());
    static_cast<void>(
        tick_captures_.maintain_free_block_reserve(
            tick_capture_reserve_policy(maximum_blocks_per_callback)));
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

std::expected<BackgroundEvaluationResult, std::string>
GraphExecutor::evaluate_background(BackgroundEvaluationRequest request)
{
    auto& realization = active_realization();
    BackgroundEvaluationTransaction transaction{
        *realization.graph,
        realization.storage.buffer().data(),
        realization.coverage,
        realization.propagation,
        persisted_pages_,
        tick_materializations_,
        std::move(request),
    };
    return transaction.execute();
}

GraphExecutorReclaimedSnapshots GraphExecutor::reclaim_retired_snapshots()
{
    return {
        .persisted_pages = persisted_pages_.reclaim_retired(),
        .tick_materializations = tick_materializations_.reclaim_retired(),
        .tick_captures = tick_captures_.reclaim_committed(),
    };
}

std::size_t GraphExecutor::maintain_tick_capture_reserve()
{
    auto const maximum = maximum_capture_blocks_per_callback();
    return tick_captures_.maintain_free_block_reserve(
        tick_capture_reserve_policy(maximum));
}

TickCaptureReservationFailures
GraphExecutor::tick_capture_reservation_failures() const noexcept
{
    return tick_captures_.reservation_failures();
}

void GraphExecutor::tick_block(std::size_t sample_index, std::size_t block_size)
{
    auto& realization = active_realization();
    if (block_size == 0
        || block_size > realization.graph->specialization.block_size) {
        throw std::invalid_argument(
            "GraphExecutor tick block size is outside the compiled specialization");
    }
    TickInvocationFrame invocation{
        tick_page_reader_,
        tick_materialization_reader_,
        realization.tick_invocation,
        sample_index,
        block_size};
    realization.graph->root_operations.tick_block(
        realization.storage.buffer().data(),
        &invocation.call(),
        sample_index,
        block_size);
}

} // namespace iv
