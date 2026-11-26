#pragma once

#include <intravenous/graph_jit/lowering_plan.h>
#include <intravenous/graph_jit/region_specialization.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <utility>
#include <vector>

namespace iv::graph_jit {
namespace detail {
struct PrimitiveActivityState;
}

namespace realtime {
inline constexpr std::size_t no_index = std::numeric_limits<std::size_t>::max();

inline std::size_t align_up(std::size_t value, std::size_t alignment) noexcept
{
    return (value + alignment - 1) & ~(alignment - 1);
}

class AlignedBytes {
    std::byte* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t alignment_ = 1;

public:
    AlignedBytes() = default;

    AlignedBytes(std::size_t size, std::size_t alignment)
        : size_(size)
        , alignment_(std::max(alignment, alignof(std::max_align_t)))
    {
        if (size_ != 0) {
            data_ = static_cast<std::byte*>(
                ::operator new(size_, std::align_val_t{alignment_}));
        }
    }

    ~AlignedBytes()
    {
        if (data_) ::operator delete(data_, std::align_val_t{alignment_});
    }

    AlignedBytes(AlignedBytes const&) = delete;
    AlignedBytes& operator=(AlignedBytes const&) = delete;

    AlignedBytes(AlignedBytes&& other) noexcept
        : data_(std::exchange(other.data_, nullptr))
        , size_(std::exchange(other.size_, 0))
        , alignment_(std::exchange(other.alignment_, 1))
    {}

    AlignedBytes& operator=(AlignedBytes&& other) noexcept
    {
        if (this == &other) return *this;
        if (data_) ::operator delete(data_, std::align_val_t{alignment_});
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0);
        alignment_ = std::exchange(other.alignment_, 1);
        return *this;
    }

    [[nodiscard]] std::byte* data() noexcept { return data_; }
    [[nodiscard]] std::byte const* data() const noexcept { return data_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
};

// The nonzero terms of one linear channel conversion. Converted channel c sums
// terms [term_begins[c], term_begins[c + 1]) in source-channel order, and a
// channel without terms is zero. A coefficient of one adds its source value
// unscaled. This is the arithmetic of the channel conversion registry, or of
// an explicit SampleChannelTransformPlan, with its zero terms removed.
struct ChannelTerm {
    std::size_t source_channel = 0;
    float coefficient = 1.0f;
};

struct ChannelMix {
    std::vector<ChannelTerm> terms{};
    std::vector<std::size_t> term_begins{};

    [[nodiscard]] std::size_t converted_channel_count() const noexcept
    {
        return term_begins.empty() ? 0 : term_begins.size() - 1;
    }
};

struct RootMaterialization {
    ChannelMix mix{};
    // Target channels the materialization writes. A transition root's copy
    // into transition-only storage writes only the channels that storage holds.
    std::uint64_t written_channels = 0;
};

struct RootContribution {
    ChannelMix mix{};
    std::vector<SequentialCoverageSource> coverage_sources{};
    std::uint64_t additive_target_channels = 0;
    // Shifted feedback compositions only: the number of frames by which each
    // converted channel is written after its target frame.
    std::vector<std::size_t> write_latencies{};
};

// Where one representation lives during a root call.
enum class RootAddressBase : std::uint8_t {
    immutable,
    node_storage,
    scratch,
};

struct RootAddress {
    RootAddressBase base = RootAddressBase::node_storage;
    std::size_t offset = 0;
    std::byte const* immutable = nullptr;
};

// One pointer field of the binding template, written at every root call
// because the representation it addresses is resolved per call.
enum class BindingPointerBase : std::uint8_t {
    sample_representation,
    event_representation,
    scratch,
    node_storage,
};

struct BindingPointer {
    // Byte offset of the pointer field in the root scratch.
    std::size_t field_offset = 0;
    BindingPointerBase base = BindingPointerBase::node_storage;
    std::size_t representation = 0;
    std::size_t byte_offset = 0;
};

// One static part of the reflected binding area that an interpreted primitive
// reads. Specialized primitives build their static bindings in the reusable
// specialization workspace and need only the pointer fields below.
struct BindingTemplateCopy {
    std::size_t scratch_offset = 0;
    std::size_t template_offset = 0;
    std::size_t size = 0;
};

// The Tick runtime slots of one primitive, resolved against its port plans.
struct RootTickSlots {
    TickNodeInvocationPlan invocation{};
    // Per Sequential slot of the invocation, the primitive input it supplies.
    std::vector<std::size_t> sample_inputs{};
    std::vector<std::size_t> event_inputs{};
    // A slot whose input has no timeline of its own replaces that input's
    // binding once per root call. A slot whose input owns a timeline writes
    // the window of every invocation into it.
    std::vector<std::size_t> sample_overlay_slots{};
    std::vector<std::size_t> sample_timeline_slots{};
    std::vector<std::size_t> event_overlay_slots{};
    std::vector<std::size_t> event_timeline_slots{};
    // Per capture slot of the invocation, the compact realtime output index.
    std::vector<std::size_t> sample_capture_outputs{};
    std::vector<std::size_t> event_capture_outputs{};
};

// The Sequential begin index of one binding, written before every invocation.
// It advances from one generation pace cursor, or, for an input bound directly
// to a Tick slot (cursor == no_index), from the slot window's first frame.
struct RootBeginIndex {
    std::size_t field_offset = 0;
    std::size_t cursor = no_index;
    std::int32_t pace_exponent = 0;
};

struct RootPrimitive {
    std::vector<std::shared_ptr<ImmutableConfigData const>> immutable_config_data{};
    AlignedBytes node_data{};
    details::NodeCompilerOperations callbacks{};
    // Root-scratch offsets of the primitive's reflected binding arrays and of
    // its ReflectedNodeTickContext. The context holds the same values for every
    // invocation of one root call, so the root completes it once per call.
    std::size_t sample_inputs_offset = 0;
    std::size_t sample_outputs_offset = 0;
    std::size_t event_inputs_offset = 0;
    std::size_t event_outputs_offset = 0;
    std::size_t context_offset = 0;
    std::size_t sample_output_count = 0;
    std::size_t event_output_count = 0;
    std::optional<RootTickSlots> tick{};
    // Entries of SamplePortBindingPlan::manual_controls that target this
    // primitive.
    std::vector<std::size_t> manual_controls{};
    // Only inputs whose sources may be absent need a resolution pass.
    std::vector<std::size_t> coverage_inputs{};
    struct InputObservation { std::size_t slot, port, channel; };
    std::vector<InputObservation> input_observations{};
    std::vector<std::size_t> live_coverage_outputs{};
    std::vector<bool> live_sample_inputs_with_manual_source{};
    std::vector<std::vector<SequentialCoverageProjection>> live_event_inputs{};
    // Compact realtime output binding and semantic output-coverage index.
    std::vector<std::pair<std::size_t, std::size_t>> sparse_event_outputs{};
    // Callback-local write starts, so coverage filtering does not revisit
    // events inherited in an output's history/in-flight window.
    std::size_t sparse_event_begins_offset = 0;
    std::vector<RootBeginIndex> begin_indices{};
    // Per event input: a Tick slot supplies it, so its activity is not shared
    // through the event representation's activity summary.
    std::vector<std::uint8_t> slot_supplied_event_inputs{};
    // Representations written by the realtime outputs.
    std::vector<std::size_t> output_sample_representations{};
    std::vector<std::size_t> output_event_representations{};
};

enum class RootOperationKind : std::uint8_t {
    event_sequence_reset,
    event_ring_prune,
    event_carry_restore,
    event_carry_commit,
    event_merge,
    event_materialize,
    event_feedback_append,
    sample_carry_restore,
    sample_materialization,
    sample_composition,
    sample_feedback_write,
    sample_carry_commit,
    observation_output,
    observation_state,
};

// One storage operation. index selects the operation's plan entry; for a
// sequence reset it is the event representation itself.
struct RootOperation {
    RootOperationKind kind = RootOperationKind::event_sequence_reset;
    std::size_t index = 0;
};

// One execution step: its primitive and the operations before and after its
// invocation, in the order PrimitiveExecutionStep defines.
struct RootStep {
    std::size_t primitive = 0;
    std::size_t maximum_block_size = 0;
    std::vector<RootOperation> before{};
    std::vector<RootOperation> after{};
    bool captures = false;
};

// The compiled code that replaces one region's interpreted invocation: a
// cyclic region's slice loop, or an acyclic region's one step. It holds the
// per-call inputs the root gathers for it.
struct RootRegionSpecialization {
    RegionSpecializationFunction function = nullptr;
    std::shared_ptr<void const> lifetime{};
    // The region's RootStep indices, in ExecutionRegionPlan order.
    std::vector<std::size_t> steps{};
    // Per begin index of every step, in request order: its pace cursor.
    std::vector<std::size_t> begin_cursors{};
    // Graph-wide representation indices in the dense order used by the
    // compiled request. The root resolves their addresses for every call.
    std::vector<std::size_t> sample_representations{};
    // The project-rate cursor used by generated sample compositions, or
    // no_index when the specialization contains none.
    std::size_t project_cursor = no_index;
};

// The operations around one cyclic region's complete root call, and the
// compiled code of any specialized region.
struct RootRegion {
    std::vector<RootOperation> before{};
    std::vector<RootOperation> after{};
    std::optional<RootRegionSpecialization> specialization{};
};

// How one acyclic ready group runs: first the regions that have compiled
// code, then the remaining steps through the interpreter, as one native batch
// when more than one remains. The group's steps are independent, so this
// order computes what one batch of all of them computes.
struct RootAcyclicGroup {
    bool has_step_operations = false;
    std::vector<std::size_t> specialized_regions{};
    std::vector<std::size_t> interpreted_steps{};
};

struct ContinuousActivitySummary {
    bool valid = false;
    std::size_t representation_epoch = 0;
    GlobalIndex index{};
    std::size_t block_size = 0;
    std::uint32_t flags = 0;
};

struct EventActivitySummary {
    bool valid = false;
    std::size_t representation_epoch = 0;
    GlobalIndex index{};
    std::size_t block_size = 0;
    bool empty = false;
};

struct PrimitiveActivityDecision {
    detail::PrimitiveActivityPlan const* plan = nullptr;
    detail::PrimitiveActivityState* state = nullptr;
    bool inputs_constant = true;
    bool inputs_unchanged = true;
    bool should_skip = false;
};

struct ManualControlLatch {
    std::uint32_t target_bits = 0;
    std::uint32_t effective_bits = 0;
};

// Offsets of the per-call arrays that follow the root scratch. All are
// relative to the aligned scratch base.
struct RootFrameLayout {
    std::size_t manual_control_latches = 0;
    std::size_t sample_representations = 0;
    std::size_t event_representations = 0;
    std::size_t authored_extents = 0;
    std::size_t pace_cursors = 0;
    std::size_t sample_epochs = 0;
    std::size_t event_epochs = 0;
    std::size_t continuous_summaries = 0;
    std::size_t event_summaries = 0;
    std::size_t tick_lanes = 0;
    std::size_t skip_lanes = 0;
    std::size_t tick_decisions = 0;
    std::size_t tick_lane_primitives = 0;
    std::size_t specialization_steps = 0;
    std::size_t specialization_begin_bases = 0;
    std::size_t specialization_sample_representations = 0;
    std::size_t specialization_workspace = 0;
    std::size_t size = 0;
};
} // namespace realtime

// Immutable after construction. Only the builder and executor need this definition.
struct RealtimeRootPlan {
    // Plan sections only the realtime root reads, taken from the LoweringPlan.
    detail::SamplePortBindingPlan sample_ports{};
    std::vector<std::size_t> manual_target_latches{};
    detail::EventPortBindingPlan event_ports{};
    detail::ExecutionPlan execution{};
    detail::RootStackBufferPlan root_stack{};
    std::vector<std::optional<detail::PrimitiveActivityPlan>> primitive_activity{};
    std::vector<detail::ContinuousActivityFrontierPlan> activity_frontiers{};

    // Facts derived once from those sections and the remaining plan.
    std::vector<realtime::RootPrimitive> primitives{};
    std::vector<std::optional<detail::LiveTickCoverageHistoryPlan>> live_coverage_histories{};
    // Indexed like ExecutionPlan::primitive_steps and ExecutionPlan::regions.
    std::vector<realtime::RootStep> steps{};
    std::vector<realtime::RootRegion> regions{};
    // Indexed like ExecutionPlan::ready_groups. Entries of cyclic groups stay
    // empty.
    std::vector<realtime::RootAcyclicGroup> acyclic_groups{};
    // Primitives with Tick runtime slots.
    std::vector<std::size_t> tick_primitives{};
    std::vector<realtime::RootAddress> sample_addresses{};
    std::vector<realtime::RootAddress> event_addresses{};
    std::vector<realtime::AlignedBytes> immutable_storage{};
    // Per sample representation, its producer authored extent or no_index,
    // and its pace's Sequential cursor or no_index.
    std::vector<std::size_t> authored_extents{};
    std::vector<std::size_t> representation_cursors{};
    // Indexed like SampleStoragePlan::materializations, compositions, and
    // feedback_timelines. Entries of operations no step schedules stay empty.
    std::vector<realtime::RootMaterialization> materializations{};
    std::vector<std::vector<realtime::RootContribution>> compositions{};
    std::vector<std::vector<realtime::RootContribution>> feedback_writes{};
    // The reflected binding arrays of every primitive. Only the coalesced
    // ranges used by interpreted primitives are copied into the root scratch.
    // Specialized primitives build their static records in the shared
    // specialization workspace. Pointer fields are written separately for
    // both paths because their addresses are resolved per root call.
    std::vector<std::byte> binding_template{};
    std::vector<realtime::BindingTemplateCopy> binding_template_copies{};
    std::vector<realtime::BindingPointer> binding_pointers{};
    std::size_t binding_offset = 0;
    std::size_t scratch_size = 0;
    std::size_t scratch_alignment = 1;
    std::size_t maximum_batch_lanes = 0;
    // Producer lists in composed sums can exceed a speaker layout's channel
    // count. One frame-local array serves every composition, sequentially.
    std::size_t maximum_composition_sources = 0;
    std::size_t composition_sources_offset = 0;
    // The largest step count, begin-index count, dense sample-representation
    // count, and reusable binding/context workspace of any specialization.
    std::size_t maximum_specialization_steps = 0;
    std::size_t maximum_specialization_begin_indices = 0;
    std::size_t maximum_specialization_sample_representations = 0;
    std::size_t maximum_specialization_workspace_size = 0;
    realtime::RootFrameLayout frame{};
    std::vector<std::optional<std::size_t>> observation_slots{};
    std::vector<std::vector<std::size_t>> observation_channels{};
    struct StateProbe { std::size_t slot = 0; std::size_t storage_offset = 0; };
    std::vector<StateProbe> state_probes{};
    bool has_tick_slots = false;
    // Some primitive has a TTL activity plan, so the root keeps
    // representation epochs for the activity summaries.
    bool tracks_activity = false;
};

} // namespace iv::graph_jit
