#pragma once

#include <intravenous/node/resources.h>
#include <intravenous/runtime/graph_jit.h>

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
    class Impl;
    std::unique_ptr<Impl> impl_;

public:
    explicit GraphExecutor(ResourceContext resources = {});
    ~GraphExecutor();
    GraphExecutor(GraphExecutor&&) noexcept;
    GraphExecutor& operator=(GraphExecutor&&) noexcept;

    GraphExecutor(GraphExecutor const&) = delete;
    GraphExecutor& operator=(GraphExecutor const&) = delete;

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

    // Executes only the already-active realization. Generation activation is
    // deliberately never hidden in this audio-thread entry point.
    void tick_block(std::size_t sample_index, std::size_t block_size);
};

} // namespace iv
