#pragma once

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

    // Runtime/compiler anchors only. declare_node is configuration-time code
    // and is deliberately not exposed to whole-project lowering.
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

// The lowerer owns graph analyses, selected primitive-LLVM import/inlining,
// persistent/scratch layout selection, and construction of the complete project
// module. Mutable execution bytes remain caller-owned according to runtime_plan;
// immutable lowering-shared tables should be emitted as LLVM globals so their
// lifetime is exactly the materialized ORC generation. The surrounding
// GraphJit code owns ABI validation, verification, O3, ORC materialization,
// symbol resolution, and generation lifetime.
struct LoweringOutput {
    CompiledGraphRuntimePlan runtime_plan{};
    LoweredGraphEntrypointSymbols entrypoints{};
};

std::expected<LoweringOutput, std::string> lower_configured_graph_to_llvm(
    LoweringInput const& input,
    llvm::Module& output_module);
} // namespace iv::graph_jit
