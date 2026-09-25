#pragma once

#include <intravenous/indexed_coverage.h>
#include <intravenous/node/resources.h>
#include <intravenous/runtime/graph_jit.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace iv {

enum class GraphExecutorStageResult : std::uint8_t {
    staged,
    ignored_stale,
};

// Generation-local roots for one exact indexed propagation batch. Endpoint and
// node ordinals are the immutable ordinals carried by the active CompiledGraph's
// IndexedPlan. Input/output-change roots carry the complete new coverage plus
// exact changed regions; an input root represents connection-set replacement,
// while node-local roots represent semantic changes with no changed random-access
// input. Output demands are background materialization roots; input demands are
// advance-preparation roots for compiler-planned prepared inputs.
struct GraphExecutorIndexedOutputChangeRoot {
    graph_jit::IndexedEndpointOrdinal endpoint = 0;
    IndexedCoverage coverage{};
    IndexedCoverage changed{};
};

struct GraphExecutorIndexedOutputDemandRoot {
    graph_jit::IndexedEndpointOrdinal endpoint = 0;
    IndexedCoverage required{};
};

struct GraphExecutorIndexedInputChangeRoot {
    graph_jit::IndexedEndpointOrdinal endpoint = 0;
    IndexedCoverage coverage{};
    IndexedCoverage changed{};
};

struct GraphExecutorIndexedInputDemandRoot {
    graph_jit::IndexedEndpointOrdinal endpoint = 0;
    IndexedCoverage required{};
};

struct GraphExecutorIndexedPropagationRequest {
    std::vector<graph_jit::IndexedNodeOrdinal> locally_changed_nodes{};
    std::vector<GraphExecutorIndexedInputChangeRoot> input_changes{};
    std::vector<GraphExecutorIndexedOutputChangeRoot> output_changes{};
    std::vector<GraphExecutorIndexedInputDemandRoot> input_demands{};
    std::vector<GraphExecutorIndexedOutputDemandRoot> output_demands{};
};

struct GraphExecutorIndexedOutputChange {
    graph_jit::IndexedEndpointOrdinal endpoint = 0;
    IndexedCoverage coverage{};
    IndexedCoverage changed{};
};

struct GraphExecutorIndexedOutputRequirement {
    graph_jit::IndexedEndpointOrdinal endpoint = 0;
    IndexedCoverage required{};
};

struct GraphExecutorIndexedInputRequirement {
    graph_jit::IndexedEndpointOrdinal endpoint = 0;
    IndexedCoverage required{};
};

// Propagation deliberately returns planning state, not payloads. A later
// materialization pass consumes these exact requirements through the physical
// indexed plan before any candidate page/version can be published.
struct GraphExecutorIndexedPropagationResult {
    std::vector<GraphExecutorIndexedOutputChange> output_changes{};
    std::vector<GraphExecutorIndexedInputRequirement> input_requirements{};
    std::vector<GraphExecutorIndexedOutputRequirement> output_requirements{};
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

    // Runs one background-only exact F/R batch against the active generation.
    // Calls are serialized by the owner and must not overlap activation. The
    // propagated semantic-coverage baseline advances only after both generated
    // traversals return successfully; no Tock/replay payload evaluation happens
    // here and no published page version advances.
    [[nodiscard]] GraphExecutorIndexedPropagationResult propagate_indexed(
        GraphExecutorIndexedPropagationRequest const& request);

    // Executes only the already-active realization. Generation activation is
    // deliberately never hidden in this audio-thread entry point.
    void tick_block(std::size_t sample_index, std::size_t block_size);
};

} // namespace iv
