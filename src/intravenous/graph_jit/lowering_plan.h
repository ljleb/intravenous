#pragma once

#include <intravenous/graph/reflected_node_operations.h>
#include <intravenous/graph_jit/lowering.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace iv::graph_jit::detail {
// Host-side lowering plan. These records intentionally contain no llvm::Value*
// or other project-module IR handles: the complete plan is built before package
// modules are consumed or output LLVM is emitted.
struct PrimitiveStoragePlan {
    bool has_state = false;
    std::size_t state_offset = 0;
    std::size_t state_size = 0;
    bool has_compiled_state = false;
    std::size_t compiled_state_offset = 0;
    std::size_t compiled_state_size = 0;
};

struct DeclarationPlan {
    NodeLayout node_layout{};
    // Indexed independently from NodeLayout::nodes so later lowering can add
    // synthetic/layout-only nodes without coupling execution-step identity to
    // canonical layout record order.
    std::vector<PrimitiveStoragePlan> primitive_storage{};
};

struct CallbackImportPlan {
    std::string source_symbol{};
    std::string import_symbol{};
    std::string role{};
};

struct RetainedGlobalImportPlan {
    std::string source_symbol{};
    std::string import_symbol{};
    std::size_t size = 0;
};

struct PackageImportGroup {
    std::size_t package_index = 0;
    std::vector<CallbackImportPlan> callbacks{};
    std::vector<RetainedGlobalImportPlan> retained_globals{};
};

struct PrimitiveCallbackPlan {
    std::string tick_block{};
    std::string skip_block{};
};

struct PackageImportPlan {
    // One group per compile-local package module. All selected callback and
    // retained-global roots must be known before that module is consumed once.
    std::vector<PackageImportGroup> packages{};
    // Indexed by analyzed concrete primitive. Several primitives may share one
    // imported callback when they select the same package-local implementation.
    std::vector<PrimitiveCallbackPlan> primitive_callbacks{};
};

struct NodeConfigurationRelocationPlan {
    std::size_t byte_offset = 0;
    std::size_t addend = 0;
    // Empty means an explicit null pointer slot. Non-empty names the imported
    // retained LLVM global whose byte-address plus addend reconstructs the
    // configured pointer value.
    std::string retained_global_symbol{};
};

struct NodeConfigurationPlan {
    // Own the configured bytes so LLVM emission does not depend on native
    // configured-object addresses after host-side planning completes. Pointer
    // slots are zeroed here and reconstructed symbolically during LLVM
    // emission; native process addresses must never enter project IR.
    std::vector<std::byte> bytes{};
    std::vector<NodeConfigurationRelocationPlan> relocations{};
    std::size_t alignment = 1;
    ReflectedNodeTickContext tick_context_template{};
    std::string node_global_symbol{};
    std::string tick_context_global_symbol{};
};

struct ConfigurationPlan {
    std::vector<NodeConfigurationPlan> nodes{};
};

struct PrimitiveExecutionStep {
    std::size_t configuration_index = 0;
    std::size_t storage_index = 0;
    std::string tick_callback_symbol{};
    std::string skip_callback_symbol{};
};

struct ExecutionPlan {
    // Ordered root execution sequence. For disconnected zero-port primitives this
    // is configured-bundle order; connection-aware scheduling will later replace
    // that provisional order without changing the LLVM-emission boundary.
    std::vector<PrimitiveExecutionStep> primitive_steps{};
    bool root_skippable = false;
};

struct LoweringPlan {
    DeclarationPlan declarations{};
    PackageImportPlan imports{};
    ConfigurationPlan configurations{};
    ExecutionPlan execution{};
};

std::expected<LoweringPlan, std::string> build_lowering_plan(
    LoweringInput const& input);
} // namespace iv::graph_jit::detail
