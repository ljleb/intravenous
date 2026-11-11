#pragma once

#include <intravenous/graph_jit/package_callback_abi.h>
#include <intravenous/graph_jit/realtime_port_state_plan.h>
#include <intravenous/graph_jit/region_specialization.h>
#include <intravenous/graph_jit/realtime_port_state_transition.h>
#include <intravenous/graph_jit/port_realization.h>
#include <intravenous/node/layout.h>
#include <intravenous/runtime/graph_jit.h>

#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Module.h>

#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace iv::graph_jit {
// The node-code copy of one retained package global, by the symbol the project
// module declares for it. An empty symbol marks a global node code cannot use.
struct PackageRetainedGlobal {
    std::string symbol{};
    void const* address = nullptr;
    std::size_t size = 0;
    std::size_t alignment = 1;
};

// One accepted package revision's node code, compiled once per package build
// (NodeCodeRecordData, graph_jit_direction.md). Lowering sees only symbols: the
// project module declares the callbacks and retained globals it uses, and
// GraphJit defines each symbol at its node-code address in the generation
// JITDylib.
struct PackageCode {
    std::shared_ptr<PackageRevision const> revision{};
    // Indexed by the revision's retained-global index.
    std::span<PackageRetainedGlobal const> retained_globals{};
};

struct NodeImplementation {
    std::size_t node_bundle = 0;
    RegisteredNodeTypeIdentity identity{};
    NodeCodeKey code_key{};
    std::shared_ptr<PackageRevision const> revision{};
    std::uint64_t provider_version = 0;
    std::shared_ptr<void const> configuration_identity{};
    PackageCode const* package = nullptr;

    std::size_t state_size = 0;
    std::size_t state_alignment = 1;
    std::size_t background_state_size = 0;
    std::size_t background_state_alignment = 1;
    bool intrinsically_replayable = false;
    bool pointwise_tick = false;
    bool block_input_access = false;

    // Host declaration data is part of lowering because the canonical
    // realtime NodeLayout must be finalized before final LLVM is emitted. That
    // lets generated code embed State/raw-region offsets as constants. The
    // separately retained TockState metadata creates background sidecars.
    void const* node_data = nullptr;
    NodeStateStructures const* state_structures = nullptr;
    std::size_t (*declare_node)(
        void const*,
        NodeStateStructures const*,
        NodeLayoutBuilder&,
        ReflectedNodePortRealization const&) = nullptr;

    // Node-code symbols of the runtime/compiler callbacks the project module
    // may import. Empty means the node has no such callback.
    std::string tick_block{};
    std::string tick_block_batch{};
    std::string skip_block{};
    std::string skip_block_batch{};
    std::string tock_coverage{};
    std::string tock_coverage_batch{};
    std::string propagate_forward_coverage{};
    std::string propagate_forward_coverage_batch{};
    std::string propagate_reverse_coverage{};
    std::string propagate_reverse_coverage_batch{};
    std::string initialize_sequential_input{};
    std::string combine_sequential_input_values{};

    // Native node-code callbacks that the realtime root calls by address. The
    // symbol fields above are the import names of the generated background
    // roots. A name and its address identify the same callback in the pinned
    // PackageRevision.
    details::NodeCompilerOperations native_operations{};

    bool (*host_initialize_sequential_input)(
        std::size_t,
        ContinuousValueTypeId,
        iv::details::SequentialInputInitialization,
        std::byte*,
        std::size_t,
        std::size_t) = nullptr;
};

struct ConfigRelocation {
    std::size_t node_bundle = 0;
    NodeConfigRelocation const* relocation = nullptr;
    // Empty for an explicit null pointer index. Non-null relocations identify
    // the exact retained package global that replaces the native pointer bytes.
    std::shared_ptr<PackageRevision const> revision{};
    PackageRetainedGlobal const* retained_global = nullptr;
};

// Immutable semantic inventory of the exact active predecessor realization.
// This is supplied only by the cold staging coordinator; ordinary desired-graph
// compilation remains predecessor-independent.
struct LoweringTransitionInput {
    RealtimePortStateRequirements const& predecessor_requirements;
    RealtimePortStateRealizations const& predecessor_realizations;
};

// Everything the difficult compiler core is allowed to see. In particular,
// there is no PackageJit/ModuleLoader/live NodeDefinitions dependency here, and
// no package LLVM: node code is already compiled, and lowering imports its
// symbols by declaration.
struct LoweringInput {
    ConfiguredGraph const& graph;
    GraphJitKernelSpecialization const& specialization;
    RealtimeStorageCostModel const& realtime_storage_cost_model;
    std::span<PackageCode const> packages{};
    // One entry for each registered concrete primitive. Synthetic/host graph
    // nodes remain represented by ConfiguredGraph and are lowered structurally.
    std::span<NodeImplementation const> node_implementations{};
    // Includes explicit-null pointer indices as entries with no revision/global.
    // The lowerer must reconstruct pointer fields from this symbolic data rather
    // than embedding the native pointer bytes stored in configured node objects.
    std::span<ConfigRelocation const> config_relocations{};
    LoweringTransitionInput const* transition = nullptr;
    // Compiles or reuses the steady root's region specializations. Without
    // it every cyclic region runs through the interpreter.
    RegionSpecializationProvider* region_specializations = nullptr;
};

// Names of the generated project-module operations. The realtime root is not
// among them: it is application code that executes a RealtimeRootPlan.
struct LoweredGraphRootSymbols {
    std::string propagate_background_forward{};
    std::string propagate_background_reverse{};
    std::string evaluate_background{};
    std::string combine_sequential_input{};
    std::string initialize_sequential_input{};
};

struct LoweredGraphTransition {
    RealtimePortStateTransitionPlan state_transition{};
    RealtimePortStateRealizations realtime_port_state_realizations{};
};

// The lowerer owns graph analysis and canonical storage planning, the realtime
// roots built from that plan, and construction of the project module that holds
// the generated background operations. realtime_root executes the steady
// realization; transition_realtime_root, present exactly when the transition
// has a finite Sequential cursor horizon, executes the cutover realization.
// TockState remains separately owned background sidecar state and is supplied
// through background call frames.
struct LoweringOutput {
    ResolvedPortRealization port_realization{};
    NodeLayout node_layout{};
    TockStateLayout tock_state_layout{};
    BackgroundEvaluationPlan background_evaluation_plan{};
    RealtimePortStateRequirements realtime_port_state_requirements{};
    RealtimePortStateRealizations realtime_port_state_realizations{};
    std::vector<ManualControlRuntimeBinding> manual_controls{};
    std::optional<LoweredGraphTransition> transition{};
    std::shared_ptr<RealtimeRootPlan const> realtime_root{};
    std::shared_ptr<RealtimeRootPlan const> transition_realtime_root{};
    LoweredGraphRootSymbols root_symbols{};
    // Wall-clock microseconds of the three lowering phases.
    std::uint64_t plan_us = 0;
    std::uint64_t transition_us = 0;
    std::uint64_t emit_us = 0;
};

std::expected<LoweringOutput, std::string> lower_configured_graph_to_llvm(
    LoweringInput& input,
    llvm::Module& output_module);
} // namespace iv::graph_jit
