#pragma once

#include <intravenous/graph_jit/region_specialization.h>
#include <intravenous/node/code_key.h>

#include <expected>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace llvm {
class Module;
}

namespace iv::graph_jit {

// One entry of finalized package bitcode's iv_package_retained_globals table,
// indexed by RetainedGlobalData::index. symbol is empty when the table entry
// does not name a plain GlobalVariable after package optimization.
struct PackageRetainedGlobalDefinition {
    std::string symbol{};
    std::size_t size = 0;
};

// What a region specialization's workspace holds for its whole call, as far as
// GraphJit knows it. known[i] marks workspace byte i whose value is bytes[i].
// A pointer field holds either another workspace address or the address of
// immutable bytes, given by their contents. Other runtime storage pointers and
// the reflected ABI's mutable fields are not known.
struct RegionSpecializationWorkspaceContents {
    struct PointerField {
        std::size_t offset = 0;
        // Set for a workspace address at target. Otherwise the field points
        // at bytes equal to constant for the whole call: an immutable input
        // channel's ring, which holds the input's value at every position.
        bool workspace = false;
        std::size_t target = 0;
        std::vector<std::byte> constant{};
    };
    std::vector<std::byte> bytes{};
    std::vector<std::uint8_t> known{};
    std::vector<PointerField> pointers{};
};

// Emits the entry function of one region specialization, a
// RegionSpecializationFunction named function_name, into module.
// tick_blocks[s] names the package definition of step s's tick_block; the
// caller links that definition into module afterwards.
//
// Once per call the function builds a private copy of each step's bindings
// and ReflectedNodeTickContext in the workspace: every binding constant from
// the request's template bytes and every storage pointer from the step's
// prepared context. An immutable input keeps that binding; the result records
// that its rings hold the input's value. The function then runs the region's
// slices, writing each binding's Sequential begin index, staging an affected
// step's ManualControlPlan timelines through the root callback, and calling
// every step's tick_block in order, as the interpreter does. The result
// describes what the workspace holds for the call.
std::expected<RegionSpecializationWorkspaceContents, std::string>
emit_region_specialization(
    llvm::Module& module,
    RegionSpecializationRequest const& request,
    std::string const& function_name,
    std::span<std::string const> tick_blocks);

// Replaces every load in function_name whose address resolves, through the
// workspace argument and contents' pointer fields, to a known workspace field
// with that field's value. The reflected ABI passes the context and bindings
// as const; only their mutable fields change during a call, and contents
// excludes those. A load of a field that points at immutable bytes becomes a
// private constant global holding them. A load from a constant global whose
// bytes repeat, such as an immutable input's ring, becomes its value when the
// address's variable part is a multiple of the repetition period: a ring slot
// computed at run time then still reads a constant. Run after the package
// callbacks are inlined. Returns the number of loads replaced.
std::size_t substitute_region_workspace_values(
    llvm::Module& module,
    std::string const& function_name,
    RegionSpecializationWorkspaceContents const& contents);

// The tick_block definition of every node in finalized package bitcode, by
// NodeCodeKey, read from its iv_node_types compiler record.
std::expected<std::vector<std::pair<NodeCodeKey, std::string>>, std::string>
package_tick_block_symbols(llvm::Module& package);

// The retained package globals whose accepted node-code copies GraphJit must
// import when it links package callbacks into a region specialization. A
// specialization must not create a second copy: node configuration pointer
// relocations refer to PackageRevision::node_code_retained_globals.
std::expected<std::vector<PackageRetainedGlobalDefinition>, std::string>
package_retained_global_definitions(llvm::Module& package);

// After the package definitions are linked into module, makes every definition
// except function_name internal, and every internal function always inlined
// unless its author made it noinline: the specialization is one fused
// operation. Package static initialization is not part of node code, so the
// module keeps no constructor or destructor list.
void internalize_region_specialization(
    llvm::Module& module,
    std::string const& function_name);

} // namespace iv::graph_jit
