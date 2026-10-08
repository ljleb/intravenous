#pragma once

#include <intravenous/runtime/background_coverage_propagation.h>
#include <intravenous/runtime/async_capacity_manager.h>
#include <intravenous/runtime/graph_jit.h>
#include <intravenous/runtime/persisted_page_store.h>
#include <intravenous/runtime/tick_materialization_snapshot.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace iv {

// Transaction-local inputs selected by the background actor. Realtime-produced
// queue records are separate complete inputs and add their own coverage roots.
// A missing semantic version retains the pinned persisted snapshot's semantic
// environment.
struct BackgroundTransactionInputs {
    std::optional<std::uint64_t> semantic_version{};
    CoveragePropagationRequest roots{};
};

enum class BackgroundEvaluationStatus : std::uint8_t {
    committed,
    stale_base,
};

struct BackgroundEvaluationResult {
    BackgroundEvaluationStatus status = BackgroundEvaluationStatus::committed;
    CoveragePropagationResult coverage{};
    // Set only when this transaction published a successor page snapshot.
    std::optional<PersistedPageSnapshotVersion> published_pages{};
    // Set when the executor-backed transaction promoted its complete
    // Tick-visible materialization selection, including an empty selection.
    std::optional<std::uint64_t> promoted_tick_materialization{};
};

// Generation-local destination metadata aligned with one independently pinned
// realtime-producer queue. The block prefix remains owned by GraphExecutor and
// is released only after the transaction reports a committed result.
struct BackgroundProducedInputRoute {
    PersistedOutputHandle output{};
    graph_jit::BackgroundPortIndex port = 0;
    PortKind kind = PortKind::sample;
};

// One complete background operation. The implementation owns its reader pin,
// prepared semantic candidate, realized storage, invocation frame and any private
// page candidate until execute() either commits all publishable state or drops it.
// execute() releases the reader pin and prepared candidate before it returns. The
// generated root receives only transaction-local call-frame records.
class BackgroundEvaluationTransaction {
    class Impl;
    std::unique_ptr<Impl> impl_;

public:
    BackgroundEvaluationTransaction(CompiledGraph const& graph,
                                    std::byte* node_storage,
                                    BackgroundCoverageState& coverage,
                                    BackgroundPropagationWorkspace& propagation,
                                    PersistedPageStore& pages,
                                    BackgroundTransactionInputs inputs = {});
    BackgroundEvaluationTransaction(CompiledGraph const& graph,
                                    std::byte* node_storage,
                                    BackgroundCoverageState& coverage,
                                    BackgroundPropagationWorkspace& propagation,
                                    PersistedPageStore& pages,
                                    TickMaterializationStore& materializations,
                                    BackgroundTransactionInputs inputs = {});
    BackgroundEvaluationTransaction(CompiledGraph const& graph,
                                    std::byte* node_storage,
                                    BackgroundCoverageState& coverage,
                                    BackgroundPropagationWorkspace& propagation,
                                    PersistedPageStore& pages,
                                    TickMaterializationStore& materializations,
                                    BackgroundTransactionInputs inputs,
                                    std::span<BackgroundProducedInputRoute const>
                                        produced_routes,
                                    std::span<PinnedBlockPrefix const>
                                        produced_prefixes);
    ~BackgroundEvaluationTransaction();

    BackgroundEvaluationTransaction(BackgroundEvaluationTransaction const&) =
        delete;
    BackgroundEvaluationTransaction&
    operator=(BackgroundEvaluationTransaction const&) = delete;
    BackgroundEvaluationTransaction(BackgroundEvaluationTransaction&&) noexcept;
    BackgroundEvaluationTransaction&
    operator=(BackgroundEvaluationTransaction&&) noexcept;

    [[nodiscard]] std::expected<BackgroundEvaluationResult, std::string>
    execute();
};

} // namespace iv
