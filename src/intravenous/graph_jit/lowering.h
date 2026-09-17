#pragma once

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
struct PackageModule {
    std::shared_ptr<PackageRevision const> revision{};
    llvm::Module* module = nullptr;
    std::span<llvm::GlobalVariable* const> retained_globals{};
};

struct NodeImplementation {
    std::size_t node_bundle = 0;
    RegisteredNodeTypeIdentity identity{};
    NodeCodeKey code_key{};
    std::shared_ptr<PackageRevision const> revision{};
    llvm::Module* package_module = nullptr;

    std::size_t state_size = 0;
    std::size_t state_alignment = 1;
    std::size_t compiled_state_size = 0;
    std::size_t compiled_state_alignment = 1;

    // Host declaration data is part of lowering because the canonical
    // NodeLayout must be finalized before final LLVM is emitted. That lets
    // generated code embed State/CompiledState/raw-region offsets as constants
    // and gives O3 the opportunity to optimize through those addresses.
    void const* node_data = nullptr;
    NodeStateStructures const* state_structures = nullptr;
    std::size_t (*declare_node)(
        void const*, NodeStateStructures const*, NodeLayoutBuilder&) = nullptr;

    // Runtime/compiler LLVM anchors imported into the whole-project module.
    llvm::Function* tick_block = nullptr;
    llvm::Function* skip_block = nullptr;
    llvm::Function* access_block_batched = nullptr;
    llvm::Function* propagate_block_access_batched = nullptr;
};

struct ConfigRelocation {
    std::size_t node_bundle = 0;
    NodeConfigRelocation const* relocation = nullptr;
    // Empty for an explicit null pointer slot. Non-null relocations identify
    // the exact retained package global that replaces the native pointer bytes.
    std::shared_ptr<PackageRevision const> revision{};
    llvm::GlobalVariable* retained_global = nullptr;
};

// Everything the difficult compiler core is allowed to see. In particular,
// there is no PackageJit/ModuleLoader/live NodeDefinitions dependency here.
// Package modules and the caller-owned output module share one LLVMContext, so
// the lowerer may clone/link the selected implementation closure directly.
struct LoweringInput {
    ConfiguredGraph const& graph;
    GraphJitKernelSpecialization const& specialization;
    std::span<PackageModule const> packages{};
    // One entry for each registered concrete primitive. Synthetic/host graph
    // nodes remain represented by ConfiguredGraph and are lowered structurally.
    std::span<NodeImplementation const> node_implementations{};
    // Includes explicit-null pointer slots as entries with no revision/global.
    // The lowerer must reconstruct pointer fields from this symbolic data rather
    // than embedding the native pointer bytes stored in configured node objects.
    std::span<ConfigRelocation const> config_relocations{};
};

// Names of the fixed GraphExecutor <-> generated-module ABI. The lowerer may
// choose unique symbol names per project generation; GraphJit validates their
// LLVM types before optimization and resolves all four after ORC materialization.
struct LoweredGraphEntrypointSymbols {
    std::string initialize{};
    std::string release{};
    std::string tick_block{};
    std::string skip_block{};
};

// The lowerer owns graph analysis and canonical storage planning as well as
// selected primitive-LLVM import/inlining and construction of the complete
// project module. It must finish node_layout before emitting final storage
// accesses into output_module. State, CompiledState, graph-persistent arrays,
// and bounded compiler workspaces therefore share one NodeStorage allocation,
// and generated accesses may use final NodeLayout offsets as constants.
//
// runtime_plan is transitional compatibility with the current post-lowering
// materialization shell. The next LLVM-IR -> CompiledGraph pass removes that
// parallel representation and makes node_layout the sole storage contract.
// Immutable lowering-shared tables should be emitted as LLVM globals so their
// lifetime is exactly the materialized ORC generation.
struct LoweringOutput {
    NodeLayout node_layout{};
    CompiledGraphRuntimePlan runtime_plan{};
    LoweredGraphEntrypointSymbols entrypoints{};
};

std::expected<LoweringOutput, std::string> lower_configured_graph_to_llvm(
    LoweringInput const& input,
    llvm::Module& output_module);
} // namespace iv::graph_jit
