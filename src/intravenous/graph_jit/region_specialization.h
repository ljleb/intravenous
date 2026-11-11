#pragma once

#include <intravenous/node/code_key.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace iv {
struct PackageRevision;
struct ReflectedNodeTickContext;
}

namespace iv::graph_jit {

// A region specialization is optimized native code for the complete sliced
// invocation of one cyclic ExecutionRegionPlan. It inlines each step's package
// tick_block from the revision's finalized bitcode and binds eligible immutable
// inputs to constant storage. LLVM can then remove ordinary loads and control
// flow proved redundant. Strict floating-point library calls can remain even
// after their arguments become constant; folding those calls is optimizer work
// inside this same general specialization mechanism, not node-specific planner
// behavior. The specialization is cached by its actual code inputs: regions
// whose steps run the same node code over the same binding constants and
// immutable values share one compiled function. The generic interpreter path in
// execute_realtime_root() remains the correctness path for every region that
// is not specialized.

// One step's per-call inputs. context is the step's prepared
// ReflectedNodeTickContext in the root scratch; the specialization reads only
// its storage pointers. operations_before and operations_after identify the
// step's own storage operations, which the specialization runs through the
// root's run_operations callback around each slice of the step, exactly as
// the interpreter runs them.
struct RegionSpecializationStep {
    void const* node_data = nullptr;
    ReflectedNodeTickContext const* context = nullptr;
    void const* operations_before = nullptr;
    void const* operations_after = nullptr;
};

// Runs one step operation list of the root call frame for one slice.
using RegionSpecializationOperationsFunction = void (*)(
    void* frame,
    void const* operations,
    std::uint64_t index_bits,
    std::size_t count) noexcept;

// begin_bases holds, for every Sequential begin index of every step in
// request order, the generation pace cursor that index advances from.
using RegionSpecializationFunction = void (*)(
    RegionSpecializationStep const* steps,
    std::uint64_t const* begin_bases,
    std::byte* workspace,
    std::uint64_t root_index_bits,
    std::size_t block_size,
    void* frame,
    RegionSpecializationOperationsFunction run_operations) noexcept;

// The binding whose sequential_begin_index one root begin-index update writes.
struct RegionSpecializationBeginIndex {
    bool output = false;
    std::size_t binding = 0;
};

// An input whose every channel reads one immutable value. The specialization
// binds it to an LLVM constant: a one-value ring with no delay, which is
// exact for a node without block input access reading no history.
struct RegionSpecializationImmutableInput {
    std::size_t binding = 0;
    std::vector<std::byte> value{};
};

struct RegionSpecializationStepRequest {
    std::shared_ptr<PackageRevision const> revision{};
    NodeCodeKey code_key{};
    std::uint64_t provider_version = 0;
    std::vector<std::size_t> input_channel_counts{};
    std::vector<std::size_t> output_channel_counts{};
    // The step's binding-template bytes: its
    // ReflectedContinuousInputPortBinding and
    // ReflectedContinuousOutputPortBinding arrays and its
    // ReflectedNodeTickContext. Every pointer field is zero.
    std::vector<std::byte> inputs{};
    std::vector<std::byte> outputs{};
    std::vector<std::byte> context{};
    std::vector<RegionSpecializationImmutableInput> immutable_inputs{};
    std::vector<RegionSpecializationBeginIndex> begin_indices{};
    // The step has storage operations before or after its invocation.
    bool operations_before = false;
    bool operations_after = false;
};

struct RegionSpecializationRequest {
    // Frames per slice: ExecutionRegionPlan::maximum_block_size.
    std::size_t quantum = 0;
    std::vector<RegionSpecializationStepRequest> steps{};
};

// Every record a specialization reads is present and sized for its bindings.
bool region_specialization_request_is_complete(
    RegionSpecializationRequest const& request) noexcept;

// Equal keys identify requests that compile to the same code. The key holds
// the field values that the emitted code depends on, never raw template bytes,
// whose padding is unspecified. An incomplete request has an empty key.
std::string region_specialization_key(RegionSpecializationRequest const& request);

// Reusable per-call storage for the request's binding and context copies. One
// root call needs only the largest specialized region's workspace because its
// regions run serially.
std::optional<std::size_t> region_specialization_workspace_size(
    RegionSpecializationRequest const& request) noexcept;

struct RegionSpecializationCode {
    RegionSpecializationFunction function = nullptr;
    // Keeps the compiled code alive while a realtime root plan uses it.
    std::shared_ptr<void const> lifetime{};
};

// Supplies compiled region specializations to the realtime root builder.
// Nothing means that the region keeps the generic interpreter path.
class RegionSpecializationProvider {
public:
    virtual ~RegionSpecializationProvider() = default;
    virtual std::optional<RegionSpecializationCode> region_specialization(
        RegionSpecializationRequest const& request) = 0;
    // A cyclic region that the root builder keeps on the interpreter without
    // requesting a specialization, and why.
    virtual void region_kept_on_interpreter(std::string const& reason) = 0;
};

} // namespace iv::graph_jit
