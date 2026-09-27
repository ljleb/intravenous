#pragma once

#include <intravenous/node/resources.h>
#include <intravenous/runtime/background_evaluation_transaction.h>
#include <intravenous/runtime/background_coverage_propagation.h>
#include <intravenous/runtime/graph_jit.h>
#include <intravenous/runtime/persisted_page_store.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace iv {

enum class GraphExecutorStageResult : std::uint8_t {
    staged,
    ignored_stale,
};

// Mutable runtime owner for immutable CompiledGraph generations. Staging and
// activation are control-path operations: callers must activate only at a legal
// whole-root boundary with no concurrent tick_block() invocation. The realtime
// call itself performs no generation selection, allocation, or lifecycle work.
class GraphExecutor {
    struct Realization {
        std::shared_ptr<CompiledGraph const> graph{};
        NodeStorage storage{};
        BackgroundCoverageState coverage{};
        BackgroundPropagationWorkspace propagation;
        bool initialized = false;

        Realization(
            std::shared_ptr<CompiledGraph const> graph,
            ResourceContext const& resources);
    };

    ResourceContext resources_{};
    // Executor-level and deliberately outside either generation realization.
    // Compatible generations will rebind their persisted ports into this one
    // canonical sample/event authority rather than migrate page ownership.
    PersistedPageStore persisted_pages_{};
    std::array<std::optional<Realization>, 2> realizations_{};
    std::optional<std::size_t> active_{};
    std::optional<std::size_t> pending_{};

    [[nodiscard]] Realization& active_realization();
    [[nodiscard]] Realization const& active_realization() const;

public:
    explicit GraphExecutor(ResourceContext resources = {});
    ~GraphExecutor() = default;

    GraphExecutor(GraphExecutor const&) = delete;
    GraphExecutor& operator=(GraphExecutor const&) = delete;
    GraphExecutor(GraphExecutor&&) = delete;
    GraphExecutor& operator=(GraphExecutor&&) = delete;

    // Builds a pending runtime realization without reading mutable active
    // storage. A newer pending generation supersedes an older pending generation
    // without disturbing the active one.
    GraphExecutorStageResult stage(
        std::shared_ptr<CompiledGraph const> compiled_graph);

    // At the caller-provided quiescent boundary, snapshots/migrates the final
    // active state into the pending realization and publishes it. Returns false
    // when no generation is pending.
    bool activate_pending();

    [[nodiscard]] std::optional<std::uint64_t> active_generation() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> pending_generation() const noexcept;
    [[nodiscard]] std::shared_ptr<CompiledGraph const> active_graph() const noexcept;

    // Runs one complete prepared/evaluate/publish operation against the active
    // generation. No propagation-only commit surface is exposed.
    [[nodiscard]] std::expected<BackgroundEvaluationResult, std::string>
    evaluate_background(BackgroundEvaluationRequest request);

    // Executes only the already-active realization. Generation activation is
    // deliberately never hidden in this audio-thread entry point.
    void tick_block(std::size_t sample_index, std::size_t block_size);
};

} // namespace iv
