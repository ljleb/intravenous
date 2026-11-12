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
namespace graph_jit {
struct RealtimeRootPlan;
}

struct GraphJitConfig {
    std::size_t sample_rate = 48000;
    std::size_t block_size = 256;
    // One compiler-wide policy for realtime sample/event buffer placement.
    // stack_budget_bytes is enforced against the packed realtime-root stack,
    // not independently against every buffer choice.
    RealtimeStorageCostModel realtime_storage_cost_model{};
    // Compile every eligible cyclic region and acyclic step into cached region
    // specializations (graph_jit/region_specialization.h). Off, every region
    // runs through the realtime root's interpreter; tests and measurements
    // compare the two.
    bool specialize_regions = true;
    // Budget of the recent-use region specialization cache, in bytes of
    // allocated code memory: the whole pages that JITLink's in-process memory
    // manager keeps for each specialization's code and data. ORC's heap
    // bookkeeping per specialization is not counted. Every specialization the
    // cache owns counts, including one that a live compiled graph also runs;
    // the least recently requested one is evicted first. Eviction removes only
    // the cache's ownership, so code that a compiled graph still runs stays
    // alive. The default is provisional, not a measured choice.
    std::size_t region_specialization_cache_budget_bytes = 64 * 1024 * 1024;
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

// Zero-input/zero-output root-node execution ABI. GraphJit's realtime root is
// application code, graph_jit::execute_realtime_root(), and its operation pairs
// that code with the immutable RealtimeRootPlan of one realization; plan is
// that pointer. Mutable bytes are owned by GraphExecutor through NodeStorage
// created from CompiledGraph::node_layout. Dynamic background-derived inputs
// arrive only as resolved views in one callback-scoped invocation record.
// Realtime lifecycle remains in NodeStorage; background TockState lifecycle
// belongs to its separate sidecar. Callers must provide block_size > 0; an
// empty audio block is outside the root-operation ABI. If the compiled graph
// has Tick runtime slots, invocation must be non-null and contain the complete
// kind-specific spans described by its BackgroundEvaluationPlan::tick_runtime.
using CompiledGraphBlockFunction = void (*)(
    graph_jit::RealtimeRootPlan const* plan,
    std::byte* storage_base,
    graph_jit::TickInvocationCall const* invocation,
    GlobalIndex sample_index,
    std::size_t block_size);

// One root operation: its code and the immutable data that code executes. The
// operation keeps that data alive for as long as the realization can run.
struct CompiledGraphBlockOperation {
    CompiledGraphBlockFunction function = nullptr;
    std::shared_ptr<graph_jit::RealtimeRootPlan const> plan{};

    explicit operator bool() const noexcept { return function != nullptr; }

    void operator()(
        std::byte* storage_base,
        graph_jit::TickInvocationCall const* invocation,
        GlobalIndex sample_index,
        std::size_t block_size) const
    {
        function(plan.get(), storage_base, invocation, sample_index, block_size);
    }

    friend bool operator==(
        CompiledGraphBlockOperation const& operation,
        std::nullptr_t) noexcept
    {
        return operation.function == nullptr;
    }
};

using CompiledGraphSequentialInputCombineFunction =
    graph_jit::SequentialInputCombineFunction;
using CompiledGraphSequentialInputInitializeFunction =
    graph_jit::SequentialInputInitializeFunction;

struct CompiledGraphRootOperations {
    CompiledGraphBlockOperation tick_block{};
    CompiledGraphBlockOperation transition_tick_block{};
    CompiledGraphSequentialInputCombineFunction combine_sequential_input = nullptr;
    CompiledGraphSequentialInputInitializeFunction initialize_sequential_input =
        nullptr;

    [[nodiscard]] bool valid() const noexcept
    {
        return static_cast<bool>(tick_block)
            && combine_sequential_input != nullptr;
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
//
// A first generation's cutover starts from the empty graph instead: it names
// no predecessor, and its transfers write only fresh values. It exists when
// two states need different fresh values in one region, so that one of them
// needs transition-only storage, or when a fresh value is not the region's
// declared initial value.
struct CompiledGraphCutover {
    bool from_empty_graph = false;
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
// Generated background code remains pinned by code_lifetime; each realtime root
// operation owns its RealtimeRootPlan.
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
    // Cold graph-revision metadata. The realtime root never consults
    // this inventory; execution-generation preparation uses it to reconcile
    // semantic port state before activating a replacement realization.
    graph_jit::RealtimePortStateRequirements realtime_port_state_requirements{};
    graph_jit::RealtimePortStateRealizations realtime_port_state_realizations{};
    std::vector<ManualControlRuntimeBinding> manual_controls{};
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

// One ProjectGraph child request. Without a predecessor GraphJit compiles the
// steady artifact. With one, it runs a single compilation specialized from
// that predecessor's steady realization: the module defines the transition
// root and the steady root that follows it, and the steady plan it builds
// first is the predecessor-independent analysis.
//
// initial_generation marks the request for the executor's first generation.
// Without a predecessor, GraphJit then compiles a cutover from the empty graph
// when the fresh values need one (CompiledGraphCutover::from_empty_graph). A
// request without a predecessor that is not initial, such as a manual-control
// recompile, yields the steady artifact.
struct ProjectGraphGraphJitRequest {
    GraphJitCompileRequest successor{};
    std::shared_ptr<CompiledGraph const> predecessor{};
    bool initial_generation = false;
};

// Wall-clock microseconds spent in each phase of one compilation, plus the
// sizes that explain them. A phase the compilation did not reach stays zero.
struct GraphJitCompileTimings {
    bool transition = false;
    std::uint64_t capture_us = 0;
    // build_lowering_plan(): host-side graph, storage, and execution planning.
    std::uint64_t lowering_plan_us = 0;
    std::uint64_t lowering_transition_us = 0;
    // emit_lowering_plan(): node-code import declarations plus IR emission.
    std::uint64_t lowering_emit_us = 0;
    std::uint64_t verify_us = 0;
    std::uint64_t optimization_us = 0;
    // ORC materialization: machine-code generation and linking.
    std::uint64_t materialization_us = 0;
    std::uint64_t total_us = 0;
    std::uint64_t functions = 0;
    std::uint64_t instructions_before_optimization = 0;
    std::uint64_t instructions_after_optimization = 0;
    // Region specializations of the steady root: compiled by this
    // compilation, reused from the cache, failed, and eligible regions or
    // acyclic steps skipped because they need work the specialization does
    // not compile. A failed or skipped candidate runs through the interpreter.
    // region_specialization_us is the compile time of the compiled ones,
    // inside lowering_emit_us. The strings hold the first failure and the
    // first skip reason.
    std::uint64_t region_specializations_compiled = 0;
    std::uint64_t region_specializations_reused = 0;
    std::uint64_t region_specialization_failures = 0;
    std::uint64_t region_specializations_skipped = 0;
    // Loads of values fixed for the call, workspace fields and immutable
    // input rings, that compiled specializations replaced with those values.
    std::uint64_t region_specialization_substitutions = 0;
    std::uint64_t region_specialization_us = 0;
    // Allocated code memory, in whole pages, of the region specializations
    // this compilation created; then the bytes the recent-use cache owns after
    // this compilation, and its budget (GraphJitConfig).
    std::uint64_t region_specialization_allocated_bytes = 0;
    std::uint64_t region_specialization_cache_bytes = 0;
    std::uint64_t region_specialization_cache_budget_bytes = 0;
    std::string region_specialization_failure{};
    std::string region_specialization_skip{};
    RealtimeStoragePlanSummary realtime_storage{};
};

struct GraphJitCompileResult {
    // False only for an unbound singleton bridge. Once GraphJit receives a
    // request, expected compiler failures are represented by diagnostics.
    bool attempted = false;
    std::shared_ptr<CompiledGraph const> compiled_graph{};
    std::vector<GraphJitDiagnostic> diagnostics{};
    // One entry per compilation this request ran, in order. A project
    // transaction with a predecessor runs a steady and then a transition one.
    std::vector<GraphJitCompileTimings> timings{};

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
        ProjectGraphGraphJitRequest const& request);
};
} // namespace iv
