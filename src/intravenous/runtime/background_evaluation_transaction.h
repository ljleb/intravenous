#pragma once

#include <intravenous/runtime/background_coverage_propagation.h>
#include <intravenous/runtime/async_capacity_manager.h>
#include <intravenous/runtime/graph_jit.h>
#include <intravenous/runtime/persisted_data_store.h>
#include <intravenous/runtime/tick_materialization_snapshot.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>

namespace iv {
class BackgroundStorageRealization;

// Raised only for an error that invalidates the executable for every
// playback demand. Ordinary data, allocation and deadline failures do not
// carry this guarantee.
class TerminalBackgroundPreparationError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
struct BackgroundTransactionProjection {
    void* data = nullptr;
    void (*project)(void*, graph_jit::BackgroundEvaluationPlan const&,
        PreparedCoveragePropagation const&, BackgroundStorageRealization const&,
        PersistedDataVersion) = nullptr;
};

// Generation-local destination metadata aligned with one independently pinned
// realtime-producer queue. The block prefix remains owned by GraphExecutor and
// is released only after the transaction reports a committed result.
struct BackgroundProducedInputRoute {
    PersistedOutputHandle output{};
    graph_jit::BackgroundPortIndex port = 0;
    PortKind kind = PortKind::sample;
};

// Transaction-local inputs selected by the background actor. Realtime-produced
// queue records are separate complete inputs and add their own coverage roots.
// A missing semantic version retains the pinned persisted snapshot's semantic
// environment.
struct BackgroundTransactionInputs {
    std::optional<std::uint64_t> semantic_version{};
    CoveragePropagationRequest roots{};
    // Generation-cutover reconciliation removes these destinations only after
    // every closed predecessor queue has committed its finite tail.
    std::span<PersistedOutputHandle const> retired_outputs{};
    // Evaluate a pending generation privately. No canonical data root or
    // materialization is published until its audio activation.
    bool prepare_only = false;
    PersistedDataStore::Snapshot const* reusable_data = nullptr;
    TickMaterializationSnapshot const* reusable_materialization = nullptr;
    // Projects only while transaction views and the immutable data pin live.
    // The caller publishes the independently owned result only after commit.
    BackgroundTransactionProjection projection{};
    // Optional background-query budget. Playback retains its normal policy.
    std::optional<std::size_t> maximum_materialized_sample_bytes{};
    // Pins imported code, immutable configuration and resource ownership for
    // reconstructible results. Component callers without an executable owner
    // keep their results transaction-scoped rather than admitting a cache.
    std::shared_ptr<CompiledGraph const> reconstruction_graph{};
    // Historical reconstruction uses these exact recorded boundaries instead
    // of the current canonical recording. It is always a private transaction.
    std::shared_ptr<PersistedDataStore::Reconstruction const> reconstruction{};

};

enum class BackgroundEvaluationStatus : std::uint8_t {
    committed,
    stale_base,
    terminal_preparation_error,
};

struct BackgroundEvaluationResult {
    BackgroundEvaluationStatus status = BackgroundEvaluationStatus::committed;
    std::string preparation_error{};
    CoveragePropagationResult coverage{};
    // Set only when this transaction published a successor retained-data root.
    std::optional<PersistedDataVersion> published_data{};
    // Set when the executor-backed transaction promoted its complete
    // Tick-visible materialization selection, including an empty selection.
    std::optional<std::uint64_t> promoted_tick_materialization{};
    std::unique_ptr<PersistedDataStore::Snapshot> prepared_data{};
    std::unique_ptr<TickMaterializationSnapshot> prepared_materialization{};
};

// One complete background operation. The implementation owns its reader pin,
// prepared semantic candidate, realized storage, invocation frame and any private
// retained-data candidate until execute() either commits all publishable state
// or drops it.
// execute() releases the reader pin and prepared candidate before it returns. The
// generated root receives only transaction-local call-frame records.
class BackgroundEvaluationTransaction {
    class Impl;
    std::unique_ptr<Impl> impl_;

public:
    [[nodiscard]] static std::expected<BackgroundEvaluationResult, std::string>
    reconstruct(PersistedDataStore& data,
        std::shared_ptr<PersistedDataStore::Reconstruction const> context,
        graph_jit::BackgroundPortIndex output, IndexCoverage required,
        ResourceContext const& resources);
    BackgroundEvaluationTransaction(CompiledGraph const& graph,
                                    TockStateStorage& tock_states,
                                    BackgroundCoverageState& coverage,
                                    BackgroundPropagationWorkspace& propagation,
                                    PersistedDataStore& data,
                                    BackgroundTransactionInputs inputs = {});
    BackgroundEvaluationTransaction(CompiledGraph const& graph,
                                    TockStateStorage& tock_states,
                                    BackgroundCoverageState& coverage,
                                    BackgroundPropagationWorkspace& propagation,
                                    PersistedDataStore& data,
                                    TickMaterializationStore& materializations,
                                    BackgroundTransactionInputs inputs = {});
    BackgroundEvaluationTransaction(CompiledGraph const& graph,
                                    TockStateStorage& tock_states,
                                    BackgroundCoverageState& coverage,
                                    BackgroundPropagationWorkspace& propagation,
                                    PersistedDataStore& data,
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
