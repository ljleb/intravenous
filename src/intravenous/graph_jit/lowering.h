#pragma once

#include <intravenous/graph_jit/realtime_port_state_plan.h>
#include <intravenous/graph_jit/realtime_port_state_transition.h>
#include <intravenous/graph_jit/port_realization.h>
#include <intravenous/node/layout.h>
#include <intravenous/runtime/graph_jit.h>

#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Module.h>

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace iv::graph_jit {
// The compiler ABI of one package callback. Every callback with a given ABI has
// the LLVM function type package_callback_type() returns for it.
enum class CallbackImportAbi : std::uint8_t {
    block,
    background,
    batch,
    sequential_initialize,
    sequential_combine,
};

llvm::FunctionType* package_callback_type(
    llvm::LLVMContext& context,
    CallbackImportAbi abi);

// The node-code copy of one retained package global, by the symbol the project
// module declares for it. An empty symbol marks a global node code cannot use.
struct PackageRetainedGlobal {
    std::string symbol{};
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
};

// Name of the generated zero-input/zero-output root operation. The root owns
// scheduling decisions; primitive skip callbacks remain internal implementation
// details and are never exposed as a root skip_block ABI.
struct LoweredGraphRootSymbols {
    std::string tick_block{};
    std::string transition_tick_block{};
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

// The lowerer owns graph analysis and canonical storage planning as well as
// the selected node-code imports and construction of the complete project
// module. It must finish node_layout before emitting final realtime
// storage accesses into output_module. TockState remains separately owned
// background sidecar state and is supplied through background call frames.
// Immutable lowering-shared tables should be emitted as LLVM globals so their
// lifetime is exactly the materialized ORC generation.
struct LoweringOutput {
    ResolvedPortRealization port_realization{};
    NodeLayout node_layout{};
    TockStateLayout tock_state_layout{};
    BackgroundEvaluationPlan background_evaluation_plan{};
    RealtimePortStateRequirements realtime_port_state_requirements{};
    RealtimePortStateRealizations realtime_port_state_realizations{};
    std::vector<ManualControlRuntimeBinding> manual_controls{};
    std::optional<LoweredGraphTransition> transition{};
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
