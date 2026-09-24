#include <intravenous/runtime/graph_executor.h>

#include <intravenous/node/layout.h>

#include <stdexcept>
#include <utility>

namespace iv {
namespace {

void validate_compiled_graph(CompiledGraph const& graph)
{
    if (!graph.root_operations.valid()) {
        throw std::invalid_argument(
            "GraphExecutor requires a valid compiled root operation");
    }
    if (graph.specialization.block_size == 0) {
        throw std::invalid_argument(
            "GraphExecutor requires a non-zero compiled block size");
    }
    if (graph.node_layout.max_block_size == 0
        || graph.node_layout.max_block_size > graph.specialization.block_size) {
        throw std::invalid_argument(
            "GraphExecutor compiled NodeLayout block size is invalid");
    }
}

} // namespace

class GraphExecutor::Impl {
    struct Realization {
        std::shared_ptr<CompiledGraph const> graph{};
        NodeStorage storage{};

        Realization(
            std::shared_ptr<CompiledGraph const> graph_,
            ResourceContext const& resources)
            : graph(std::move(graph_))
            , storage(graph->node_layout.create_storage(resources))
        {}
    };

    struct PendingRealization {
        std::unique_ptr<Realization> realization{};
        bool initialized = false;
    };

public:
    // NodeStorage retains a pointer to this value, so it lives in the stable
    // heap-allocated Impl rather than directly inside movable GraphExecutor.
    ResourceContext resources{};
    std::unique_ptr<Realization> active{};
    std::optional<PendingRealization> pending{};

    explicit Impl(ResourceContext resources_)
        : resources(std::move(resources_))
    {}

    GraphExecutorStageResult stage(
        std::shared_ptr<CompiledGraph const> compiled_graph)
    {
        if (!compiled_graph) {
            throw std::invalid_argument(
                "GraphExecutor cannot stage an empty compiled graph");
        }
        validate_compiled_graph(*compiled_graph);

        auto const newest_generation = pending
            ? pending->realization->graph->project_generation
            : active
                ? active->graph->project_generation
                : std::uint64_t{0};
        if ((active || pending)
            && compiled_graph->project_generation <= newest_generation) {
            return GraphExecutorStageResult::ignored_stale;
        }

        PendingRealization candidate{
            .realization = std::make_unique<Realization>(
                std::move(compiled_graph), resources),
        };
        if (!active) {
            candidate.realization->storage.initialize();
            candidate.initialized = true;
        }
        pending = std::move(candidate);
        return GraphExecutorStageResult::staged;
    }

    bool activate_pending()
    {
        if (!pending) return false;
        if (!pending->initialized) {
            if (!active) {
                pending->realization->storage.initialize();
            } else {
                auto migration =
                    pending->realization->storage.prepare_migration_from(
                        active->storage);
                migration.commit();
            }
            pending->initialized = true;
        }

        auto previous = std::move(active);
        active = std::move(pending->realization);
        pending.reset();
        // Keep the previous code/layout alive through migration commit and
        // release. Its realization is destroyed only after publication.
        previous.reset();
        return true;
    }

    void tick_block(std::size_t sample_index, std::size_t block_size)
    {
        if (!active) {
            throw std::logic_error(
                "GraphExecutor cannot execute without an active generation");
        }
        if (block_size == 0
            || block_size > active->graph->specialization.block_size) {
            throw std::invalid_argument(
                "GraphExecutor tick block size is outside the compiled specialization");
        }
        active->graph->root_operations.tick_block(
            active->storage.buffer().data(), sample_index, block_size);
    }
};

GraphExecutor::GraphExecutor(ResourceContext resources)
    : impl_(std::make_unique<Impl>(std::move(resources)))
{}

GraphExecutor::~GraphExecutor() = default;
GraphExecutor::GraphExecutor(GraphExecutor&&) noexcept = default;
GraphExecutor& GraphExecutor::operator=(GraphExecutor&&) noexcept = default;

GraphExecutorStageResult GraphExecutor::stage(
    std::shared_ptr<CompiledGraph const> compiled_graph)
{
    return impl_->stage(std::move(compiled_graph));
}

bool GraphExecutor::activate_pending()
{
    return impl_->activate_pending();
}

std::optional<std::uint64_t> GraphExecutor::active_generation() const noexcept
{
    if (!impl_->active) return std::nullopt;
    return impl_->active->graph->project_generation;
}

std::optional<std::uint64_t> GraphExecutor::pending_generation() const noexcept
{
    if (!impl_->pending) return std::nullopt;
    return impl_->pending->realization->graph->project_generation;
}

std::shared_ptr<CompiledGraph const> GraphExecutor::active_graph() const noexcept
{
    return impl_->active ? impl_->active->graph : nullptr;
}

void GraphExecutor::tick_block(
    std::size_t sample_index,
    std::size_t block_size)
{
    impl_->tick_block(sample_index, block_size);
}

} // namespace iv
