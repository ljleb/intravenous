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

// A region specialization is optimized native code for the complete
// invocation of one ExecutionRegionPlan: a cyclic region's slices, or an
// acyclic region's one step, sliced to its maximum block size. It inlines each
// step's package tick_block from the revision's finalized bitcode. GraphJit
// then replaces reads of every value fixed for the call, binding constants,
// context constants, and immutable inputs, with LLVM constants, and LLVM
// removes the computation that depends only on them. The specialization is
// cached by its actual code inputs. A value specialization includes immutable
// values in that identity. A structure specialization leaves those values as
// runtime ring reads and can therefore serve several value tuples of the same
// code and binding shape. The generic interpreter path in
// execute_realtime_root() remains the correctness path for every region that
// is not specialized.

// One step's per-call inputs. context is the step's prepared
// ReflectedNodeTickContext in the root scratch; the specialization reads only
// its storage pointers. operations_before and operations_after point at the
// complete RootOperation vectors selected by the request's ordered operation
// actions. Generated code delegates selected spans and directly executes
// eligible sample compositions.
struct RegionSpecializationStep {
    void const* node_data = nullptr;
    ReflectedNodeTickContext const* context = nullptr;
    void const* operations_before = nullptr;
    void const* operations_after = nullptr;
    // RootPrimitive index used only when the request asks the root to stage
    // this step's participating ManualControlPlan records.
    std::size_t primitive_index = 0;
};

// Runs one step operation list of the root call frame for one slice.
using RegionSpecializationOperationsFunction = void (*)(
    void* frame,
    void const* operations,
    std::size_t operation_begin,
    std::size_t operation_count,
    std::uint64_t index_bits,
    std::size_t count) noexcept;

// Invalidates the activity summary of one sample representation after
// generated code writes it.
using RegionSpecializationMarkSampleFunction = void (*)(
    void* frame,
    std::size_t representation_index) noexcept;

// Stages the ManualControlPlan timelines of one primitive immediately before
// its specialized callback. slice_index/count identify the current region
// slice. enclosing_index/count identify the complete root invocation, which
// owns the interpolation ramp.
using RegionSpecializationManualControlsFunction = void (*)(
    void* frame,
    std::size_t primitive_index,
    std::uint64_t slice_index_bits,
    std::size_t slice_count,
    std::uint64_t enclosing_index_bits,
    std::size_t enclosing_count) noexcept;

// begin_bases holds, for every Sequential begin index of every step in
// request order, the generation pace cursor that index advances from.
using RegionSpecializationFunction = void (*)(
    RegionSpecializationStep const* steps,
    std::uint64_t const* begin_bases,
    std::byte* const* sample_representations,
    std::size_t const* sample_representation_indices,
    std::uint64_t project_begin_index,
    std::byte* workspace,
    std::uint64_t root_index_bits,
    std::size_t block_size,
    void* frame,
    RegionSpecializationOperationsFunction run_operations,
    RegionSpecializationManualControlsFunction stage_manual_controls,
    RegionSpecializationMarkSampleFunction mark_sample) noexcept;

// The binding whose sequential_begin_index one root begin-index update writes.
struct RegionSpecializationBeginIndex {
    bool output = false;
    std::size_t binding = 0;
};

// An input whose every channel reads one immutable value. The specialization
// keeps the input's interpreter binding, with its ring, delay, and contiguous
// scratch, so every read means what it means to the interpreter. GraphJit
// knows that each channel's ring holds this value at every position: a read
// through the ring, at any history offset, becomes this value.
struct RegionSpecializationImmutableInput {
    std::size_t binding = 0;
    std::vector<std::byte> value{};
};

// One continuous representation used by a generated sample composition.
// representation selects a dense per-region pointer. The remaining fields
// describe how one timeline index and channel select a value in that storage.
struct RegionSpecializationSampleAddress {
    std::size_t representation = 0;
    std::size_t frame_capacity = 0;
    std::size_t channel_count = 0;
    std::size_t value_size_bytes = 0;
    bool planar = false;
};

struct RegionSpecializationCompositionSource {
    RegionSpecializationSampleAddress address{};
    std::size_t channel = 0;
    std::size_t read_latency = 0;
};

struct RegionSpecializationCompositionTerm {
    std::size_t source = 0;
    float coefficient = 1.0f;
};

struct RegionSpecializationCompositionContribution {
    std::vector<RegionSpecializationCompositionSource> sources{};
    std::vector<std::size_t> target_channels{};
    std::vector<std::size_t> term_begins{};
    std::vector<RegionSpecializationCompositionTerm> terms{};
};

struct RegionSpecializationSampleComposition {
    RegionSpecializationSampleAddress target{};
    std::size_t target_history = 0;
    std::size_t scalar_lane_count = 0;
    std::vector<RegionSpecializationCompositionContribution> contributions{};
};

enum class RegionSpecializationOperationKind : std::uint8_t {
    root_operations,
    sample_composition,
};

// One action in a step's before or after operation list. A root_operations
// action delegates the selected contiguous span to the existing root. A
// sample_composition action emits that operation directly into native code.
struct RegionSpecializationOperation {
    RegionSpecializationOperationKind kind =
        RegionSpecializationOperationKind::root_operations;
    std::size_t operation_begin = 0;
    std::size_t operation_count = 0;
    RegionSpecializationSampleComposition composition{};
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
    std::vector<RegionSpecializationOperation> operations_before{};
    std::vector<RegionSpecializationOperation> operations_after{};
    // The root stages this primitive's participating ManualControlPlan
    // timelines after operations_before and before tick_block.
    bool stages_manual_controls = false;
};

struct RegionSpecializationRequest {
    // Frames per slice: ExecutionRegionPlan::maximum_block_size, at most the
    // kernel's maximum block size.
    std::size_t quantum = 0;
    // Cyclic-region step operations run around every slice. An acyclic
    // region's one step runs its operations once around the complete sliced
    // callback.
    bool operations_per_slice = false;
    std::size_t sample_representation_count = 0;
    std::vector<RegionSpecializationStepRequest> steps{};
};

// Every record a specialization reads is present and sized for its bindings.
bool region_specialization_request_is_complete(
    RegionSpecializationRequest const& request) noexcept;

// Equal keys identify requests that compile to the same code. The key holds
// the field values that the emitted code depends on, never raw template bytes,
// whose padding is unspecified. An incomplete request has an empty key.
std::string region_specialization_key(RegionSpecializationRequest const& request);

// Equal keys identify requests whose code and binding shape agree after
// immutable input values are left as runtime ring reads. This is the cache key
// of a shared structure specialization. An incomplete request has an empty
// key.
std::string region_specialization_structure_key(
    RegionSpecializationRequest const& request);

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
    // A candidate region that the root builder keeps on the interpreter
    // without requesting a specialization, and why.
    virtual void region_kept_on_interpreter(std::string const& reason) = 0;
};

} // namespace iv::graph_jit
