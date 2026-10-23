#pragma once

#include <intravenous/graph/configured_graph.hpp>
#include <intravenous/graph/realtime_port_planning.h>
#include <intravenous/graph_jit/background_evaluation_call.h>
#include <intravenous/graph_jit/background_evaluation_plan.h>
#include <intravenous/graph_jit/port_realization.h>
#include <intravenous/graph_jit/realtime_port_state_plan.h>
#include <intravenous/graph_jit/realtime_port_state_transition.h>
#include <intravenous/graph_jit/tick_invocation_call.h>
#include <intravenous/node/layout.h>
#include <intravenous/runtime/node_definition_types.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace iv {

struct GraphJitConfig {
    std::size_t sample_rate = 48000;
    std::size_t block_size = 256;
    // One compiler-wide policy for realtime sample/event buffer placement.
    // stack_budget_bytes is enforced against the packed generated-root stack,
    // not independently against every buffer choice.
    RealtimeStorageCostModel realtime_storage_cost_model{};
};

// Immutable specialization facts for one project kernel. CPU/features are
// captured by GraphJit from the host used to create its persistent ORC domain.
struct GraphJitKernelSpecialization {
    std::size_t sample_rate = 0;
    std::size_t block_size = 0;
    std::string target_triple{};
    std::string target_cpu{};
    std::string target_features{};
};

enum class GraphJitDiagnosticStage : std::uint8_t {
    input_capture,
    package_llvm,
    lowering,
    optimization,
    materialization,
};

struct GraphJitDiagnostic {
    GraphJitDiagnosticStage stage = GraphJitDiagnosticStage::input_capture;
    std::string message{};
    std::size_t node_bundle = static_cast<std::size_t>(-1);
    std::string node_type_id{};
    std::string package_root{};
};

// Generated zero-input/zero-output root-node execution ABI. Mutable bytes are
// owned by GraphExecutor through NodeStorage created from CompiledGraph::node_layout.
// Dynamic background-derived inputs arrive only as resolved views in one
// callback-scoped invocation record. Realtime lifecycle remains in NodeStorage;
// background TockState lifecycle belongs to its separate sidecar. Callers must
// provide block_size > 0; an empty audio block is outside the root-operation ABI.
using CompiledGraphBlockFunction = void (*)(
    std::byte* storage_base,
    graph_jit::TickInvocationCall const* invocation,
    GlobalIndex sample_index,
    std::size_t block_size);

struct CompiledGraphRootOperations {
    CompiledGraphBlockFunction tick_block = nullptr;
    CompiledGraphBlockFunction transition_tick_block = nullptr;

    [[nodiscard]] bool valid() const noexcept
    {
        return tick_block != nullptr;
    }
};

enum class CompiledGraphRealtimePhase : std::uint8_t {
    transition,
    steady,
};

struct CompiledGraph;

// Cold cutover program specialized against one exact active predecessor
// realization. The weak predecessor reference validates staging without
// retaining obsolete generated code for the successor's entire lifetime.
struct CompiledGraphCutover {
    std::weak_ptr<CompiledGraph const> predecessor{};
    CompiledGraphRealtimePhase predecessor_phase =
        CompiledGraphRealtimePhase::steady;
    graph_jit::RealtimePortStateTransitionPlan state_transition{};
    graph_jit::RealtimePortStateRealizations transition_realizations{};
};

// Background roots consume one executor-owned logical batch. Each authored
// Tock context already contains its background-owned TockState sidecar view;
// generated code has no NodeStorage base and cannot address realtime State.
using CompiledGraphBackgroundFunction =
    void (*)(graph_jit::BackgroundEvaluationCall* batch);

struct CompiledGraphBackgroundOperations {
    CompiledGraphBackgroundFunction propagate_forward = nullptr;
    CompiledGraphBackgroundFunction propagate_reverse = nullptr;
    CompiledGraphBackgroundFunction evaluate = nullptr;

    [[nodiscard]] bool valid() const noexcept
    {
        return propagate_forward != nullptr
            && propagate_reverse != nullptr
            && evaluate != nullptr;
    }
};

// One immutable native project generation. GraphJit owns code/layout/planning
// metadata only; GraphExecutor creates realtime NodeStorage plus the
// background-owned TockState sidecar and owns their separate lifecycles.
// Generated code remains pinned by code_lifetime.
struct CompiledGraph {
    std::uint64_t project_generation = 0;
    std::uint64_t definitions_generation = 0;
    GraphJitKernelSpecialization specialization{};
    std::shared_ptr<ConfiguredGraph const> configured_graph{};
    std::vector<std::shared_ptr<PackageRevision const>> package_revisions{};
    graph_jit::ResolvedPortRealization port_realization{};
    NodeLayout node_layout{};
    TockStateLayout tock_state_layout{};
    graph_jit::BackgroundEvaluationPlan background_evaluation_plan{};
    // Cold graph-revision metadata. Generated realtime code never consults
    // this inventory; execution-generation preparation uses it to reconcile
    // semantic port state before activating a replacement realization.
    graph_jit::RealtimePortStateRequirements realtime_port_state_requirements{};
    graph_jit::RealtimePortStateRealizations realtime_port_state_realizations{};
    std::optional<CompiledGraphCutover> cutover{};
    CompiledGraphRootOperations root_operations{};
    CompiledGraphBackgroundOperations background_operations{};
    std::shared_ptr<void const> code_lifetime{};
};

struct GraphJitCompileRequest {
    std::uint64_t project_generation = 0;
    std::shared_ptr<ConfiguredGraph const> graph{};
    std::shared_ptr<NodeDefinitionsSnapshot const> definitions{};
};

// A transition specialization augments an already compiled steady successor.
// Recompilation is cold and deterministic; the steady artifact remains the
// ProjectGraph-owned desired/diagnostic result.
struct GraphJitTransitionCompileRequest {
    GraphJitCompileRequest successor{};
    std::shared_ptr<CompiledGraph const> steady_successor{};
    std::shared_ptr<CompiledGraph const> active_predecessor{};
    CompiledGraphRealtimePhase active_predecessor_phase =
        CompiledGraphRealtimePhase::steady;
};

struct GraphJitCompileResult {
    // False only for an unbound singleton bridge. Once GraphJit receives a
    // request, expected compiler failures are represented by diagnostics.
    bool attempted = false;
    std::shared_ptr<CompiledGraph const> compiled_graph{};
    std::vector<GraphJitDiagnostic> diagnostics{};

    [[nodiscard]] bool succeeded() const noexcept
    {
        return compiled_graph != nullptr;
    }
};

class GraphJit {
    class Impl;
    std::unique_ptr<Impl> impl_;

public:
    explicit GraphJit(GraphJitConfig config = {});
    ~GraphJit();
    GraphJit(GraphJit&&) noexcept;
    GraphJit& operator=(GraphJit&&) noexcept;

    GraphJit(GraphJit const&) = delete;
    GraphJit& operator=(GraphJit const&) = delete;

    [[nodiscard]] GraphJitKernelSpecialization const& specialization() const noexcept;
    [[nodiscard]] GraphJitCompileResult compile(GraphJitCompileRequest const& request);
    [[nodiscard]] GraphJitCompileResult compile_transition(
        GraphJitTransitionCompileRequest const& request);

    [[nodiscard]] GraphJitCompileResult handle_project_graph_transaction(
        GraphJitCompileRequest const& request);
};
} // namespace iv
