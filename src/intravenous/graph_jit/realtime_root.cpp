#include <intravenous/graph_jit/realtime_root.h>

#include <intravenous/channel_layout.h>
#include <intravenous/graph/reflected_node_operations.h>
#include <intravenous/graph_jit/activity_runtime.h>
#include <intravenous/graph_jit/event_conversion_runtime.h>
#include <intravenous/graph_jit/event_retention_runtime.h>
#include <intravenous/graph_jit/lowering_plan.h>
#include <intravenous/graph_jit/tick_invocation_call.h>
#include <intravenous/node/manual_control.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <alloca.h>

namespace iv::graph_jit {
namespace {
constexpr std::size_t no_index = std::numeric_limits<std::size_t>::max();

static_assert(maximum_supported_channel_count <= 64,
    "realtime root channel masks hold one bit per channel");

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

// The operations around one cyclic region's complete root call.
struct RootRegion {
    std::vector<RootOperation> before{};
    std::vector<RootOperation> after{};
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

// Offsets of the per-call arrays that follow the root scratch. All are
// relative to the aligned scratch base.
struct RootFrameLayout {
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
    std::size_t size = 0;
};
} // namespace

struct RealtimeRootPlan {
    // Plan sections only the realtime root reads, taken from the LoweringPlan.
    detail::SamplePortBindingPlan sample_ports{};
    detail::EventPortBindingPlan event_ports{};
    detail::ExecutionPlan execution{};
    detail::RootStackBufferPlan root_stack{};
    std::vector<std::optional<detail::PrimitiveActivityPlan>> primitive_activity{};
    std::vector<detail::ContinuousActivityFrontierPlan> activity_frontiers{};

    // Facts derived once from those sections and the remaining plan.
    std::vector<RootPrimitive> primitives{};
    // Indexed like ExecutionPlan::primitive_steps and ExecutionPlan::regions.
    std::vector<RootStep> steps{};
    std::vector<RootRegion> regions{};
    // Primitives with Tick runtime slots.
    std::vector<std::size_t> tick_primitives{};
    std::vector<RootAddress> sample_addresses{};
    std::vector<RootAddress> event_addresses{};
    std::vector<AlignedBytes> immutable_storage{};
    // Per sample representation, its producer authored extent or no_index,
    // and its pace's Sequential cursor or no_index.
    std::vector<std::size_t> authored_extents{};
    std::vector<std::size_t> representation_cursors{};
    // Indexed like SampleStoragePlan::materializations, compositions, and
    // feedback_timelines. Entries of operations no step schedules stay empty.
    std::vector<RootMaterialization> materializations{};
    std::vector<std::vector<RootContribution>> compositions{};
    std::vector<std::vector<RootContribution>> feedback_writes{};
    // The reflected binding arrays of every primitive, copied to
    // binding_offset in the root scratch at every call, and the pointer fields
    // written after that copy.
    std::vector<std::byte> binding_template{};
    std::vector<BindingPointer> binding_pointers{};
    std::size_t binding_offset = 0;
    std::size_t scratch_size = 0;
    std::size_t scratch_alignment = 1;
    std::size_t maximum_batch_lanes = 0;
    RootFrameLayout frame{};
    bool has_tick_slots = false;
    // Some primitive has a TTL activity plan, so the root keeps
    // representation epochs for the activity summaries.
    bool tracks_activity = false;
};

namespace {
template<class T>
T load_raw(std::byte const* base, std::size_t offset) noexcept
{
    T value{};
    std::memcpy(&value, base + offset, sizeof(value));
    return value;
}

template<class T>
void store_raw(std::byte* base, std::size_t offset, T value) noexcept
{
    std::memcpy(base + offset, &value, sizeof(value));
}

std::size_t align_up(std::size_t value, std::size_t alignment) noexcept
{
    return (value + alignment - 1) & ~(alignment - 1);
}

struct LatticeProjection {
    std::uint64_t begin_index = 0;
    std::size_t value_count = 0;
};

LatticeProjection project_lattice(
    GlobalIndex project_index,
    std::size_t project_count,
    std::int32_t pace_exponent) noexcept
{
    auto const bits = project_index.bits();
    if (pace_exponent >= 0) {
        auto const shift = static_cast<unsigned>(pace_exponent);
        return {
            .begin_index = bits << shift,
            .value_count = project_count << shift,
        };
    }
    auto const shift = static_cast<unsigned>(-pace_exponent);
    if (shift == sizeof(std::size_t) * 8) {
        auto const offset = std::uint64_t{0} - bits;
        return {
            .begin_index = 0,
            .value_count = offset < project_count ? 1u : 0u,
        };
    }
    auto const spacing = std::uint64_t{1} << shift;
    auto const mask = spacing - 1;
    auto const offset = (spacing - (bits & mask)) & mask;
    if (offset >= project_count) {
        return {.begin_index = (bits + offset) >> shift};
    }
    return {
        .begin_index = (bits + offset) >> shift,
        .value_count = 1 + ((project_count - offset - 1) >> shift),
    };
}

// Values of pace_exponent's lattice in [root_index, callback_index).
std::size_t projected_prefix(
    GlobalIndex root_index,
    GlobalIndex callback_index,
    std::int32_t pace_exponent) noexcept
{
    return project_lattice(
        root_index,
        static_cast<std::size_t>(root_index.forward_distance_to(callback_index)),
        pace_exponent).value_count;
}

bool supports_linear_audio_channel_conversion(
    ContinuousValueStorageGeometry const& geometry) noexcept
{
    return geometry.channel_capability == ContinuousValueChannelCapability::audio
        && geometry.element_alignment >= alignof(float)
        && geometry.channel_scalar_lane_count != 0
        && geometry.value_size_bytes
            == geometry.channel_scalar_lane_count * sizeof(float);
}

float mixed_value(
    ChannelMix const& mix,
    std::size_t converted_channel,
    float const* sources) noexcept
{
    auto const begin = mix.term_begins[converted_channel];
    auto const end = mix.term_begins[converted_channel + 1];
    if (begin == end) return 0.0f;
    auto term = [&](ChannelTerm const& selected) noexcept {
        auto const value = sources[selected.source_channel];
        return selected.coefficient == 1.0f
            ? value
            : value * selected.coefficient;
    };
    auto value = term(mix.terms[begin]);
    for (auto index = begin + 1; index < end; ++index) {
        value += term(mix.terms[index]);
    }
    return value;
}

// ---------------------------------------------------------------------------
// Root call
// ---------------------------------------------------------------------------

struct RootFrame {
    RealtimeRootPlan const& plan;
    std::byte* storage_base = nullptr;
    std::byte* scratch = nullptr;
    std::span<std::byte*> sample_representations{};
    std::span<std::byte*> event_representations{};
    std::span<std::size_t> producer_authored_extents{};
    std::span<std::uint64_t> sequential_pace_cursors{};
    std::span<std::size_t> sample_representation_epochs{};
    std::span<std::size_t> event_representation_epochs{};
    std::span<ContinuousActivitySummary> continuous_activity_summaries{};
    std::span<EventActivitySummary> event_activity_summaries{};
    std::span<ReflectedNodeTickBatchLane> tick_lanes{};
    std::span<ReflectedNodeTickBatchLane> skip_lanes{};
    std::span<PrimitiveActivityDecision> tick_decisions{};
    std::span<std::size_t> tick_lane_primitives{};
    TickInvocationCall const* invocation = nullptr;
    GlobalIndex root_index{};
};

template<class T>
std::span<T> frame_array(
    std::byte* scratch,
    std::size_t offset,
    std::size_t count) noexcept
{
    if (count == 0) return {};
    return {reinterpret_cast<T*>(scratch + offset), count};
}

std::byte* resolve_address(
    RootAddress const& address,
    std::byte* storage_base,
    std::byte* scratch) noexcept
{
    switch (address.base) {
    case RootAddressBase::immutable:
        return const_cast<std::byte*>(address.immutable);
    case RootAddressBase::node_storage:
        return storage_base + address.offset;
    case RootAddressBase::scratch:
        return scratch + address.offset;
    }
    return nullptr;
}

// The physical Sequential index of callback_index on one generation pace
// cursor.
std::uint64_t sequential_begin_index(
    RootFrame const& frame,
    std::size_t cursor,
    std::int32_t pace_exponent,
    GlobalIndex callback_index) noexcept
{
    return frame.sequential_pace_cursors[cursor]
        + projected_prefix(frame.root_index, callback_index, pace_exponent);
}

std::uint64_t representation_begin_index(
    RootFrame const& frame,
    std::size_t representation,
    GlobalIndex callback_index) noexcept
{
    return sequential_begin_index(
        frame,
        frame.plan.representation_cursors[representation],
        frame.plan.sample_ports.storage.representations[representation]
            .absolute_pace_exponent,
        callback_index);
}

std::uint64_t pace_begin_index(
    RootFrame const& frame,
    std::int32_t pace_exponent,
    GlobalIndex callback_index) noexcept
{
    auto const& cursors = frame.plan.sample_ports.storage.sequential_pace_cursors;
    std::size_t cursor = 0;
    while (cursors[cursor].absolute_pace_exponent != pace_exponent) ++cursor;
    return sequential_begin_index(frame, cursor, pace_exponent, callback_index);
}

std::byte* continuous_value(
    RootFrame const& frame,
    std::size_t representation_index,
    std::uint64_t value_index,
    std::size_t channel) noexcept
{
    auto const& representation = frame.plan.sample_ports.storage
        .representations[representation_index];
    auto const channels = channel_count(representation.channel_layout);
    auto const slot = static_cast<std::size_t>(value_index)
        & (representation.frame_capacity - 1);
    auto const value = representation.channel_layout.sample_layout
            == SampleStreamLayout::planar
        ? channel * representation.frame_capacity + slot
        : slot * channels + channel;
    return frame.sample_representations[representation_index]
        + value * representation.geometry.value_size_bytes;
}

float load_lane(std::byte const* value, std::size_t lane) noexcept
{
    return load_raw<float>(value, lane * sizeof(float));
}

void store_lane(std::byte* value, std::size_t lane, float sample) noexcept
{
    store_raw(value, lane * sizeof(float), sample);
}

void materialize_sample(
    RootFrame& frame,
    std::size_t materialization_index,
    GlobalIndex callback_index,
    std::size_t callback_size) noexcept
{
    auto const& storage = frame.plan.sample_ports.storage;
    auto const& operation = storage.materializations[materialization_index];
    auto const& derived = frame.plan.materializations[materialization_index];
    auto const& target = storage.representations[operation.target_representation];
    auto const projected = project_lattice(
        callback_index, callback_size, target.absolute_pace_exponent).value_count;
    if (projected == 0) return;
    auto const begin = representation_begin_index(
        frame, operation.target_representation, callback_index);
    auto const first = begin - operation.retained_before;
    auto const count = projected
        + (operation.retained_before - operation.latest_read_latency);
    // A transition root's copy into an input's transition-only storage skips
    // the frames before the end of the window the cutover placed there.
    std::size_t first_offset = 0;
    if (operation.transition_cutover_index_storage_offset) {
        auto const cutover = load_raw<std::uint64_t>(
            frame.storage_base,
            *operation.transition_cutover_index_storage_offset);
        auto const skip = static_cast<std::int64_t>(
            cutover + static_cast<std::uint64_t>(operation.transition_first_frame)
            - first);
        if (skip > 0) {
            first_offset = std::min(static_cast<std::size_t>(skip), count);
        }
    }
    auto const source_channels = channel_count(operation.source_layout);
    auto const target_channels = derived.mix.converted_channel_count();
    auto const lanes = target.geometry.channel_scalar_lane_count;
    std::array<float, maximum_supported_channel_count> sources{};
    for (auto offset = first_offset; offset < count; ++offset) {
        auto const value_index = first + offset;
        for (std::size_t lane = 0; lane < lanes; ++lane) {
            for (std::size_t channel = 0; channel < source_channels; ++channel) {
                sources[channel] = load_lane(
                    continuous_value(
                        frame,
                        operation.source_representation,
                        value_index,
                        channel),
                    lane);
            }
            for (std::size_t channel = 0; channel < target_channels; ++channel) {
                if ((derived.written_channels >> channel & 1u) == 0) continue;
                store_lane(
                    continuous_value(
                        frame,
                        operation.target_representation,
                        value_index,
                        channel),
                    lane,
                    mixed_value(derived.mix, channel, sources.data()));
            }
        }
    }
}

std::size_t available_revision(
    RootFrame const& frame,
    std::size_t source_representation,
    std::size_t requested,
    GlobalIndex callback_index) noexcept
{
    if (requested == 0) return 0;
    auto const extent_index = frame.plan.authored_extents[source_representation];
    auto const& extent = frame.plan.sample_ports.storage
        .producer_authored_extents[extent_index];
    auto const prior = std::min(
        frame.producer_authored_extents[extent_index],
        extent.maximum_revision_frames);
    auto const prefix = projected_prefix(
        frame.root_index, callback_index, extent.absolute_pace_exponent);
    return std::min(
        requested,
        prior + std::min(prefix, extent.maximum_revision_frames - prior));
}

void compose_samples(
    RootFrame& frame,
    std::span<detail::SampleCompositionContributionPlan const> contributions,
    std::span<RootContribution const> derived,
    std::size_t target_representation,
    std::size_t target_history,
    bool shift_writes_by_read_latency,
    GlobalIndex callback_index,
    std::size_t callback_size) noexcept
{
    auto const& storage = frame.plan.sample_ports.storage;
    auto const& target = storage.representations[target_representation];
    auto const count = project_lattice(
        callback_index, callback_size, target.absolute_pace_exponent).value_count
        + target_history;
    if (count == 0) return;
    auto const first = representation_begin_index(
        frame, target_representation, callback_index) - target_history;
    auto const lanes = target.geometry.channel_scalar_lane_count;

    // Shifted feedback writes rewrite only the history frames that every
    // source of a contribution has already authored.
    std::array<std::size_t, maximum_supported_channel_count> first_authored{};
    for (std::size_t index = 0; index < contributions.size(); ++index) {
        auto available = target_history;
        if (shift_writes_by_read_latency && target_history != 0) {
            for (auto const& source : contributions[index].sources) {
                available = std::min(
                    available,
                    available_revision(
                        frame,
                        source.source_representation,
                        target_history,
                        callback_index));
            }
        }
        first_authored[index] = target_history - available;
    }

    std::array<float, maximum_supported_channel_count> sources{};
    for (std::size_t offset = 0; offset < count; ++offset) {
        auto const target_frame = first + offset;
        for (std::size_t index = 0; index < contributions.size(); ++index) {
            auto const& contribution = contributions[index];
            auto const& mix = derived[index].mix;
            auto const authored = offset >= first_authored[index];
            auto const aligned = contribution.feedback_alignment_representation
                != detail::no_sample_representation;
            for (std::size_t lane = 0; lane < lanes; ++lane) {
                for (std::size_t source_index = 0;
                     source_index < contribution.sources.size();
                     ++source_index) {
                    auto const& source = contribution.sources[source_index];
                    auto const source_frame = shift_writes_by_read_latency
                        ? target_frame
                        : target_frame - source.read_latency;
                    sources[source_index] = load_lane(
                        continuous_value(
                            frame,
                            source.source_representation,
                            source_frame,
                            source.source_channel),
                        lane);
                }
                if (aligned) {
                    // Stage the current source values, then read each source
                    // where it aligns with the contribution's write latency.
                    for (std::size_t source_index = 0;
                         source_index < contribution.sources.size();
                         ++source_index) {
                        if (!authored) continue;
                        store_lane(
                            continuous_value(
                                frame,
                                contribution.feedback_alignment_representation,
                                target_frame,
                                source_index),
                            lane,
                            sources[source_index]);
                    }
                    for (std::size_t source_index = 0;
                         source_index < contribution.sources.size();
                         ++source_index) {
                        auto const latency_delta =
                            contribution.sources[source_index].read_latency
                            - contribution.feedback_alignment_write_latency;
                        sources[source_index] = load_lane(
                            continuous_value(
                                frame,
                                contribution.feedback_alignment_representation,
                                target_frame - latency_delta,
                                source_index),
                            lane);
                    }
                }
                for (std::size_t converted = 0;
                     converted < contribution.target_channels.size();
                     ++converted) {
                    if (shift_writes_by_read_latency && !authored) continue;
                    auto const write_frame = shift_writes_by_read_latency
                        ? target_frame + derived[index].write_latencies[converted]
                        : target_frame;
                    store_lane(
                        continuous_value(
                            frame,
                            target_representation,
                            write_frame,
                            contribution.target_channels[converted]),
                        lane,
                        mixed_value(mix, converted, sources.data()));
                }
            }
        }
    }
}

void write_feedback_samples(
    RootFrame& frame,
    std::size_t timeline_index,
    GlobalIndex callback_index,
    std::size_t callback_size) noexcept
{
    auto const& storage = frame.plan.sample_ports.storage;
    auto const& timeline = storage.feedback_timelines[timeline_index];
    if (timeline.writer.kind
        == detail::SampleFeedbackTimelineWriterKind::composition) {
        compose_samples(
            frame,
            timeline.writer.composition_contributions,
            frame.plan.feedback_writes[timeline_index],
            timeline.timeline_representation,
            timeline.writer.revision_frames,
            true,
            callback_index,
            callback_size);
        return;
    }
    auto const& target = storage.representations[timeline.timeline_representation];
    auto const projected = project_lattice(
        callback_index, callback_size, target.absolute_pace_exponent).value_count;
    auto const begin = representation_begin_index(
        frame, timeline.timeline_representation, callback_index);
    auto const revision = available_revision(
        frame,
        timeline.writer.source_representation,
        timeline.writer.revision_frames,
        callback_index);
    auto const first = begin - revision;
    auto const count = projected + revision;
    auto const channels = channel_count(timeline.channel_layout);
    for (std::size_t offset = 0; offset < count; ++offset) {
        for (std::size_t channel = 0; channel < channels; ++channel) {
            std::memcpy(
                continuous_value(
                    frame,
                    timeline.timeline_representation,
                    first + offset,
                    channel),
                continuous_value(
                    frame,
                    timeline.writer.source_representation,
                    first + offset,
                    channel),
                target.geometry.value_size_bytes);
        }
    }
}

void sample_carry(
    RootFrame& frame,
    detail::SampleCarryOperationPlan const& operation,
    GlobalIndex callback_index,
    std::size_t callback_size,
    bool restore) noexcept
{
    auto const& storage = frame.plan.sample_ports.storage;
    auto const& representation = storage.representations[
        operation.representation_index];
    auto const projected = project_lattice(
        callback_index, callback_size, representation.absolute_pace_exponent)
        .value_count;
    if (projected == 0) return;
    auto const& allocation = storage.persistent_allocations[
        operation.persistent_allocation];
    auto const begin = representation_begin_index(
        frame, operation.representation_index, callback_index);
    auto const retained_before = operation.retained_frames
        - operation.future_frames;
    auto const first = restore
        ? begin - retained_before
        : begin + projected - retained_before;
    auto* carry = frame.storage_base + allocation.storage_offset;
    auto const channels = channel_count(allocation.channel_layout);
    auto const value_size = allocation.geometry.value_size_bytes;
    auto const planar = allocation.channel_layout.sample_layout
        == SampleStreamLayout::planar;
    for (std::size_t offset = 0; offset < operation.retained_frames; ++offset) {
        for (std::size_t channel = 0; channel < channels; ++channel) {
            auto* working = continuous_value(
                frame, operation.representation_index, first + offset, channel);
            auto* retained = carry + value_size * (planar
                ? channel * operation.retained_frames + offset
                : offset * channels + channel);
            if (restore) {
                std::memcpy(working, retained, value_size);
            } else {
                std::memcpy(retained, working, value_size);
            }
        }
    }
}

void event_carry(
    RootFrame& frame,
    detail::EventCarryPlan const& carry,
    GlobalIndex index,
    std::size_t block_size,
    bool restore) noexcept
{
    auto const& ports = frame.plan.event_ports.ports;
    auto const& working = ports[carry.working_representation];
    auto const& persistent = ports[carry.persistent_representation];
    auto* working_base = frame.event_representations[carry.working_representation];
    auto* persistent_base = frame.event_representations[carry.persistent_representation];
    auto* working_events = working_base + working.events_relative_offset;
    auto* persistent_events = persistent_base + persistent.events_relative_offset;
    auto* working_indices = working.has_source_indices
        ? reinterpret_cast<std::size_t*>(
            working_base + working.source_indices_relative_offset)
        : nullptr;
    auto* persistent_indices = persistent.has_source_indices
        ? reinterpret_cast<std::size_t*>(
            persistent_base + persistent.source_indices_relative_offset)
        : nullptr;
    if (restore) {
        auto const count = load_raw<std::size_t>(
            persistent_base, persistent.count_relative_offset);
        auto const initialized = load_raw<std::uint8_t>(
            persistent_base, persistent.timeline_initialized_relative_offset);
        auto const expected = initialized != 0
            ? GlobalIndex::from_bits(load_raw<std::uint64_t>(
                persistent_base, persistent.expected_next_time_relative_offset))
            : index;
        detail::iv_graph_jit_rebase_event_sequence(
            persistent_events, count, expected, index);
        store_raw<std::uint8_t>(
            persistent_base, persistent.timeline_initialized_relative_offset, 1);
        store_raw<std::uint64_t>(
            persistent_base,
            persistent.expected_next_time_relative_offset,
            index.advanced_by(block_size).bits());
        auto const restored = working.has_source_indices
            ? detail::iv_graph_jit_restore_ordered_event_carry(
                persistent_events,
                persistent_indices,
                count,
                working_events,
                working_indices,
                working.event_capacity)
            : detail::iv_graph_jit_restore_event_carry(
                persistent_events,
                count,
                working_events,
                working.event_capacity);
        store_raw(working_base, working.count_relative_offset, restored);
        return;
    }
    auto const count = load_raw<std::size_t>(
        working_base, working.count_relative_offset);
    auto const committed = working.has_source_indices
        ? detail::iv_graph_jit_commit_ordered_event_carry(
            working_events,
            working_indices,
            count,
            index,
            block_size,
            carry.retained_history_samples,
            carry.retained_latency_samples,
            persistent_events,
            persistent_indices,
            persistent.event_capacity)
        : detail::iv_graph_jit_commit_event_carry(
            working_events,
            count,
            index,
            block_size,
            carry.retained_history_samples,
            carry.retained_latency_samples,
            persistent_events,
            persistent.event_capacity);
    store_raw(persistent_base, persistent.count_relative_offset, committed);
}

void prune_event_ring(
    RootFrame& frame,
    detail::EventPersistentRingPlan const& ring,
    GlobalIndex index,
    std::size_t block_size) noexcept
{
    auto const& storage = frame.plan.event_ports.ports[ring.storage];
    auto* base = frame.event_representations[ring.storage];
    auto const read = load_raw<std::size_t>(base, storage.read_index_relative_offset);
    auto const write = load_raw<std::size_t>(base, storage.write_index_relative_offset);
    auto* events = base + storage.events_relative_offset;
    auto const initialized = load_raw<std::uint8_t>(
        base, storage.timeline_initialized_relative_offset);
    auto const expected = initialized != 0
        ? GlobalIndex::from_bits(load_raw<std::uint64_t>(
            base, storage.expected_next_time_relative_offset))
        : index;
    detail::iv_graph_jit_rebase_event_ring(
        events, storage.event_capacity, read, write, expected, index);
    store_raw<std::uint8_t>(base, storage.timeline_initialized_relative_offset, 1);
    store_raw<std::uint64_t>(
        base,
        storage.expected_next_time_relative_offset,
        index.advanced_by(block_size).bits());
    store_raw(
        base,
        storage.read_index_relative_offset,
        detail::iv_graph_jit_prune_event_persistent_ring(
            events,
            storage.event_capacity,
            read,
            write,
            index,
            ring.retained_history_samples));
}

void merge_events(RootFrame& frame, detail::EventMergePlan const& merge) noexcept
{
    auto const& ports = frame.plan.event_ports.ports;
    auto const& target = ports[merge.target_representation];
    auto* target_base = frame.event_representations[merge.target_representation];
    auto* target_events = target_base + target.events_relative_offset;
    auto* target_indices = target.has_source_indices
        ? reinterpret_cast<std::size_t*>(
            target_base + target.source_indices_relative_offset)
        : nullptr;
    auto const read = target.persistent_ring
        ? load_raw<std::size_t>(target_base, target.read_index_relative_offset)
        : 0;
    auto write = target.persistent_ring
        ? load_raw<std::size_t>(target_base, target.write_index_relative_offset)
        : merge.preserve_existing_target
            ? load_raw<std::size_t>(target_base, target.count_relative_offset)
            : 0;
    // Every merge into one sequence orders from the root call's
    // history-inclusive origin, including each slice of an SCC.
    auto const origin = frame.root_index.retreated_by(
        merge.ordering_history_samples);
    for (std::size_t index = 0; index < merge.source_representations.size(); ++index) {
        auto const representation = merge.source_representations[index];
        auto const& source = ports[representation];
        auto* source_base = frame.event_representations[representation];
        auto const source_count = load_raw<std::size_t>(
            source_base, source.count_relative_offset);
        auto* source_events = source_base + source.events_relative_offset;
        auto const delay = merge.source_delays.empty()
            ? std::size_t{0}
            : merge.source_delays[index];
        write = target.has_source_indices
            ? detail::iv_graph_jit_merge_ordered_event_sequence(
                target_events,
                target_indices,
                target.event_capacity,
                read,
                write,
                source_events,
                source.event_capacity,
                source_count,
                merge.source_indices[index],
                origin,
                delay)
            : detail::iv_graph_jit_merge_event_sequence(
                target_events,
                target.event_capacity,
                read,
                write,
                source_events,
                source.event_capacity,
                source_count,
                origin,
                delay);
    }
    store_raw(
        target_base,
        target.persistent_ring
            ? target.write_index_relative_offset
            : target.count_relative_offset,
        write);
}

std::uint32_t conversion_step(
    EventConversionPlan const& conversion,
    std::size_t index) noexcept
{
    return index < conversion.step_count
        ? static_cast<std::uint32_t>(conversion.steps[index])
        : 0;
}

void materialize_events(
    RootFrame& frame,
    detail::EventMaterializationPlan const& operation,
    GlobalIndex index,
    std::size_t block_size) noexcept
{
    auto const& ports = frame.plan.event_ports.ports;
    auto const& source = ports[operation.source_representation];
    auto const& target = ports[operation.target_representation];
    auto* source_base = frame.event_representations[operation.source_representation];
    auto* target_base = frame.event_representations[operation.target_representation];
    auto* source_events = source_base + source.events_relative_offset;
    auto* target_events = target_base + target.events_relative_offset;
    auto const* source_indices =
        operation.source_index_filter || operation.preserve_source_indices
        ? reinterpret_cast<std::size_t const*>(
            source_base + source.source_indices_relative_offset)
        : nullptr;
    auto* target_indices = operation.preserve_source_indices
        ? reinterpret_cast<std::size_t*>(
            target_base + target.source_indices_relative_offset)
        : nullptr;
    std::size_t count = 0;
    if (operation.source_index_filter
        || operation.select_invocation_window
        || source.persistent_ring) {
        auto const read = source.persistent_ring
            ? load_raw<std::size_t>(source_base, source.read_index_relative_offset)
            : 0;
        auto const write = source.persistent_ring
            ? load_raw<std::size_t>(source_base, source.write_index_relative_offset)
            : load_raw<std::size_t>(source_base, source.count_relative_offset);
        count = detail::iv_graph_jit_materialize_event_sequence(
            static_cast<std::uint32_t>(source.type),
            static_cast<std::uint32_t>(target.type),
            conversion_step(operation.conversion, 0),
            conversion_step(operation.conversion, 1),
            conversion_step(operation.conversion, 2),
            operation.conversion.step_count,
            source_events,
            source_indices,
            operation.source_index_filter.value_or(0),
            static_cast<std::uint32_t>(operation.source_index_filter.has_value()),
            source.event_capacity,
            read,
            write,
            index,
            block_size,
            operation.history_samples,
            target_events,
            target.event_capacity,
            target_indices);
    } else {
        auto const source_count = std::min(
            load_raw<std::size_t>(source_base, source.count_relative_offset),
            source.event_capacity);
        if (operation.conversion.step_count == 0) {
            count = std::min(source_count, target.event_capacity);
            std::memcpy(target_events, source_events, count * sizeof(TimedEvent));
            if (target_indices) {
                std::memcpy(
                    target_indices, source_indices, count * sizeof(std::size_t));
            }
        } else {
            count = detail::iv_graph_jit_convert_event_sequence(
                static_cast<std::uint32_t>(source.type),
                static_cast<std::uint32_t>(target.type),
                conversion_step(operation.conversion, 0),
                conversion_step(operation.conversion, 1),
                conversion_step(operation.conversion, 2),
                operation.conversion.step_count,
                source_events,
                source_count,
                target_events,
                target.event_capacity,
                source_indices,
                target_indices);
        }
    }
    store_raw(target_base, target.count_relative_offset, count);
}

void append_event_feedback(
    RootFrame& frame,
    detail::EventFeedbackPlan const& feedback,
    GlobalIndex index,
    std::size_t block_size) noexcept
{
    auto const& ports = frame.plan.event_ports.ports;
    auto const& source = ports[feedback.source_representation];
    auto const& target = ports[feedback.target_representation];
    auto* source_base = frame.event_representations[feedback.source_representation];
    auto* target_base = frame.event_representations[feedback.target_representation];
    auto const source_begin = source.persistent_ring
        ? load_raw<std::size_t>(source_base, source.read_index_relative_offset)
        : 0;
    auto const source_end = source.persistent_ring
        ? load_raw<std::size_t>(source_base, source.write_index_relative_offset)
        : std::min(
            load_raw<std::size_t>(source_base, source.count_relative_offset),
            source.event_capacity);
    // Most slices produce no events; they never enter the feedback helper.
    if (source_begin >= source_end) return;
    auto* source_events = source_base + source.events_relative_offset;
    auto const* source_indices = feedback.source_index_filter
        ? reinterpret_cast<std::size_t const*>(
            source_base + source.source_indices_relative_offset)
        : nullptr;
    auto* target_events = target_base + target.events_relative_offset;
    auto const origin = index.retreated_by(feedback.ordering_history_samples);
    auto const filter = feedback.source_index_filter.value_or(0);
    auto const has_filter = static_cast<std::uint32_t>(
        feedback.source_index_filter.has_value());
    auto const select_window = static_cast<std::uint32_t>(
        feedback.selects_final_window);
    auto const window_extent = feedback.selects_final_window ? block_size : 0;
    if (target.persistent_ring) {
        auto* read = reinterpret_cast<std::size_t*>(
            target_base + target.read_index_relative_offset);
        auto* write = reinterpret_cast<std::size_t*>(
            target_base + target.write_index_relative_offset);
        if (source.persistent_ring) {
            detail::iv_graph_jit_append_event_feedback_ring_source(
                source_events, source_indices, filter, has_filter,
                source.event_capacity, source_begin, source_end, origin,
                feedback.loop_extra_latency, target_events,
                target.event_capacity, read, write, select_window, index,
                window_extent);
        } else {
            detail::iv_graph_jit_append_event_feedback(
                source_events, source_indices, filter, has_filter,
                source_begin, source_end, origin,
                feedback.loop_extra_latency, target_events,
                target.event_capacity, read, write, select_window, index,
                window_extent);
        }
        return;
    }
    auto* count = reinterpret_cast<std::size_t*>(
        target_base + target.count_relative_offset);
    if (source.persistent_ring) {
        detail::iv_graph_jit_append_event_feedback_sequence_ring_source(
            source_events, source_indices, filter, has_filter,
            source.event_capacity, source_begin, source_end, origin,
            feedback.loop_extra_latency, target_events,
            target.event_capacity, count, select_window, index,
            window_extent);
    } else {
        detail::iv_graph_jit_append_event_feedback_sequence(
            source_events, source_indices, filter, has_filter,
            source_begin, source_end, origin,
            feedback.loop_extra_latency, target_events,
            target.event_capacity, count, select_window, index,
            window_extent);
    }
}

void mark_sample_representation(RootFrame& frame, std::size_t representation) noexcept
{
    if (frame.plan.tracks_activity) {
        ++frame.sample_representation_epochs[representation];
    }
}

void mark_event_representation(RootFrame& frame, std::size_t representation) noexcept
{
    if (frame.plan.tracks_activity) {
        ++frame.event_representation_epochs[representation];
    }
}

void run_operations(
    RootFrame& frame,
    std::span<RootOperation const> operations,
    GlobalIndex index,
    std::size_t block_size) noexcept
{
    auto const& plan = frame.plan;
    auto const& events = plan.event_ports;
    auto const& storage = plan.sample_ports.storage;
    for (auto const operation : operations) {
        switch (operation.kind) {
        case RootOperationKind::event_sequence_reset:
            store_raw<std::size_t>(
                frame.event_representations[operation.index],
                events.ports[operation.index].count_relative_offset,
                0);
            mark_event_representation(frame, operation.index);
            break;
        case RootOperationKind::event_ring_prune: {
            auto const& selected = events.persistent_rings[operation.index];
            prune_event_ring(frame, selected, index, block_size);
            mark_event_representation(frame, selected.storage);
            break;
        }
        case RootOperationKind::event_carry_restore: {
            auto const& selected = events.carry_operations[operation.index];
            event_carry(frame, selected, index, block_size, true);
            mark_event_representation(frame, selected.working_representation);
            break;
        }
        case RootOperationKind::event_carry_commit: {
            auto const& selected = events.carry_operations[operation.index];
            event_carry(frame, selected, index, block_size, false);
            mark_event_representation(frame, selected.persistent_representation);
            break;
        }
        case RootOperationKind::event_merge: {
            auto const& selected = events.merges[operation.index];
            merge_events(frame, selected);
            mark_event_representation(frame, selected.target_representation);
            break;
        }
        case RootOperationKind::event_materialize: {
            auto const& selected = events.materializations[operation.index];
            materialize_events(frame, selected, index, block_size);
            mark_event_representation(frame, selected.target_representation);
            break;
        }
        case RootOperationKind::event_feedback_append: {
            auto const& selected = events.feedback_operations[operation.index];
            append_event_feedback(frame, selected, index, block_size);
            mark_event_representation(frame, selected.target_representation);
            break;
        }
        case RootOperationKind::sample_carry_restore: {
            auto const& selected = storage.carry_operations[operation.index];
            sample_carry(frame, selected, index, block_size, true);
            mark_sample_representation(frame, selected.representation_index);
            break;
        }
        case RootOperationKind::sample_materialization:
            materialize_sample(frame, operation.index, index, block_size);
            mark_sample_representation(
                frame,
                storage.materializations[operation.index].target_representation);
            break;
        case RootOperationKind::sample_composition: {
            auto const& composition = storage.compositions[operation.index];
            compose_samples(
                frame,
                composition.contributions,
                plan.compositions[operation.index],
                composition.target_representation,
                composition.target_history,
                false,
                index,
                block_size);
            mark_sample_representation(frame, composition.target_representation);
            break;
        }
        case RootOperationKind::sample_feedback_write:
            write_feedback_samples(frame, operation.index, index, block_size);
            mark_sample_representation(
                frame,
                storage.feedback_timelines[operation.index].timeline_representation);
            break;
        case RootOperationKind::sample_carry_commit:
            // A commit writes the retained carry, not the working
            // representation, so no activity summary is invalidated.
            sample_carry(
                frame,
                storage.carry_operations[operation.index],
                index,
                block_size,
                false);
            break;
        }
    }
}

void initialize_bindings(RootFrame& frame) noexcept
{
    auto const& plan = frame.plan;
    if (plan.binding_template.empty()) return;
    std::memcpy(
        frame.scratch + plan.binding_offset,
        plan.binding_template.data(),
        plan.binding_template.size());
    for (auto const& pointer : plan.binding_pointers) {
        std::byte* base = nullptr;
        switch (pointer.base) {
        case BindingPointerBase::sample_representation:
            base = frame.sample_representations[pointer.representation];
            break;
        case BindingPointerBase::event_representation:
            base = frame.event_representations[pointer.representation];
            break;
        case BindingPointerBase::scratch:
            base = frame.scratch;
            break;
        case BindingPointerBase::node_storage:
            base = frame.storage_base;
            break;
        }
        store_raw(frame.scratch, pointer.field_offset, base + pointer.byte_offset);
    }
}

std::byte* bound_channel_value(
    ReflectedContinuousChannelStorageBinding const& channel,
    std::uint64_t index) noexcept
{
    auto const slot = static_cast<std::size_t>(index - channel.value_delay)
        & (channel.value_capacity - 1);
    return channel.storage + slot * channel.value_stride_bytes;
}

// Writes the current window of every slot-fed input that owns a timeline. A
// pure input receives its slot window; a mixed input receives the node's
// combination of its realtime operand with that window. Earlier timeline
// positions are the input's node-owned history and are never rewritten.
void stage_sample_timelines(
    RootFrame& frame,
    std::size_t primitive_index,
    RootTickSlots const& tick,
    GlobalIndex callback_index,
    std::size_t callback_size) noexcept
{
    auto const& plan = frame.plan;
    auto const& inputs = plan.sample_ports.primitives[primitive_index].inputs;
    auto const& callbacks = plan.primitives[primitive_index].callbacks;
    auto const* supplied = frame.invocation->sequential_sample_inputs.data()
        + tick.invocation.sequential_sample_begin;
    for (auto const slot : tick.sample_timeline_slots) {
        auto const input_index = tick.sample_inputs[slot];
        auto const& input = inputs[input_index];
        auto const& source = supplied[slot];
        // The timeline and the realtime operand are addressed by the same
        // generation pace cursor as the callback binding; slot frame zero is
        // the root call's first position.
        auto const pace = input.absolute_pace_exponent;
        auto const timeline_begin = pace_begin_index(frame, pace, callback_index);
        auto const supplied_begin = source.sequential_begin_index
            + projected_prefix(frame.root_index, callback_index, pace)
            - source.read_latency;
        auto const count = project_lattice(callback_index, callback_size, pace)
            .value_count;
        auto const value_size = input.geometry.value_size_bytes;
        for (std::size_t channel = 0;
             channel < channel_count(input.channel_layout);
             ++channel) {
            auto const& timeline = input.channels[channel];
            auto const& supplied_channel = source.storage.channels[channel];
            for (std::size_t value = 0; value < count; ++value) {
                auto* destination = continuous_value(
                    frame,
                    timeline.storage,
                    timeline_begin + value - timeline.frame_delay,
                    timeline.representation_channel);
                auto const* supplied_value = bound_channel_value(
                    supplied_channel, supplied_begin + value);
                if (!input.combines_invocation_supplied) {
                    std::memcpy(destination, supplied_value, value_size);
                    continue;
                }
                auto const& operand = input.realtime_operand_channels[channel];
                std::memcpy(
                    destination,
                    continuous_value(
                        frame,
                        operand.storage,
                        timeline_begin + value - operand.frame_delay,
                        operand.representation_channel),
                    value_size);
                auto const accepted = callbacks.combine_sequential_input_values(
                    input_index,
                    input.geometry.value_type,
                    destination,
                    supplied_value,
                    value_size,
                    input.geometry.elements_per_value);
                IV_ASSERT(accepted,
                    "GraphJit Sequential sample combination was rejected");
            }
        }
    }
}

// Inserts the current window of every slot-fed event input that owns a
// timeline ring. A mixed input inserts the current window of the deterministic
// merge of its realtime operand with the slot window. The ring was pruned and
// rebased before this primitive, so earlier events are the input's node-owned
// history.
void stage_event_timelines(
    RootFrame& frame,
    std::size_t primitive_index,
    RootTickSlots const& tick,
    GlobalIndex callback_index,
    std::size_t callback_size) noexcept
{
    auto const& plan = frame.plan;
    auto const& inputs = plan.event_ports.primitives[primitive_index].inputs;
    auto const* supplied = frame.invocation->sequential_event_inputs.data()
        + tick.invocation.sequential_event_begin;
    for (auto const slot : tick.event_timeline_slots) {
        auto const& input = inputs[tick.event_inputs[slot]];
        auto const& source_binding = supplied[slot];
        auto const& source = source_binding.storage;
        auto const* source_events = source.storage + source.events_offset;
        auto source_capacity = source.event_capacity;
        auto source_read = source.persistent_ring
            ? load_raw<std::size_t>(source.storage, source.read_index_offset)
            : 0;
        auto source_write = source.persistent_ring
            ? load_raw<std::size_t>(source.storage, source.write_index_offset)
            : load_raw<std::size_t>(source.storage, source.count_offset);

        if (input.combines_invocation_supplied) {
            auto const realtime_index = *input.realtime_operand_storage;
            auto const& realtime = plan.event_ports.ports[realtime_index];
            auto* realtime_base = frame.event_representations[realtime_index];
            auto const& combined = plan.event_ports.ports[*input.combined_storage];
            auto* combined_events = frame.event_representations[
                *input.combined_storage] + combined.events_relative_offset;
            source_write = detail::iv_graph_jit_merge_mixed_event_sequences(
                combined_events,
                combined.event_capacity,
                realtime_base + realtime.events_relative_offset,
                realtime.has_source_indices
                    ? reinterpret_cast<std::size_t const*>(
                        realtime_base + realtime.source_indices_relative_offset)
                    : nullptr,
                input.realtime_semantic_source_index.value_or(no_index),
                realtime.event_capacity,
                realtime.persistent_ring
                    ? load_raw<std::size_t>(
                        realtime_base, realtime.read_index_relative_offset)
                    : 0,
                load_raw<std::size_t>(
                    realtime_base,
                    realtime.persistent_ring
                        ? realtime.write_index_relative_offset
                        : realtime.count_relative_offset),
                source_events,
                source_binding.semantic_source_indices,
                no_index,
                source.event_capacity,
                source_read,
                source_write,
                callback_index);
            IV_ASSERT(source_write != no_index,
                "GraphJit mixed Sequential event merge overflowed its storage");
            source_events = combined_events;
            source_capacity = combined.event_capacity;
            source_read = 0;
        }

        auto const& target = plan.event_ports.ports[*input.storage];
        auto* target_base = frame.event_representations[*input.storage];
        auto const written = detail::iv_graph_jit_merge_staged_event_window(
            target_base + target.events_relative_offset,
            target.event_capacity,
            load_raw<std::size_t>(target_base, target.read_index_relative_offset),
            load_raw<std::size_t>(target_base, target.write_index_relative_offset),
            source_events,
            source_capacity,
            source_read,
            source_write,
            callback_index,
            callback_size,
            input.timeline_history_samples);
        IV_ASSERT(written != no_index,
            "GraphJit Sequential event staging overflowed its timeline");
        store_raw(target_base, target.write_index_relative_offset, written);
    }
}

// Adds every participating manual control of a primitive to its input
// timeline. The destination is always target-owned storage: an ordinary
// producer alias is copied first, and the node's Sequential combine operation
// then receives the scalar manual contribution.
void stage_manual_control_sample_timelines(
    RootFrame& frame,
    std::size_t primitive_index,
    GlobalIndex slice_index,
    std::size_t slice_size,
    GlobalIndex callback_index,
    std::size_t callback_size) noexcept
{
    auto const& plan = frame.plan;
    auto const& primitive = plan.primitives[primitive_index];
    auto const& inputs = plan.sample_ports.primitives[primitive_index].inputs;
    for (auto const control_index : primitive.manual_controls) {
        auto const& control = plan.sample_ports.manual_controls[control_index];
        auto const& input = inputs[control.input_index];
        auto* state = reinterpret_cast<ManualControlRuntimeState*>(
            frame.storage_base + control.state_storage_offset);
        auto const settled_bits = std::bit_cast<std::uint32_t>(
            control.configured.settled_value.value);
        if (!control.configured.dynamic || !control.stages_timeline) {
            // Constant code owns the effective interpolation origin but never
            // overwrites target_bits, which may already hold the first
            // interaction value awaiting dynamic-code activation.
            state->effective_bits = settled_bits;
        }
        if (!control.stages_timeline) continue;

        auto const pace = input.absolute_pace_exponent;
        auto const values = project_lattice(slice_index, slice_size, pace).value_count;
        if (values == 0) continue;
        auto const target_bits = control.configured.dynamic
            ? std::atomic_ref<std::uint32_t>{state->target_bits}.load(
                  std::memory_order_relaxed)
            : settled_bits;
        auto const effective_bits = control.configured.dynamic
            ? state->effective_bits
            : settled_bits;
        auto const target_value = std::bit_cast<float>(target_bits);
        auto const effective_value = std::bit_cast<float>(effective_bits);
        auto const callback_values = project_lattice(
            callback_index, callback_size, pace).value_count;
        auto const callback_prefix = projected_prefix(callback_index, slice_index, pace);
        auto const timeline_begin = pace_begin_index(frame, pace, slice_index);
        auto const has_existing_value = input.invocation_supplied
            || !input.manual_operand_channels.empty();
        for (std::size_t channel = 0;
             channel < channel_count(input.channel_layout);
             ++channel) {
            auto const& target = input.manual_timeline_channels[channel];
            for (std::size_t value_index = 0; value_index < values; ++value_index) {
                auto* destination = continuous_value(
                    frame,
                    target.storage,
                    timeline_begin + value_index - target.frame_delay,
                    target.representation_channel);
                if (!input.manual_operand_channels.empty()) {
                    auto const& operand = input.manual_operand_channels[channel];
                    std::memcpy(
                        destination,
                        continuous_value(
                            frame,
                            operand.storage,
                            timeline_begin + value_index - operand.frame_delay,
                            operand.representation_channel),
                        sizeof(Sample));
                }
                auto value = target_value;
                if (control.configured.dynamic) {
                    auto const ratio
                        = static_cast<float>(callback_prefix + value_index + 1)
                        / static_cast<float>(callback_values);
                    value = effective_value + (target_value - effective_value) * ratio;
                }
                Sample const manual{value};
                if (has_existing_value) {
                    auto const accepted = primitive.callbacks
                        .combine_sequential_input_values(
                            control.input_index,
                            ContinuousValueTypeId::sample,
                            destination,
                            reinterpret_cast<std::byte const*>(&manual),
                            sizeof(manual),
                            1);
                    IV_ASSERT(accepted,
                        "GraphJit manual input combination was rejected");
                } else {
                    std::memcpy(destination, &manual, sizeof(manual));
                }
            }
        }
        if (callback_prefix + values >= callback_values) {
            state->effective_bits = target_bits;
        } else {
            state->effective_bits = effective_bits;
        }
    }
}

template<class T>
ReflectedSpan<T const> invocation_span(
    ReflectedSpan<T const> supplied,
    std::size_t begin,
    std::size_t count) noexcept
{
    if (supplied.data() == nullptr || supplied.size() < begin + count) return {};
    return {supplied.data() + begin, count};
}

// Completes, once per root call, the parts of each Tick primitive's bindings
// and context that its TickInvocationCall supplies: the bindings of inputs a
// slot replaces, and the Random Access views.
void initialize_tick_slots(RootFrame& frame) noexcept
{
    auto const& plan = frame.plan;
    for (auto const primitive_index : plan.tick_primitives) {
        auto const& primitive = plan.primitives[primitive_index];
        auto const& tick = *primitive.tick;
        auto const& invocation = tick.invocation;
        auto sample_inputs = frame_array<ReflectedContinuousInputPortBinding>(
            frame.scratch,
            primitive.sample_inputs_offset,
            plan.sample_ports.primitives[primitive_index].inputs.size());
        for (auto const slot : tick.sample_overlay_slots) {
            auto const& supplied = frame.invocation->sequential_sample_inputs
                .data()[invocation.sequential_sample_begin + slot];
            // The slot replaces the binding except for its compiler-owned
            // contiguous scratch.
            auto& binding = sample_inputs[tick.sample_inputs[slot]];
            auto const capacity = binding.contiguous_value_capacity;
            std::array<std::byte*, maximum_supported_channel_count> scratch{};
            for (std::size_t channel = 0; channel < scratch.size(); ++channel) {
                scratch[channel] = binding.storage.channels[channel].contiguous_scratch;
            }
            binding = supplied;
            binding.contiguous_value_capacity = capacity;
            for (std::size_t channel = 0; channel < scratch.size(); ++channel) {
                binding.storage.channels[channel].contiguous_scratch = scratch[channel];
            }
        }
        auto event_inputs = frame_array<ReflectedEventInputPortBinding>(
            frame.scratch,
            primitive.event_inputs_offset,
            plan.event_ports.primitives[primitive_index].inputs.size());
        for (auto const slot : tick.event_overlay_slots) {
            event_inputs[tick.event_inputs[slot]] = frame.invocation
                ->sequential_event_inputs.data()[invocation.sequential_event_begin + slot];
        }
        auto& context = *reinterpret_cast<ReflectedNodeTickContext*>(
            frame.scratch + primitive.context_offset);
        if (invocation.random_access_sample_count != 0) {
            context.random_access_inputs = invocation_span(
                frame.invocation->random_access_sample_inputs,
                invocation.random_access_sample_begin,
                invocation.random_access_sample_count);
        }
        if (invocation.random_access_event_count != 0) {
            context.random_access_event_inputs = invocation_span(
                frame.invocation->random_access_event_inputs,
                invocation.random_access_event_begin,
                invocation.random_access_event_count);
        }
    }
}

// Writes what one invocation of a primitive changes: the windows of its
// timeline-owning slot inputs and staged manual controls, and the Sequential
// begin index of each binding. Everything else in its context is complete for
// the root call.
ReflectedNodeTickContext const& prepare_invocation(
    RootFrame& frame,
    std::size_t primitive_index,
    GlobalIndex callback_index,
    std::size_t callback_size,
    GlobalIndex enclosing_index,
    std::size_t enclosing_size) noexcept
{
    auto const& primitive = frame.plan.primitives[primitive_index];
    if (primitive.tick) {
        if (!primitive.tick->sample_timeline_slots.empty()) {
            stage_sample_timelines(
                frame, primitive_index, *primitive.tick, callback_index, callback_size);
        }
        if (!primitive.tick->event_timeline_slots.empty()) {
            stage_event_timelines(
                frame, primitive_index, *primitive.tick, callback_index, callback_size);
        }
    }
    if (!primitive.manual_controls.empty()) {
        stage_manual_control_sample_timelines(
            frame,
            primitive_index,
            callback_index,
            callback_size,
            enclosing_index,
            enclosing_size);
    }
    for (auto const& update : primitive.begin_indices) {
        // A directly bound slot holds the root call's window from frame zero,
        // so a slice begins at its projected offset.
        auto const prefix = projected_prefix(
            frame.root_index, callback_index, update.pace_exponent);
        store_raw<std::uint64_t>(
            frame.scratch,
            update.field_offset,
            update.cursor == no_index
                ? prefix
                : frame.sequential_pace_cursors[update.cursor] + prefix);
    }
    return *reinterpret_cast<ReflectedNodeTickContext const*>(
        frame.scratch + primitive.context_offset);
}

PrimitiveActivityDecision decide_primitive_activity(
    RootFrame& frame,
    std::size_t primitive_index,
    ReflectedNodeTickContext const& context,
    GlobalIndex index,
    std::size_t block_size) noexcept
{
    auto const& plan = frame.plan;
    auto const& selected = plan.primitive_activity[primitive_index];
    if (!selected) return {};
    auto const& activity = *selected;
    auto* state = reinterpret_cast<detail::PrimitiveActivityState*>(
        frame.storage_base + activity.storage_offset);
    auto inputs_constant = true;
    auto inputs_unchanged = true;
    auto const& sample_plan = plan.sample_ports.primitives[primitive_index];
    for (std::size_t input = 0; input < activity.continuous_inputs.size(); ++input) {
        auto const& input_activity = activity.continuous_inputs[input];
        if (input_activity.changed_during_transition) {
            inputs_constant = false;
            inputs_unchanged = false;
            continue;
        }
        if (input_activity.static_constant) continue;
        auto const value_count = project_lattice(
            index,
            block_size,
            sample_plan.inputs[input].absolute_pace_exponent).value_count;
        for (std::size_t channel = 0;
             channel < input_activity.channels.size();
             ++channel) {
            auto const frontier_index = input_activity.channels[channel].frontier;
            auto const& frontier = plan.activity_frontiers[frontier_index];
            // A frontier without a realtime representation has no realtime
            // writer that could invalidate its summary.
            auto const epoch = frontier.representation
                    == detail::no_sample_representation
                ? std::size_t{0}
                : frame.sample_representation_epochs[frontier.representation];
            auto& summary = frame.continuous_activity_summaries[frontier_index];
            if (!summary.valid
                || summary.representation_epoch != epoch
                || summary.index.bits() != index.bits()
                || summary.block_size != block_size) {
                summary = {
                    .valid = true,
                    .representation_epoch = epoch,
                    .index = index,
                    .block_size = block_size,
                    .flags = detail::iv_graph_jit_continuous_input_activity(
                        &context.continuous_input_bindings.data()[input],
                        channel,
                        value_count,
                        reinterpret_cast<std::uint8_t*>(
                            frame.storage_base
                            + frontier.remembered_valid_storage_offset),
                        frame.storage_base
                            + frontier.remembered_value_storage_offset),
                };
            }
            inputs_constant = inputs_constant
                && (summary.flags & detail::continuous_input_constant) != 0;
            inputs_unchanged = inputs_unchanged
                && (summary.flags & detail::continuous_input_unchanged) != 0;
        }
    }

    auto const& primitive = plan.primitives[primitive_index];
    auto const& event_plan = plan.event_ports.primitives[primitive_index];
    for (std::size_t input = 0; input < event_plan.inputs.size(); ++input) {
        auto const* binding = &context.event_input_bindings.data()[input];
        auto const storage = event_plan.inputs[input].storage;
        bool empty = false;
        if (!primitive.slot_supplied_event_inputs[input] && storage) {
            auto const epoch = frame.event_representation_epochs[*storage];
            auto& summary = frame.event_activity_summaries[*storage];
            if (!summary.valid
                || summary.representation_epoch != epoch
                || summary.index.bits() != index.bits()
                || summary.block_size != block_size) {
                summary = {
                    .valid = true,
                    .representation_epoch = epoch,
                    .index = index,
                    .block_size = block_size,
                    .empty = detail::iv_graph_jit_event_input_is_empty(
                        binding, index, block_size),
                };
            }
            empty = summary.empty;
        } else {
            empty = detail::iv_graph_jit_event_input_is_empty(
                binding, index, block_size);
        }
        inputs_constant = inputs_constant && empty;
        inputs_unchanged = inputs_unchanged && empty;
    }
    return {
        .plan = &activity,
        .state = state,
        .inputs_constant = inputs_constant,
        .inputs_unchanged = inputs_unchanged,
        .should_skip = state->dormant != 0 && inputs_unchanged,
    };
}

void update_primitive_activity(
    RootFrame const& frame,
    std::size_t primitive_index,
    PrimitiveActivityDecision const& decision,
    ReflectedNodeTickContext const& context,
    GlobalIndex index,
    std::size_t block_size) noexcept
{
    if (!decision.plan) return;
    auto const& sample_plan = frame.plan.sample_ports.primitives[primitive_index];
    auto outputs_clear = !context.continuous_output_bindings.empty()
        || !context.event_output_bindings.empty();
    std::size_t compact = 0;
    for (auto const& output : sample_plan.outputs) {
        if (!output.realtime) continue;
        auto const value_count = project_lattice(
            index, block_size, output.absolute_pace_exponent).value_count;
        auto const channels = decision.plan->continuous_output_channel_counts[compact];
        for (std::size_t channel = 0; channel < channels; ++channel) {
            outputs_clear = outputs_clear
                && detail::iv_graph_jit_continuous_output_is_clear(
                    &context.continuous_output_bindings.data()[compact],
                    channel,
                    value_count);
        }
        ++compact;
    }
    for (std::size_t output = 0;
         output < context.event_output_bindings.size();
         ++output) {
        outputs_clear = outputs_clear
            && detail::iv_graph_jit_event_output_is_empty(
                &context.event_output_bindings.data()[output], index, block_size);
    }
    // A changed input starts a new quiet interval. In particular, a dormant
    // primitive must not reuse the interval that preceded its wake.
    auto const quiescent = decision.inputs_constant && outputs_clear;
    auto const base = decision.inputs_unchanged
        ? decision.state->silent_samples
        : std::size_t{0};
    auto const next = base > std::numeric_limits<std::size_t>::max() - block_size
        ? std::numeric_limits<std::size_t>::max()
        : base + block_size;
    decision.state->silent_samples = quiescent ? next : 0;
    decision.state->dormant = static_cast<std::uint8_t>(
        quiescent && decision.state->silent_samples >= decision.plan->ttl_samples);
}

void mark_primitive_outputs(RootFrame& frame, RootPrimitive const& primitive) noexcept
{
    if (!frame.plan.tracks_activity) return;
    for (auto const representation : primitive.output_sample_representations) {
        ++frame.sample_representation_epochs[representation];
    }
    for (auto const representation : primitive.output_event_representations) {
        ++frame.event_representation_epochs[representation];
    }
}

void invoke_primitive(
    RootFrame& frame,
    std::size_t primitive_index,
    GlobalIndex callback_index,
    std::size_t callback_size,
    GlobalIndex enclosing_index,
    std::size_t enclosing_size) noexcept
{
    auto const& primitive = frame.plan.primitives[primitive_index];
    auto const& context = prepare_invocation(
        frame,
        primitive_index,
        callback_index,
        callback_size,
        enclosing_index,
        enclosing_size);
    if (!frame.plan.tracks_activity) {
        primitive.callbacks.tick_block(
            primitive.node_data.data(), context, callback_index, callback_size);
        return;
    }
    auto const decision = decide_primitive_activity(
        frame, primitive_index, context, callback_index, callback_size);
    if (decision.should_skip) {
        primitive.callbacks.skip_block(
            primitive.node_data.data(), context, callback_index, callback_size);
    } else {
        if (decision.state) decision.state->dormant = 0;
        primitive.callbacks.tick_block(
            primitive.node_data.data(), context, callback_index, callback_size);
        update_primitive_activity(
            frame, primitive_index, decision, context, callback_index, callback_size);
    }
    mark_primitive_outputs(frame, primitive);
}

// Captures run after every operation of the producer step has finalized its
// authored output window: once per SCC slice, and once for a primitive that
// slices its own invocation.
void capture_outputs(
    RootFrame& frame,
    std::size_t primitive_index,
    GlobalIndex index,
    std::size_t block_size) noexcept
{
    auto const& primitive = frame.plan.primitives[primitive_index];
    auto const& tick = *primitive.tick;
    auto sample_outputs = frame_array<ReflectedContinuousOutputPortBinding>(
        frame.scratch, primitive.sample_outputs_offset, primitive.sample_output_count);
    auto event_outputs = frame_array<ReflectedEventOutputPortBinding>(
        frame.scratch, primitive.event_outputs_offset, primitive.event_output_count);
    auto const& sample_captures = frame.invocation->sample_captures;
    for (std::size_t offset = 0; offset < tick.sample_capture_outputs.size(); ++offset) {
        auto const slot = tick.invocation.sample_capture_begin + offset;
        if (sample_captures.data() == nullptr || slot >= sample_captures.size()) break;
        auto const& operation = sample_captures.data()[slot];
        if (!operation.context || !operation.capture) continue;
        operation.capture(
            operation.context,
            &sample_outputs[tick.sample_capture_outputs[offset]],
            index,
            block_size);
    }
    auto const& event_captures = frame.invocation->event_captures;
    for (std::size_t offset = 0; offset < tick.event_capture_outputs.size(); ++offset) {
        auto const slot = tick.invocation.event_capture_begin + offset;
        if (event_captures.data() == nullptr || slot >= event_captures.size()) break;
        auto const& operation = event_captures.data()[slot];
        if (!operation.context || !operation.capture) continue;
        operation.capture(
            operation.context,
            &event_outputs[tick.event_capture_outputs[offset]],
            index,
            block_size);
    }
}

void run_step(
    RootFrame& frame,
    std::size_t step_index,
    GlobalIndex index,
    std::size_t block_size,
    bool allow_primitive_slicing) noexcept
{
    auto const& step = frame.plan.steps[step_index];
    if (!step.before.empty()) run_operations(frame, step.before, index, block_size);
    if (allow_primitive_slicing && step.maximum_block_size < block_size) {
        for (std::size_t offset = 0; offset < block_size;
             offset += step.maximum_block_size) {
            invoke_primitive(
                frame,
                step.primitive,
                index.advanced_by(offset),
                std::min(step.maximum_block_size, block_size - offset),
                index,
                block_size);
        }
    } else {
        invoke_primitive(frame, step.primitive, index, block_size, index, block_size);
    }
    if (!step.after.empty()) run_operations(frame, step.after, index, block_size);
    if (step.captures) capture_outputs(frame, step.primitive, index, block_size);
}

// A multi-step acyclic ready group is an antichain whose lanes have no
// step-local operations or captures and need no slicing. Every lane decides
// Tick or skip before either batch runs, so no lane observes another lane's
// outputs and the decision matches scalar execution.
void run_batch(
    RootFrame& frame,
    detail::ExecutionReadyGroupPlan const& group,
    GlobalIndex index,
    std::size_t block_size) noexcept
{
    auto const& plan = frame.plan;
    if (!plan.tracks_activity) {
        details::NodeCompilerOperations const* callbacks = nullptr;
        std::size_t lane_count = 0;
        for (auto const step_index : group.primitive_steps) {
            auto const primitive_index = plan.steps[step_index].primitive;
            auto const& primitive = plan.primitives[primitive_index];
            if (!callbacks) callbacks = &primitive.callbacks;
            frame.tick_lanes[lane_count++] = ReflectedNodeTickBatchLane{
                .node_data = primitive.node_data.data(),
                .context = prepare_invocation(
                    frame, primitive_index, index, block_size, index, block_size),
                .index = index,
                .block_size = block_size,
            };
        }
        callbacks->tick_block_batch(ReflectedNodeTickBatchContext{
            .lanes = {frame.tick_lanes.data(), lane_count},
        });
        return;
    }
    std::size_t tick_count = 0;
    std::size_t skip_count = 0;
    details::NodeCompilerOperations const* callbacks = nullptr;
    for (auto const step_index : group.primitive_steps) {
        auto const primitive_index = plan.steps[step_index].primitive;
        auto const& primitive = plan.primitives[primitive_index];
        if (!callbacks) callbacks = &primitive.callbacks;
        auto const& context = prepare_invocation(
            frame, primitive_index, index, block_size, index, block_size);
        auto const decision = decide_primitive_activity(
            frame, primitive_index, context, index, block_size);
        auto const lane = ReflectedNodeTickBatchLane{
            .node_data = primitive.node_data.data(),
            .context = context,
            .index = index,
            .block_size = block_size,
        };
        if (decision.should_skip) {
            frame.skip_lanes[skip_count++] = lane;
            continue;
        }
        if (decision.state) decision.state->dormant = 0;
        frame.tick_lanes[tick_count] = lane;
        frame.tick_decisions[tick_count] = decision;
        frame.tick_lane_primitives[tick_count] = primitive_index;
        ++tick_count;
    }
    if (tick_count != 0) {
        callbacks->tick_block_batch(ReflectedNodeTickBatchContext{
            .lanes = {frame.tick_lanes.data(), tick_count},
        });
        for (std::size_t lane = 0; lane < tick_count; ++lane) {
            update_primitive_activity(
                frame,
                frame.tick_lane_primitives[lane],
                frame.tick_decisions[lane],
                frame.tick_lanes[lane].context,
                index,
                block_size);
        }
    }
    if (skip_count != 0) {
        callbacks->skip_block_batch(ReflectedNodeTickBatchContext{
            .lanes = {frame.skip_lanes.data(), skip_count},
        });
    }
    for (auto const step_index : group.primitive_steps) {
        mark_primitive_outputs(frame, plan.primitives[plan.steps[step_index].primitive]);
    }
}

// ---------------------------------------------------------------------------
// Plan construction
// ---------------------------------------------------------------------------

using Checked = std::expected<void, std::string>;

std::unexpected<std::string> invalid(std::string message)
{
    return std::unexpected(std::move(message));
}

// The generation Sequential cursor of one pace, or no_index.
std::size_t pace_cursor(RealtimeRootPlan const& plan, std::int32_t pace_exponent)
{
    auto const& cursors = plan.sample_ports.storage.sequential_pace_cursors;
    for (std::size_t cursor = 0; cursor < cursors.size(); ++cursor) {
        if (cursors[cursor].absolute_pace_exponent == pace_exponent) return cursor;
    }
    return no_index;
}

Checked require_pace(std::int32_t pace_exponent)
{
    if (pace_exponent >= static_cast<std::int32_t>(sizeof(std::size_t) * 8)
        || pace_exponent < -static_cast<std::int32_t>(sizeof(std::size_t) * 8)) {
        return invalid("GraphJit continuous pace is outside the Q64.64 lattice");
    }
    return {};
}

Checked require_pace_cursor(RealtimeRootPlan const& plan, std::int32_t pace_exponent)
{
    if (auto valid = require_pace(pace_exponent); !valid) return valid;
    if (pace_cursor(plan, pace_exponent) == no_index) {
        return invalid("GraphJit continuous storage has no cursor for its resolved pace");
    }
    return {};
}

// The realized storage of one representation, as a callback binding sees it.
std::expected<ReflectedContinuousPortStorageBinding, std::string>
sample_storage_binding(
    detail::SampleStoragePlan const& plan,
    std::size_t representation_index)
{
    if (representation_index >= plan.representations.size()) {
        return invalid("GraphJit sample port references missing storage");
    }
    auto const& storage = plan.representations[representation_index];
    if (storage.constant_value) {
        if (storage.transient_allocation != detail::no_sample_transient_allocation
            || storage.persistent_allocation
                != detail::no_sample_persistent_allocation) {
            return invalid(
                "GraphJit constant sample representation unexpectedly owns writable storage");
        }
    } else if (storage.transient_allocation
               != detail::no_sample_transient_allocation) {
        if (storage.transient_allocation >= plan.transient_allocations.size()) {
            return invalid(
                "GraphJit sample storage references a missing transient allocation");
        }
        if (plan.transient_allocations[storage.transient_allocation]
                .representation_index
            != representation_index) {
            return invalid(
                "GraphJit sample transient allocation points at the wrong storage");
        }
    } else if (storage.persistent_allocation
               != detail::no_sample_persistent_allocation) {
        if (storage.persistent_allocation >= plan.persistent_allocations.size()) {
            return invalid(
                "GraphJit sample storage references a missing persistent allocation");
        }
        auto const& allocation =
            plan.persistent_allocations[storage.persistent_allocation];
        if (allocation.representation_index != representation_index) {
            return invalid(
                "GraphJit sample persistent allocation points at the wrong storage");
        }
        if (allocation.kind != detail::SamplePersistentStorageKind::ring) {
            return invalid(
                "GraphJit compact sample carry must bind through its transient working storage");
        }
    } else {
        return invalid("GraphJit sample storage has no realized storage");
    }

    ReflectedContinuousPortStorageBinding binding{
        .geometry = storage.geometry,
        .value_capacity = storage.frame_capacity,
        .channel_layout = storage.channel_layout,
    };
    auto const channels = channel_count(storage.channel_layout);
    for (std::size_t channel = 0; channel < channels; ++channel) {
        binding.channels[channel].value_capacity = storage.frame_capacity;
        binding.channels[channel].value_stride_bytes =
            storage.channel_layout.sample_layout == SampleStreamLayout::planar
            ? storage.geometry.value_size_bytes
            : channels * storage.geometry.value_size_bytes;
    }
    return binding;
}

// A representation that root operations address by masked value index.
std::expected<ReflectedContinuousPortStorageBinding, std::string>
ring_storage_binding(
    detail::SampleStoragePlan const& plan,
    std::size_t representation_index,
    char const* message)
{
    auto binding = sample_storage_binding(plan, representation_index);
    if (!binding) return binding;
    if (binding->value_capacity == 0 || !std::has_single_bit(binding->value_capacity)) {
        return invalid(message);
    }
    return binding;
}

std::expected<ReflectedContinuousPortStorageBinding, std::string>
sample_input_storage_binding(
    detail::SampleStoragePlan const& storage,
    detail::PrimitiveSampleInputBindingPlan const& input)
{
    auto const target_channels = channel_count(input.channel_layout);
    if (input.channels.size() != target_channels || target_channels == 0) {
        return invalid("GraphJit sample input storage has an invalid channel count");
    }
    ReflectedContinuousPortStorageBinding binding{
        .channel_layout = input.channel_layout,
    };
    for (std::size_t target_channel = 0;
         target_channel < target_channels;
         ++target_channel) {
        auto const& channel = input.channels[target_channel];
        if (channel.storage == detail::no_sample_representation) {
            return invalid("GraphJit sample input channel has no storage");
        }
        auto source = sample_storage_binding(storage, channel.storage);
        if (!source) return std::unexpected(std::move(source.error()));
        if (channel.representation_channel >= channel_count(source->channel_layout)) {
            return invalid("GraphJit sample input channel is outside its storage");
        }
        binding.channels[target_channel] =
            source->channels[channel.representation_channel];
        binding.channels[target_channel].value_delay = channel.frame_delay;
        if (target_channel == 0) {
            binding.geometry = source->geometry;
            binding.value_capacity = binding.channels[target_channel].value_capacity;
        } else if (binding.geometry != source->geometry) {
            return invalid(
                "GraphJit continuous input channels disagree on value geometry");
        }
    }
    return binding;
}

// One channel of a planned representation addressed by masked value index,
// as timeline staging and manual-control staging write it.
Checked validate_timeline_channel(
    RealtimeRootPlan const& plan,
    detail::PrimitiveSampleInputChannelBindingPlan const& channel)
{
    auto binding = ring_storage_binding(
        plan.sample_ports.storage,
        channel.storage,
        "GraphJit sample timeline requires bounded power-of-two storage");
    if (!binding) return std::unexpected(std::move(binding.error()));
    if (channel.representation_channel >= channel_count(binding->channel_layout)) {
        return invalid("GraphJit sample timeline channel is outside its storage");
    }
    return {};
}

Checked conversion_registered(ChannelLayout source, ChannelLayout target)
{
    try {
        (void)ChannelConversionRegistry::plan(source, target);
    } catch (std::exception const& error) {
        return invalid(
            "GraphJit sample conversion is unsupported: " + std::string(error.what()));
    }
    return {};
}

std::expected<ChannelMix, std::string> channel_mix(
    ChannelLayout source_layout,
    ChannelLayout converted_layout,
    detail::SampleChannelTransformPlan const& transform,
    std::size_t source_values)
{
    auto const source = source_layout.channel_type;
    auto const target = converted_layout.channel_type;
    auto const source_count = transform.empty()
        ? channel_count(source)
        : transform.source_channel_count;
    auto const target_count = transform.empty()
        ? channel_count(target)
        : transform.target_channel_count;
    if (!is_registered_channel_type(source)
        || !is_registered_channel_type(target)
        || source_values != source_count
        || source_count > maximum_supported_channel_count
        || target_count > maximum_supported_channel_count
        || (!transform.empty()
            && transform.coefficients.size() != source_count * target_count)) {
        return invalid(
            "GraphJit channel conversion has an invalid registered type or source value count");
    }
    if (transform.empty()) {
        if (auto supported = conversion_registered(source_layout, converted_layout);
            !supported) {
            return std::unexpected(std::move(supported.error()));
        }
    }
    ChannelMix mix;
    mix.term_begins.reserve(target_count + 1);
    for (std::size_t target_channel = 0; target_channel < target_count; ++target_channel) {
        mix.term_begins.push_back(mix.terms.size());
        for (std::size_t source_channel = 0; source_channel < source_count; ++source_channel) {
            auto const coefficient = transform.empty()
                ? channel_conversion_coefficient(
                    source, target, source_channel, target_channel)
                : transform.coefficients[target_channel * source_count + source_channel];
            if (coefficient == 0.0f) continue;
            mix.terms.push_back({
                .source_channel = source_channel,
                .coefficient = coefficient,
            });
        }
    }
    mix.term_begins.push_back(mix.terms.size());
    return mix;
}

class RootPlanBuilder {
    LoweringInput const& input_;
    detail::LoweringPlan const& lowering_;
    RealtimeRootPlan& plan_;
    std::vector<std::uint8_t> materialization_ready_{};
    std::vector<std::uint8_t> composition_ready_{};
    std::vector<std::uint8_t> feedback_ready_{};
    std::vector<std::uint8_t> sample_carry_ready_{};
    std::vector<std::uint8_t> primitive_ready_{};

public:
    RootPlanBuilder(
        LoweringInput const& input,
        detail::LoweringPlan const& lowering,
        RealtimeRootPlan& plan)
        : input_(input)
        , lowering_(lowering)
        , plan_(plan)
    {}

    Checked build()
    {
        auto const& storage = plan_.sample_ports.storage;
        plan_.materializations.resize(storage.materializations.size());
        plan_.compositions.resize(storage.compositions.size());
        plan_.feedback_writes.resize(storage.feedback_timelines.size());
        materialization_ready_.resize(storage.materializations.size());
        composition_ready_.resize(storage.compositions.size());
        feedback_ready_.resize(storage.feedback_timelines.size());
        sample_carry_ready_.resize(storage.carry_operations.size());
        primitive_ready_.resize(plan_.sample_ports.primitives.size());

        if (auto valid = build_addresses(); !valid) return valid;
        if (auto valid = build_authored_extents(); !valid) return valid;
        if (auto valid = build_primitives(); !valid) return valid;
        if (auto valid = build_steps(); !valid) return valid;
        if (auto valid = build_groups(); !valid) return valid;
        return build_layout();
    }

private:
    // --- storage ----------------------------------------------------------

    // The scratch offset of one region of a root-stack arena.
    std::expected<std::size_t, std::string> arena_offset(
        std::size_t arena_begin,
        std::size_t arena_size,
        std::size_t region_offset,
        std::size_t region_size,
        char const* message) const
    {
        auto const& stack = plan_.root_stack;
        if (arena_size == 0
            || arena_begin > stack.size_bytes
            || arena_size > stack.size_bytes - arena_begin
            || region_offset > arena_size
            || region_size > arena_size - region_offset) {
            return invalid(message);
        }
        return arena_begin + region_offset;
    }

    Checked build_addresses()
    {
        auto const& stack = plan_.root_stack;
        if (stack.alignment == 0 || !std::has_single_bit(stack.alignment)) {
            return invalid("GraphJit realtime root stack alignment is not a power of two");
        }
        auto const& storage = plan_.sample_ports.storage;
        plan_.sample_addresses.resize(storage.representations.size());
        plan_.representation_cursors.resize(storage.representations.size());
        for (std::size_t index = 0; index < storage.representations.size(); ++index) {
            auto const& representation = storage.representations[index];
            // An operation on a representation without a cursor is rejected
            // by require_pace_cursor() when the operation is prepared.
            plan_.representation_cursors[index] =
                pace_cursor(plan_, representation.absolute_pace_exponent);
            auto& address = plan_.sample_addresses[index];
            if (representation.constant_value) {
                auto const channels = channel_count(representation.channel_layout);
                auto const value_size = representation.geometry.value_size_bytes;
                if (channels == 0
                    || representation.frame_capacity
                        > std::numeric_limits<std::size_t>::max() / channels
                    || representation.constant_value->size() != value_size
                    || channels * representation.frame_capacity
                        > std::numeric_limits<std::size_t>::max() / value_size) {
                    return invalid(
                        "GraphJit constant continuous representation has invalid storage geometry");
                }
                auto const values = channels * representation.frame_capacity;
                AlignedBytes bytes(
                    values * value_size, representation.geometry.element_alignment);
                for (std::size_t value = 0; value < values; ++value) {
                    std::memcpy(
                        bytes.data() + value * value_size,
                        representation.constant_value->data(),
                        value_size);
                }
                address = {
                    .base = RootAddressBase::immutable,
                    .immutable = bytes.data(),
                };
                plan_.immutable_storage.push_back(std::move(bytes));
            } else if (representation.transient_allocation
                       != detail::no_sample_transient_allocation) {
                if (representation.transient_allocation
                    >= storage.transient_allocations.size()) {
                    return invalid(
                        "GraphJit sample storage lost its transient root storage");
                }
                auto const& allocation = storage.transient_allocations[
                    representation.transient_allocation];
                auto offset = arena_offset(
                    stack.sample_offset,
                    storage.transient_arena_size,
                    allocation.region_relative_offset,
                    allocation.size_bytes,
                    "GraphJit sample storage lost its transient root storage");
                if (!offset) return std::unexpected(std::move(offset.error()));
                address = {.base = RootAddressBase::scratch, .offset = *offset};
            } else if (representation.persistent_allocation
                       != detail::no_sample_persistent_allocation) {
                if (representation.persistent_allocation
                    >= storage.persistent_allocations.size()) {
                    return invalid("GraphJit sample storage lost its persistent storage");
                }
                address = {
                    .base = RootAddressBase::node_storage,
                    .offset = storage.persistent_allocations[
                        representation.persistent_allocation].storage_offset,
                };
            } else {
                return invalid("GraphJit sample storage has no resolved storage");
            }
        }

        auto const& events = plan_.event_ports;
        plan_.event_addresses.resize(events.ports.size());
        for (std::size_t index = 0; index < events.ports.size(); ++index) {
            auto const& representation = events.ports[index];
            auto& address = plan_.event_addresses[index];
            if (representation.immutable_empty) {
                AlignedBytes bytes(representation.size_bytes, representation.alignment);
                if (bytes.size() != 0) std::memset(bytes.data(), 0, bytes.size());
                address = {
                    .base = RootAddressBase::immutable,
                    .immutable = bytes.data(),
                };
                plan_.immutable_storage.push_back(std::move(bytes));
            } else if (representation.persistent) {
                address = {
                    .base = RootAddressBase::node_storage,
                    .offset = representation.storage_offset,
                };
            } else {
                if (representation.transient_allocation
                    >= events.transient_allocations.size()) {
                    return invalid(
                        "GraphJit event storage lost its transient root storage");
                }
                auto const& allocation =
                    events.transient_allocations[representation.transient_allocation];
                auto offset = arena_offset(
                    stack.event_offset,
                    events.transient_arena_size,
                    allocation.region_relative_offset,
                    allocation.size_bytes,
                    "GraphJit event storage lost its transient root storage");
                if (!offset) return std::unexpected(std::move(offset.error()));
                address = {.base = RootAddressBase::scratch, .offset = *offset};
            }
        }
        return {};
    }

    Checked build_authored_extents()
    {
        auto const& storage = plan_.sample_ports.storage;
        plan_.authored_extents.assign(storage.representations.size(), no_index);
        for (std::size_t index = storage.producer_authored_extents.size(); index-- > 0;) {
            auto const& extent = storage.producer_authored_extents[index];
            if (extent.source_representation >= storage.representations.size()) {
                return invalid(
                    "GraphJit producer authored extent references a missing storage");
            }
            // The first extent of a representation is the one the root reads.
            plan_.authored_extents[extent.source_representation] = index;
        }
        return {};
    }

    Checked require_revision(std::size_t source_representation, std::size_t requested)
    {
        if (requested == 0) return {};
        auto const extent = plan_.authored_extents[source_representation];
        if (extent == no_index) {
            return invalid(
                "GraphJit detached continuous revision has no producer authored extent");
        }
        auto const& planned = plan_.sample_ports.storage.producer_authored_extents[extent];
        if (requested > planned.maximum_revision_frames) {
            return invalid(
                "GraphJit detached continuous revision exceeds its producer authored extent");
        }
        return {};
    }

    // --- sample operations ------------------------------------------------

    Checked prepare_materialization(std::size_t index)
    {
        auto const& storage = plan_.sample_ports.storage;
        if (index >= storage.materializations.size()) {
            return invalid("GraphJit execution plan references a missing sample materialization");
        }
        if (materialization_ready_[index]) return {};
        auto const& operation = storage.materializations[index];
        if (operation.source_representation >= storage.representations.size()
            || operation.target_representation >= storage.representations.size()) {
            return invalid("GraphJit sample materialization references a missing storage");
        }
        auto const& source = storage.representations[operation.source_representation];
        auto const& target = storage.representations[operation.target_representation];
        if (source.channel_layout != operation.source_layout
            || target.channel_layout != operation.target_layout
            || source.geometry != target.geometry
            || !supports_linear_audio_channel_conversion(source.geometry)) {
            return invalid(
                "GraphJit continuous materialization has incompatible layout or value geometry");
        }
        constexpr auto capacity_message =
            "GraphJit sample materialization requires bounded power-of-two frame capacities";
        if (auto binding = ring_storage_binding(
                storage, operation.source_representation, capacity_message);
            !binding) {
            return std::unexpected(std::move(binding.error()));
        }
        if (auto binding = ring_storage_binding(
                storage, operation.target_representation, capacity_message);
            !binding) {
            return std::unexpected(std::move(binding.error()));
        }
        if (operation.latest_read_latency > operation.retained_before) {
            return invalid("GraphJit sample materialization has an invalid consumer window");
        }
        if (auto valid = require_pace_cursor(plan_, target.absolute_pace_exponent); !valid) {
            return valid;
        }
        if (auto supported = conversion_registered(
                operation.source_layout, operation.target_layout);
            !supported) {
            return supported;
        }
        auto mix = channel_mix(
            operation.source_layout,
            operation.target_layout,
            operation.transform,
            channel_count(operation.source_layout));
        if (!mix) return std::unexpected(std::move(mix.error()));
        if (mix->converted_channel_count() != channel_count(operation.target_layout)) {
            return invalid(
                "GraphJit continuous materialization conversion produced an invalid channel count");
        }
        auto& derived = plan_.materializations[index];
        derived.mix = std::move(*mix);
        for (std::size_t channel = 0;
             channel < derived.mix.converted_channel_count();
             ++channel) {
            if (operation.transition_channels.empty()
                || std::ranges::contains(operation.transition_channels, channel)) {
                derived.written_channels |= std::uint64_t{1} << channel;
            }
        }
        materialization_ready_[index] = 1;
        return {};
    }

    // One composition write, feed-forward or shifted into a feedback timeline.
    std::expected<std::vector<RootContribution>, std::string> composition_write(
        std::span<detail::SampleCompositionContributionPlan const> contributions,
        std::size_t target_representation,
        ChannelLayout target_layout,
        std::size_t target_history,
        bool shift_writes_by_read_latency)
    {
        auto const& storage = plan_.sample_ports.storage;
        if (target_representation >= storage.representations.size()) {
            return invalid("GraphJit sample composition references a missing target storage");
        }
        auto const& target = storage.representations[target_representation];
        if (target.channel_layout != target_layout
            || !supports_linear_audio_channel_conversion(target.geometry)) {
            return invalid(
                "GraphJit continuous composition target has incompatible layout or value geometry");
        }
        if (contributions.empty()) {
            return invalid("GraphJit sample composition contains no semantic contributions");
        }
        if (contributions.size() > maximum_supported_channel_count) {
            return invalid(
                "GraphJit sample composition contains an invalid target projection");
        }
        if (shift_writes_by_read_latency
            && (target.storage == RealtimeBufferStorageKind::transient_stack
                || target.persistent_allocation
                    == detail::no_sample_persistent_allocation)) {
            return invalid(
                "GraphJit shifted sample composition requires retained feedback storage");
        }
        auto target_binding = ring_storage_binding(
            storage,
            target_representation,
            "GraphJit sample composition requires bounded power-of-two target storage");
        if (!target_binding) return std::unexpected(std::move(target_binding.error()));
        if (auto valid = require_pace_cursor(plan_, target.absolute_pace_exponent); !valid) {
            return std::unexpected(std::move(valid.error()));
        }

        std::vector<RootContribution> result;
        result.reserve(contributions.size());
        std::vector<bool> populated(channel_count(target_layout), false);
        for (auto const& contribution : contributions) {
            auto mix = channel_mix(
                contribution.source_layout,
                contribution.converted_layout,
                contribution.transform,
                contribution.sources.size());
            if (!mix) return std::unexpected(std::move(mix.error()));
            if (contribution.target_channels.size() != mix->converted_channel_count()) {
                return invalid(
                    "GraphJit sample composition contribution has inconsistent semantic channel counts");
            }
            for (auto const& source : contribution.sources) {
                auto binding = sample_storage_binding(storage, source.source_representation);
                if (!binding) return std::unexpected(std::move(binding.error()));
                if (binding->value_capacity == 0
                    || !std::has_single_bit(binding->value_capacity)
                    || binding->geometry != target_binding->geometry
                    || source.source_channel >= channel_count(binding->channel_layout)) {
                    return invalid(
                        "GraphJit sample composition contains an invalid source channel");
                }
                if (shift_writes_by_read_latency && target_history != 0) {
                    if (auto valid = require_revision(
                            source.source_representation, target_history);
                        !valid) {
                        return std::unexpected(std::move(valid.error()));
                    }
                }
            }
            for (auto const channel : contribution.target_channels) {
                if (channel >= populated.size() || populated[channel]) {
                    return invalid(
                        "GraphJit sample composition contains an invalid target projection");
                }
                populated[channel] = true;
            }

            RootContribution derived{.mix = std::move(*mix)};
            auto const aligned = contribution.feedback_alignment_representation
                != detail::no_sample_representation;
            if (!shift_writes_by_read_latency) {
                if (aligned || contribution.feedback_alignment_write_latency != 0) {
                    return invalid(
                        "GraphJit feed-forward sample composition unexpectedly owns feedback alignment storage");
                }
                result.push_back(std::move(derived));
                continue;
            }

            // Converting unequal-latency sources needs them at one logical
            // timestamp, so a mixing contribution stages them in alignment
            // storage and writes every channel at the smallest latency.
            auto const [minimum, maximum] = std::ranges::minmax(
                contribution.sources | std::views::transform(
                    &detail::SampleCompositionInputPlan::read_latency));
            auto const mixes_sources = std::ranges::any_of(
                std::views::iota(std::size_t{0}, derived.mix.converted_channel_count()),
                [&](std::size_t channel) {
                    return derived.mix.term_begins[channel + 1]
                        - derived.mix.term_begins[channel] > 1;
                });
            derived.write_latencies.resize(derived.mix.converted_channel_count());
            if (mixes_sources && minimum != maximum) {
                if (!aligned
                    || contribution.feedback_alignment_representation
                        >= storage.representations.size()
                    || contribution.feedback_alignment_write_latency != minimum) {
                    return invalid(
                        "GraphJit unequal-latency feedback mixing lost its alignment plan");
                }
                auto alignment = sample_storage_binding(
                    storage, contribution.feedback_alignment_representation);
                if (!alignment) return std::unexpected(std::move(alignment.error()));
                if (alignment->channel_layout != contribution.source_layout
                    || alignment->geometry != target_binding->geometry
                    || alignment->value_capacity == 0
                    || !std::has_single_bit(alignment->value_capacity)) {
                    return invalid(
                        "GraphJit unequal-latency feedback mixing has invalid alignment storage");
                }
                std::ranges::fill(derived.write_latencies, minimum);
            } else {
                if (aligned) {
                    return invalid(
                        "GraphJit feedback conversion unexpectedly owns alignment storage");
                }
                for (std::size_t channel = 0;
                     channel < derived.mix.converted_channel_count();
                     ++channel) {
                    std::optional<std::size_t> latency;
                    for (auto term = derived.mix.term_begins[channel];
                         term < derived.mix.term_begins[channel + 1];
                         ++term) {
                        auto const candidate = contribution.sources[
                            derived.mix.terms[term].source_channel].read_latency;
                        if (latency && *latency != candidate) {
                            return invalid(
                                "GraphJit feedback conversion mixes unaligned source latencies");
                        }
                        latency = candidate;
                    }
                    if (!latency) {
                        return invalid(
                            "GraphJit feedback conversion has a target channel with no source contribution");
                    }
                    derived.write_latencies[channel] = *latency;
                }
            }
            result.push_back(std::move(derived));
        }
        if (!std::ranges::all_of(populated, [](bool value) { return value; })) {
            return invalid(
                "GraphJit sample composition does not populate every target channel");
        }
        return result;
    }

    Checked prepare_composition(std::size_t index)
    {
        auto const& storage = plan_.sample_ports.storage;
        if (index >= storage.compositions.size()) {
            return invalid("GraphJit execution plan references a missing sample composition");
        }
        if (composition_ready_[index]) return {};
        auto const& composition = storage.compositions[index];
        auto written = composition_write(
            composition.contributions,
            composition.target_representation,
            composition.target_layout,
            composition.target_history,
            false);
        if (!written) return std::unexpected(std::move(written.error()));
        plan_.compositions[index] = std::move(*written);
        composition_ready_[index] = 1;
        return {};
    }

    Checked prepare_feedback_write(std::size_t index)
    {
        auto const& storage = plan_.sample_ports.storage;
        if (index >= storage.feedback_timelines.size()) {
            return invalid("GraphJit execution plan references a missing sample feedback timeline");
        }
        if (feedback_ready_[index]) return {};
        auto const& timeline = storage.feedback_timelines[index];
        if (timeline.timeline_representation >= storage.representations.size()) {
            return invalid("GraphJit sample feedback timeline references a missing storage");
        }
        auto const& target = storage.representations[timeline.timeline_representation];
        if (target.storage == RealtimeBufferStorageKind::transient_stack
            || target.persistent_allocation == detail::no_sample_persistent_allocation
            || target.channel_layout != timeline.channel_layout
            || timeline.loop_extra_latency == 0
            || timeline.loop_extra_latency >= target.frame_capacity) {
            return invalid(
                "GraphJit sample feedback timeline is inconsistent with its retained storage");
        }
        if (target.persistent_allocation >= storage.persistent_allocations.size()) {
            return invalid(
                "GraphJit sample feedback timeline references a missing persistent allocation");
        }
        auto const& allocation = storage.persistent_allocations[target.persistent_allocation];
        auto const expected_kind = target.storage
                == RealtimeBufferStorageKind::stack_with_persistent_carry
            ? detail::SamplePersistentStorageKind::compact_carry
            : detail::SamplePersistentStorageKind::ring;
        if (allocation.kind != expected_kind
            || allocation.representation_index != timeline.timeline_representation
            || allocation.retained_frames < timeline.retained_frames
            || !allocation.initialize_value
            || *allocation.initialize_value != timeline.initial_value
            || allocation.geometry != target.geometry) {
            return invalid(
                "GraphJit sample feedback timeline lost its persistent initialization semantics");
        }

        switch (timeline.writer.kind) {
        case detail::SampleFeedbackTimelineWriterKind::producer_home:
            return invalid(
                "GraphJit producer-home feedback timeline must not schedule a post-producer write");
        case detail::SampleFeedbackTimelineWriterKind::composition: {
            if (timeline.writer.source_representation != detail::no_sample_representation
                || timeline.writer.composition_contributions.empty()) {
                return invalid(
                    "GraphJit composed sample feedback timeline has an invalid writer shape");
            }
            auto written = composition_write(
                timeline.writer.composition_contributions,
                timeline.timeline_representation,
                timeline.channel_layout,
                timeline.writer.revision_frames,
                true);
            if (!written) return std::unexpected(std::move(written.error()));
            plan_.feedback_writes[index] = std::move(*written);
            break;
        }
        case detail::SampleFeedbackTimelineWriterKind::copy: {
            if (!timeline.writer.composition_contributions.empty()
                || timeline.writer.source_representation
                    >= storage.representations.size()) {
                return invalid(
                    "GraphJit copied sample feedback timeline has an invalid writer shape");
            }
            auto const& source = storage.representations[
                timeline.writer.source_representation];
            if (target.channel_layout != source.channel_layout
                || target.geometry != source.geometry) {
                return invalid(
                    "GraphJit sample feedback copy changed channel layout without composition");
            }
            constexpr auto capacity_message =
                "GraphJit sample feedback copy requires bounded power-of-two storage";
            if (auto binding = ring_storage_binding(
                    storage, timeline.writer.source_representation, capacity_message);
                !binding) {
                return std::unexpected(std::move(binding.error()));
            }
            if (auto binding = ring_storage_binding(
                    storage, timeline.timeline_representation, capacity_message);
                !binding) {
                return std::unexpected(std::move(binding.error()));
            }
            if (auto valid = require_revision(
                    timeline.writer.source_representation,
                    timeline.writer.revision_frames);
                !valid) {
                return valid;
            }
            if (auto valid = require_pace_cursor(plan_, target.absolute_pace_exponent);
                !valid) {
                return valid;
            }
            break;
        }
        }
        feedback_ready_[index] = 1;
        return {};
    }

    Checked prepare_sample_carry(std::size_t index)
    {
        auto const& storage = plan_.sample_ports.storage;
        if (index >= storage.carry_operations.size()) {
            return invalid("GraphJit execution plan references a missing sample carry operation");
        }
        if (sample_carry_ready_[index]) return {};
        auto const& operation = storage.carry_operations[index];
        if (operation.representation_index >= storage.representations.size()) {
            return invalid("GraphJit sample carry references a missing storage");
        }
        if (operation.persistent_allocation >= storage.persistent_allocations.size()) {
            return invalid("GraphJit sample carry references a missing persistent allocation");
        }
        auto const& representation = storage.representations[operation.representation_index];
        auto const& allocation = storage.persistent_allocations[operation.persistent_allocation];
        if (allocation.kind != detail::SamplePersistentStorageKind::compact_carry
            || allocation.representation_index != operation.representation_index
            || representation.persistent_allocation != operation.persistent_allocation
            || representation.transient_allocation == detail::no_sample_transient_allocation
            || allocation.retained_frames != operation.retained_frames
            || operation.retained_frames == 0
            || operation.future_frames > operation.retained_frames) {
            return invalid("GraphJit sample carry plan is inconsistent with its storage");
        }
        auto working = sample_storage_binding(storage, operation.representation_index);
        if (!working) return std::unexpected(std::move(working.error()));
        if (working->value_capacity == 0
            || !std::has_single_bit(working->value_capacity)
            || working->channel_layout != allocation.channel_layout
            || working->geometry != allocation.geometry) {
            return invalid("GraphJit sample carry working storage is invalid");
        }
        if (auto valid = require_pace_cursor(plan_, representation.absolute_pace_exponent);
            !valid) {
            return valid;
        }
        sample_carry_ready_[index] = 1;
        return {};
    }

    // --- event operations -------------------------------------------------

    Checked validate_event_operation(detail::EventOperationRef operation) const
    {
        auto const& events = plan_.event_ports;
        auto const& ports = events.ports;
        switch (operation.kind) {
        case detail::EventOperationKind::sequence_reset:
            if (operation.index >= ports.size()) {
                return invalid("GraphJit event sequence reset references a missing storage");
            }
            return {};
        case detail::EventOperationKind::persistent_ring_prune: {
            if (operation.index >= events.persistent_rings.size()) {
                return invalid("GraphJit execution plan references a missing persistent event ring");
            }
            auto const& ring = events.persistent_rings[operation.index];
            if (ring.storage >= ports.size()) {
                return invalid("GraphJit persistent event ring references a missing storage");
            }
            auto const& storage = ports[ring.storage];
            if (!storage.region.valid() || !storage.persistent
                || !storage.persistent_ring || !storage.has_sequential_timeline) {
                return invalid("GraphJit persistent event ring has inconsistent storage");
            }
            return {};
        }
        case detail::EventOperationKind::carry_restore:
        case detail::EventOperationKind::carry_commit: {
            if (operation.index >= events.carry_operations.size()) {
                return invalid("GraphJit execution plan references a missing event carry operation");
            }
            auto const& carry = events.carry_operations[operation.index];
            if (carry.working_representation >= ports.size()
                || carry.persistent_representation >= ports.size()) {
                return invalid("GraphJit event carry references a missing storage");
            }
            auto const& working = ports[carry.working_representation];
            auto const& persistent = ports[carry.persistent_representation];
            if (working.persistent || !persistent.persistent
                || working.type != persistent.type
                || working.has_source_indices != persistent.has_source_indices
                || working.has_sequential_timeline
                || !persistent.has_sequential_timeline) {
                return invalid("GraphJit event carry has inconsistent storage ports");
            }
            return {};
        }
        case detail::EventOperationKind::merge: {
            if (operation.index >= events.merges.size()) {
                return invalid("GraphJit execution plan references a missing event merge");
            }
            auto const& merge = events.merges[operation.index];
            if (merge.target_representation >= ports.size()
                || merge.source_representations.empty()) {
                return invalid("GraphJit event merge references a missing storage");
            }
            auto const& target = ports[merge.target_representation];
            if ((target.has_source_indices
                    && merge.source_indices.size() != merge.source_representations.size())
                || (!target.has_source_indices && !merge.source_indices.empty())) {
                return invalid("GraphJit ordered event merge has inconsistent source indices");
            }
            if (!merge.source_delays.empty()
                && merge.source_delays.size() != merge.source_representations.size()) {
                return invalid("GraphJit event merge has inconsistent source delays");
            }
            if (target.event_capacity != 0 && !std::has_single_bit(target.event_capacity)) {
                return invalid("GraphJit event merge target has invalid storage");
            }
            if (target.persistent_ring && !merge.preserve_existing_target) {
                return invalid(
                    "GraphJit persistent event merge must preserve retained target events");
            }
            for (auto const source_index : merge.source_representations) {
                if (source_index >= ports.size()) {
                    return invalid("GraphJit event merge references a missing source storage");
                }
                auto const& source = ports[source_index];
                if (source.persistent_ring || source.type != target.type
                    || source.has_source_indices) {
                    return invalid("GraphJit event merge source has inconsistent storage");
                }
            }
            return {};
        }
        case detail::EventOperationKind::materialize: {
            if (operation.index >= events.materializations.size()) {
                return invalid("GraphJit execution plan references a missing event materialization");
            }
            auto const& materialization = events.materializations[operation.index];
            if (materialization.source_representation >= ports.size()
                || materialization.target_representation >= ports.size()) {
                return invalid("GraphJit event materialization references a missing storage");
            }
            auto const& source = ports[materialization.source_representation];
            auto const& target = ports[materialization.target_representation];
            if (materialization.conversion.source_type != source.type
                || materialization.conversion.target_type != target.type
                || materialization.conversion.step_count > EventConversionPlan::max_steps) {
                return invalid(
                    "GraphJit transient event materialization has an invalid conversion plan");
            }
            if ((materialization.source_index_filter
                    || materialization.preserve_source_indices)
                && !source.has_source_indices) {
                return invalid(
                    "GraphJit event source selection lost its source-index sidecar");
            }
            if (materialization.preserve_source_indices && !target.has_source_indices) {
                return invalid(
                    "GraphJit event materialization target lost its source-index sidecar");
            }
            auto const windowed = materialization.source_index_filter
                || materialization.select_invocation_window
                || source.persistent_ring;
            if (!windowed && materialization.conversion.step_count == 0
                && source.type != target.type) {
                return invalid("GraphJit identity event materialization changes event type");
            }
            return {};
        }
        case detail::EventOperationKind::feedback_append: {
            if (operation.index >= events.feedback_operations.size()) {
                return invalid("GraphJit execution plan references a missing event feedback append");
            }
            auto const& feedback = events.feedback_operations[operation.index];
            if (feedback.source_representation >= ports.size()
                || feedback.target_representation >= ports.size()) {
                return invalid("GraphJit event feedback references a missing storage");
            }
            if (feedback.source_sequence_restarts_each_append
                == feedback.selects_final_window) {
                return invalid(
                    "GraphJit event feedback must consume either its whole callback sequence or its final window");
            }
            auto const& source = ports[feedback.source_representation];
            auto const& target = ports[feedback.target_representation];
            if (source.type != target.type) {
                return invalid("GraphJit exact-type event feedback changed event type");
            }
            if (feedback.storage_plan.kind == RealtimeBufferStorageKind::full_node_storage) {
                if (!target.persistent || !target.persistent_ring) {
                    return invalid("GraphJit full event feedback has no persistent ring");
                }
            } else if (target.persistent || target.persistent_ring) {
                return invalid(
                    "GraphJit transient/carry event feedback has invalid working storage");
            }
            if (feedback.source_index_filter && !source.has_source_indices) {
                return invalid(
                    "GraphJit filtered event feedback lost its source-index sidecar");
            }
            return {};
        }
        }
        return invalid("GraphJit execution plan contains an unknown event operation");
    }

    // --- primitives -------------------------------------------------------

    Checked build_primitives()
    {
        auto const count = plan_.sample_ports.primitives.size();
        if (plan_.event_ports.primitives.size() != count
            || lowering_.configurations.nodes.size() != count
            || lowering_.imports.primitive_callbacks.size() != count
            || plan_.primitive_activity.size() != count) {
            return invalid("GraphJit realtime root primitive inventories disagree");
        }

        std::unordered_map<std::string_view, void const*> retained_globals;
        for (auto const& package : input_.packages) {
            for (auto const& global : package.retained_globals) {
                if (!global.symbol.empty() && global.address) {
                    retained_globals.emplace(global.symbol, global.address);
                }
            }
        }
        std::unordered_map<std::size_t, NodeImplementation const*> implementations;
        for (auto const& implementation : input_.node_implementations) {
            implementations.emplace(implementation.node_bundle, &implementation);
        }

        plan_.tracks_activity = std::ranges::any_of(
            plan_.primitive_activity,
            [](auto const& activity) { return activity.has_value(); });
        plan_.primitives.resize(count);
        for (std::size_t index = 0; index < count; ++index) {
            auto& primitive = plan_.primitives[index];
            auto const& configuration = lowering_.configurations.nodes[index];
            if (configuration.alignment == 0
                || !std::has_single_bit(configuration.alignment)) {
                return invalid("GraphJit node configuration alignment is not a power of two");
            }
            primitive.node_data = AlignedBytes(
                configuration.bytes.size(), configuration.alignment);
            if (!configuration.bytes.empty()) {
                std::memcpy(
                    primitive.node_data.data(),
                    configuration.bytes.data(),
                    configuration.bytes.size());
            }
            std::size_t cursor = 0;
            for (auto const& relocation : configuration.relocations) {
                if (relocation.byte_offset < cursor
                    || relocation.byte_offset > configuration.bytes.size()
                    || sizeof(void*) > configuration.bytes.size() - relocation.byte_offset) {
                    return invalid(
                        "planned node configuration relocation is outside configuration bytes");
                }
                void const* pointer = nullptr;
                if (!relocation.retained_global_symbol.empty()) {
                    auto const found = retained_globals.find(
                        relocation.retained_global_symbol);
                    if (found == retained_globals.end()) {
                        return invalid(
                            "planned node configuration relocation references an unavailable retained global");
                    }
                    pointer = static_cast<std::byte const*>(found->second)
                        + relocation.addend;
                }
                store_raw(primitive.node_data.data(), relocation.byte_offset, pointer);
                cursor = relocation.byte_offset + sizeof(void*);
            }

            // A primitive no step executes needs no native callbacks;
            // build_steps() rejects a step whose primitive has none.
            auto const bundle = lowering_.imports.primitive_callbacks[index].bundle;
            if (auto const implementation = implementations.find(bundle);
                implementation != implementations.end()) {
                primitive.callbacks = implementation->second->native_operations;
            }

            if (auto valid = build_tick_slots(index, bundle); !valid) return valid;
            if (auto valid = build_manual_controls(index); !valid) return valid;
        }
        return build_binding_template();
    }

    Checked build_tick_slots(std::size_t index, NodeBundleHandle bundle)
    {
        auto& primitive = plan_.primitives[index];
        auto const& sample_inputs = plan_.sample_ports.primitives[index].inputs;
        auto const& sample_outputs = plan_.sample_ports.primitives[index].outputs;
        auto const& event_inputs = plan_.event_ports.primitives[index].inputs;
        auto const& event_outputs = plan_.event_ports.primitives[index].outputs;
        primitive.slot_supplied_event_inputs.assign(event_inputs.size(), 0);

        auto const& background = lowering_.connections.background;
        if (bundle >= background.bundle_to_background_node.size()
            || !background.bundle_to_background_node[bundle]) {
            return {};
        }
        auto const node = *background.bundle_to_background_node[bundle];
        if (node >= background.tick_runtime.nodes.size()) {
            return invalid("GraphJit Tick invocation map references a missing node");
        }
        auto const& runtime = background.tick_runtime;
        RootTickSlots tick{.invocation = runtime.nodes[node]};
        auto const& invocation = tick.invocation;

        auto slot_inputs = [&](std::span<TickSequentialBindingPlan const> bindings,
                               std::size_t begin,
                               std::size_t slot_count,
                               PortKind kind,
                               std::size_t input_count,
                               std::vector<std::size_t>& result) -> Checked {
            if (begin > bindings.size() || slot_count > bindings.size() - begin) {
                return invalid("GraphJit Tick Sequential binding range is out of bounds");
            }
            for (std::size_t slot = begin; slot < begin + slot_count; ++slot) {
                if (bindings[slot].port >= background.ports.size()) {
                    return invalid(
                        "GraphJit Tick Sequential binding references a missing port");
                }
                auto const& port = background.ports[bindings[slot].port];
                if (port.node != node || port.kind != kind
                    || port.configured_port.port_index >= input_count) {
                    return invalid(
                        "GraphJit Tick Sequential binding has no matching primitive input");
                }
                result.push_back(port.configured_port.port_index);
            }
            return {};
        };
        if (auto valid = slot_inputs(
                runtime.sequential_sample_inputs,
                invocation.sequential_sample_begin,
                invocation.sequential_sample_count,
                PortKind::sample,
                sample_inputs.size(),
                tick.sample_inputs);
            !valid) {
            return valid;
        }
        if (auto valid = slot_inputs(
                runtime.sequential_event_inputs,
                invocation.sequential_event_begin,
                invocation.sequential_event_count,
                PortKind::event,
                event_inputs.size(),
                tick.event_inputs);
            !valid) {
            return valid;
        }
        for (std::size_t slot = 0; slot < tick.sample_inputs.size(); ++slot) {
            (sample_inputs[tick.sample_inputs[slot]].channels.empty()
                    ? tick.sample_overlay_slots
                    : tick.sample_timeline_slots)
                .push_back(slot);
        }
        for (std::size_t slot = 0; slot < tick.event_inputs.size(); ++slot) {
            primitive.slot_supplied_event_inputs[tick.event_inputs[slot]] = 1;
            (event_inputs[tick.event_inputs[slot]].storage
                    ? tick.event_timeline_slots
                    : tick.event_overlay_slots)
                .push_back(slot);
        }
        for (auto const input : tick.sample_inputs) {
            auto const& planned = sample_inputs[input];
            if (planned.channels.empty()) continue;
            for (std::size_t channel = 0; channel < planned.channels.size(); ++channel) {
                if (auto valid = validate_timeline_channel(
                        plan_, planned.channels[channel]);
                    !valid) {
                    return valid;
                }
            }
            if (planned.combines_invocation_supplied) {
                if (planned.realtime_operand_channels.size() != planned.channels.size()
                    || !primitive.callbacks.combine_sequential_input_values) {
                    return invalid("GraphJit mixed sample input lost its realtime operand");
                }
                for (auto const& operand : planned.realtime_operand_channels) {
                    if (auto valid = validate_timeline_channel(plan_, operand);
                        !valid) {
                        return valid;
                    }
                }
            }
        }
        for (auto const input : tick.event_inputs) {
            auto const& planned = event_inputs[input];
            if (!planned.storage) continue;
            auto const& ports = plan_.event_ports.ports;
            if (*planned.storage >= ports.size()
                || !ports[*planned.storage].persistent
                || !ports[*planned.storage].persistent_ring) {
                return invalid("GraphJit slot-fed event input has no timeline ring");
            }
            if (planned.combines_invocation_supplied
                && (!planned.realtime_operand_storage
                    || *planned.realtime_operand_storage >= ports.size()
                    || !planned.combined_storage
                    || *planned.combined_storage >= ports.size())) {
                return invalid("GraphJit mixed event input lost its planned storage");
            }
        }

        auto captures = [&](std::span<TickCaptureBindingPlan const> bindings,
                            std::size_t begin,
                            std::size_t capture_count,
                            PortKind kind,
                            auto const& outputs,
                            std::vector<std::size_t>& result) -> Checked {
            if (begin > bindings.size() || capture_count > bindings.size() - begin) {
                return invalid("GraphJit Tick capture range is out of bounds");
            }
            for (std::size_t slot = begin; slot < begin + capture_count; ++slot) {
                if (bindings[slot].port >= background.ports.size()) {
                    return invalid("GraphJit Tick capture references a missing logical port");
                }
                auto const& port = background.ports[bindings[slot].port];
                auto const authored = port.configured_port.port_index;
                if (port.node != node || port.kind != kind
                    || port.direction != PortDirection::output
                    || !port.persisted_tick_output
                    || port.retention != OutputRetention::persisted
                    || authored >= outputs.size() || !outputs[authored].realtime) {
                    return invalid("GraphJit Tick capture has no matching realtime output");
                }
                result.push_back(static_cast<std::size_t>(std::ranges::count_if(
                    outputs.begin(),
                    outputs.begin() + static_cast<std::ptrdiff_t>(authored),
                    [](auto const& output) { return output.realtime; })));
            }
            return {};
        };
        if (auto valid = captures(
                runtime.sample_captures,
                invocation.sample_capture_begin,
                invocation.sample_capture_count,
                PortKind::sample,
                sample_outputs,
                tick.sample_capture_outputs);
            !valid) {
            return valid;
        }
        if (auto valid = captures(
                runtime.event_captures,
                invocation.event_capture_begin,
                invocation.event_capture_count,
                PortKind::event,
                event_outputs,
                tick.event_capture_outputs);
            !valid) {
            return valid;
        }
        // Only a primitive with a slot reads the TickInvocationCall.
        if (invocation.sequential_sample_count == 0
            && invocation.sequential_event_count == 0
            && invocation.random_access_sample_count == 0
            && invocation.random_access_event_count == 0
            && invocation.sample_capture_count == 0
            && invocation.event_capture_count == 0) {
            return {};
        }
        primitive.tick = std::move(tick);
        plan_.tick_primitives.push_back(index);
        plan_.has_tick_slots = true;
        return {};
    }

    Checked build_manual_controls(std::size_t index)
    {
        auto& primitive = plan_.primitives[index];
        auto const& inputs = plan_.sample_ports.primitives[index].inputs;
        auto const& controls = plan_.sample_ports.manual_controls;
        for (std::size_t control_index = 0; control_index < controls.size(); ++control_index) {
            auto const& control = controls[control_index];
            if (control.primitive_index != index) continue;
            if (control.input_index >= inputs.size()) {
                return invalid("GraphJit manual control lost its primitive input");
            }
            primitive.manual_controls.push_back(control_index);
            if (!control.stages_timeline) continue;
            auto const& input = inputs[control.input_index];
            auto const channels = channel_count(input.channel_layout);
            if (!input.manual_control
                || *input.manual_control >= controls.size()
                || input.manual_timeline_channels.size() != channels
                || input.geometry.value_type != ContinuousValueTypeId::sample
                || input.geometry.value_size_bytes != sizeof(Sample)
                || (!input.manual_operand_channels.empty()
                    && input.manual_operand_channels.size() != channels)) {
                return invalid("GraphJit manual control lost its target timeline");
            }
            if ((input.invocation_supplied || !input.manual_operand_channels.empty())
                && !primitive.callbacks.combine_sequential_input_values) {
                return invalid("GraphJit manual input has no combination callback");
            }
            for (auto const& channel : input.manual_timeline_channels) {
                if (auto valid = validate_timeline_channel(plan_, channel); !valid) {
                    return valid;
                }
            }
            for (auto const& channel : input.manual_operand_channels) {
                if (auto valid = validate_timeline_channel(plan_, channel); !valid) {
                    return valid;
                }
            }
        }
        return {};
    }

    // Facts the root needs only for a primitive that a step executes.
    Checked prepare_executed_primitive(std::size_t index)
    {
        if (primitive_ready_[index]) {
            return invalid("GraphJit execution plan executes a primitive more than once");
        }
        auto& primitive = plan_.primitives[index];
        auto const& callbacks = primitive.callbacks;
        if (!callbacks.tick_block
            || (plan_.primitive_activity[index] && !callbacks.skip_block)) {
            return invalid("GraphJit execution plan references a missing primitive callback");
        }
        if (auto valid = validate_activity(index); !valid) return valid;
        auto const& sample = plan_.sample_ports.primitives[index];
        for (std::size_t input = 0; input < sample.inputs.size(); ++input) {
            auto const& planned = sample.inputs[input];
            if (planned.random_access) continue;
            auto update = RootBeginIndex{
                .field_offset = primitive.sample_inputs_offset
                    + input * sizeof(ReflectedContinuousInputPortBinding)
                    + offsetof(ReflectedContinuousInputPortBinding, sequential_begin_index),
                .pace_exponent = planned.absolute_pace_exponent,
            };
            if (planned.channels.empty()) {
                if (auto valid = require_pace(planned.absolute_pace_exponent); !valid) {
                    return valid;
                }
            } else {
                if (auto valid = require_pace_cursor(plan_, planned.absolute_pace_exponent);
                    !valid) {
                    return valid;
                }
                update.cursor = pace_cursor(plan_, planned.absolute_pace_exponent);
            }
            primitive.begin_indices.push_back(update);
        }
        std::size_t compact = 0;
        for (auto const& output : sample.outputs) {
            if (!output.realtime) continue;
            if (auto valid = require_pace_cursor(plan_, output.absolute_pace_exponent);
                !valid) {
                return valid;
            }
            primitive.begin_indices.push_back(RootBeginIndex{
                .field_offset = primitive.sample_outputs_offset
                    + compact++ * sizeof(ReflectedContinuousOutputPortBinding)
                    + offsetof(ReflectedContinuousOutputPortBinding, sequential_begin_index),
                .cursor = pace_cursor(plan_, output.absolute_pace_exponent),
                .pace_exponent = output.absolute_pace_exponent,
            });
        }
        for (auto const control : primitive.manual_controls) {
            auto const& planned = plan_.sample_ports.manual_controls[control];
            if (!planned.stages_timeline) continue;
            if (auto valid = require_pace_cursor(
                    plan_, sample.inputs[planned.input_index].absolute_pace_exponent);
                !valid) {
                return valid;
            }
        }
        if (primitive.tick) {
            for (auto const slot : primitive.tick->sample_timeline_slots) {
                if (auto valid = require_pace_cursor(
                        plan_,
                        sample.inputs[primitive.tick->sample_inputs[slot]]
                            .absolute_pace_exponent);
                    !valid) {
                    return valid;
                }
            }
        }
        primitive_ready_[index] = 1;
        return {};
    }

    Checked validate_activity(std::size_t index)
    {
        auto const& selected = plan_.primitive_activity[index];
        if (!selected) return {};
        auto const& activity = *selected;
        auto const& sample = plan_.sample_ports.primitives[index];
        if (activity.continuous_inputs.size() != sample.inputs.size()) {
            return invalid(
                "GraphJit activity input plan must match the primitive binding plan");
        }
        for (std::size_t input = 0; input < activity.continuous_inputs.size(); ++input) {
            auto const& input_activity = activity.continuous_inputs[input];
            if (input_activity.static_constant) continue;
            auto const& binding = sample.inputs[input];
            if (input_activity.channels.size()
                != (binding.invocation_supplied
                    ? channel_count(binding.channel_layout)
                    : binding.channels.size())) {
                return invalid(
                    "GraphJit activity channel plan must match the primitive binding");
            }
            for (auto const& channel : input_activity.channels) {
                if (channel.frontier >= plan_.activity_frontiers.size()) {
                    return invalid("GraphJit activity channel must reference a frontier");
                }
                auto const representation =
                    plan_.activity_frontiers[channel.frontier].representation;
                if (representation != detail::no_sample_representation
                    && representation >= plan_.sample_ports.storage.representations.size()) {
                    return invalid("GraphJit activity frontier must reference sample storage");
                }
            }
        }
        auto const outputs = static_cast<std::size_t>(std::ranges::count_if(
            sample.outputs, [](auto const& output) { return output.realtime; }));
        if (activity.continuous_output_channel_counts.size() != outputs) {
            return invalid(
                "GraphJit activity output plan must match the primitive binding");
        }
        return {};
    }

    // --- binding template -------------------------------------------------

    std::size_t binding_area_ = 0;
    std::size_t binding_alignment_ = 1;

    std::size_t reserve_bindings(std::size_t count, std::size_t size, std::size_t alignment)
    {
        binding_alignment_ = std::max(binding_alignment_, alignment);
        binding_area_ = align_up(binding_area_, alignment);
        auto const offset = binding_area_;
        binding_area_ += count * size;
        return offset;
    }

    template<class T>
    void write_template(std::size_t area_offset, T const& value)
    {
        std::memcpy(plan_.binding_template.data() + area_offset, &value, sizeof(T));
    }

    void add_pointer(
        std::size_t area_field_offset,
        BindingPointerBase base,
        std::size_t representation,
        std::size_t byte_offset)
    {
        plan_.binding_pointers.push_back(BindingPointer{
            .field_offset = area_field_offset,
            .base = base,
            .representation = representation,
            .byte_offset = byte_offset,
        });
    }

    static constexpr std::size_t channel_field_offset(
        std::size_t storage_offset, std::size_t channel)
    {
        return storage_offset
            + offsetof(ReflectedContinuousPortStorageBinding, channels)
            + channel * sizeof(ReflectedContinuousChannelStorageBinding);
    }

    // The byte offset of one channel inside its representation.
    std::size_t channel_byte_offset(
        std::size_t representation_index,
        std::size_t representation_channel) const
    {
        auto const& representation = plan_.sample_ports.storage
            .representations[representation_index];
        auto const value = representation.channel_layout.sample_layout
                == SampleStreamLayout::planar
            ? representation_channel * representation.frame_capacity
            : representation_channel;
        return value * representation.geometry.value_size_bytes;
    }

    Checked build_binding_template()
    {
        // Area offsets first, then the template, then the pointer fields.
        for (std::size_t index = 0; index < plan_.primitives.size(); ++index) {
            auto& primitive = plan_.primitives[index];
            auto const& sample = plan_.sample_ports.primitives[index];
            auto const& event = plan_.event_ports.primitives[index];
            primitive.sample_output_count = static_cast<std::size_t>(std::ranges::count_if(
                sample.outputs, [](auto const& output) { return output.realtime; }));
            primitive.event_output_count = static_cast<std::size_t>(std::ranges::count_if(
                event.outputs, [](auto const& output) { return output.realtime; }));
            primitive.sample_inputs_offset = reserve_bindings(
                sample.inputs.size(),
                sizeof(ReflectedContinuousInputPortBinding),
                alignof(ReflectedContinuousInputPortBinding));
            primitive.sample_outputs_offset = reserve_bindings(
                primitive.sample_output_count,
                sizeof(ReflectedContinuousOutputPortBinding),
                alignof(ReflectedContinuousOutputPortBinding));
            primitive.event_inputs_offset = reserve_bindings(
                event.inputs.size(),
                sizeof(ReflectedEventInputPortBinding),
                alignof(ReflectedEventInputPortBinding));
            primitive.event_outputs_offset = reserve_bindings(
                primitive.event_output_count,
                sizeof(ReflectedEventOutputPortBinding),
                alignof(ReflectedEventOutputPortBinding));
            primitive.context_offset = reserve_bindings(
                1, sizeof(ReflectedNodeTickContext), alignof(ReflectedNodeTickContext));
        }
        // The binding area follows the root stack. Primitive offsets become
        // scratch offsets; the template is indexed relative to the area.
        plan_.binding_offset = align_up(plan_.root_stack.size_bytes, binding_alignment_);
        plan_.binding_template.assign(binding_area_, std::byte{});
        auto const area = plan_.binding_offset;
        for (auto& primitive : plan_.primitives) {
            primitive.sample_inputs_offset += area;
            primitive.sample_outputs_offset += area;
            primitive.event_inputs_offset += area;
            primitive.event_outputs_offset += area;
            primitive.context_offset += area;
        }

        auto const& storage = plan_.sample_ports.storage;
        auto const& events = plan_.event_ports;
        for (std::size_t index = 0; index < plan_.primitives.size(); ++index) {
            auto& primitive = plan_.primitives[index];
            auto const& sample = plan_.sample_ports.primitives[index];
            auto const& event = plan_.event_ports.primitives[index];

            for (std::size_t input_index = 0; input_index < sample.inputs.size(); ++input_index) {
                auto const& input = sample.inputs[input_index];
                auto const offset = primitive.sample_inputs_offset
                    + input_index * sizeof(ReflectedContinuousInputPortBinding);
                auto const storage_offset = offset
                    + offsetof(ReflectedContinuousInputPortBinding, storage);
                // A Random Access input reads only its TickInvocationCall view;
                // its declared entry is never read.
                if (input.random_access) {
                    write_template(offset - area, ReflectedContinuousInputPortBinding{});
                    continue;
                }
                ReflectedContinuousPortStorageBinding binding{
                    .geometry = input.geometry,
                    .channel_layout = input.channel_layout,
                };
                if (!input.channels.empty()) {
                    auto resolved = sample_input_storage_binding(storage, input);
                    if (!resolved) return std::unexpected(std::move(resolved.error()));
                    binding = *resolved;
                    for (std::size_t channel = 0; channel < input.channels.size(); ++channel) {
                        auto const& source = input.channels[channel];
                        add_pointer(
                            channel_field_offset(storage_offset, channel)
                                + offsetof(ReflectedContinuousChannelStorageBinding, storage),
                            BindingPointerBase::sample_representation,
                            source.storage,
                            channel_byte_offset(source.storage, source.representation_channel));
                    }
                }
                if (input.has_contiguous_scratch) {
                    auto const value_size = input.geometry.value_size_bytes;
                    auto const channels = channel_count(input.channel_layout);
                    if (value_size == 0
                        || input.contiguous_storage_capacity
                            > std::numeric_limits<std::size_t>::max() / value_size
                            / std::max<std::size_t>(channels, 1)) {
                        return invalid("GraphJit continuous input scratch lost its root arena");
                    }
                    auto const channel_bytes = input.contiguous_storage_capacity * value_size;
                    auto scratch = arena_offset(
                        plan_.root_stack.sample_offset,
                        storage.transient_arena_size,
                        input.contiguous_scratch_offset,
                        channels * channel_bytes,
                        "GraphJit continuous input scratch lost its root arena");
                    if (!scratch) return std::unexpected(std::move(scratch.error()));
                    for (std::size_t channel = 0; channel < channels; ++channel) {
                        add_pointer(
                            channel_field_offset(storage_offset, channel)
                                + offsetof(
                                    ReflectedContinuousChannelStorageBinding,
                                    contiguous_scratch),
                            BindingPointerBase::scratch,
                            0,
                            *scratch + channel * channel_bytes);
                    }
                }
                write_template(offset - area, ReflectedContinuousInputPortBinding{
                    .storage = binding,
                    .history = input.history,
                    .read_latency = input.read_latency,
                    .absolute_pace_exponent = input.absolute_pace_exponent,
                    .contiguous_value_capacity = input.contiguous_value_capacity,
                });
            }

            std::size_t compact = 0;
            for (auto const& output : sample.outputs) {
                if (!output.realtime) continue;
                if (!output.storage) {
                    return invalid("GraphJit sample output has no storage");
                }
                auto binding = sample_storage_binding(storage, *output.storage);
                if (!binding) return std::unexpected(std::move(binding.error()));
                auto const offset = primitive.sample_outputs_offset
                    + compact * sizeof(ReflectedContinuousOutputPortBinding);
                auto const storage_offset = offset
                    + offsetof(ReflectedContinuousOutputPortBinding, storage);
                for (std::size_t channel = 0;
                     channel < channel_count(binding->channel_layout);
                     ++channel) {
                    add_pointer(
                        channel_field_offset(storage_offset, channel)
                            + offsetof(ReflectedContinuousChannelStorageBinding, storage),
                        BindingPointerBase::sample_representation,
                        *output.storage,
                        channel_byte_offset(*output.storage, channel));
                }
                write_template(offset - area, ReflectedContinuousOutputPortBinding{
                    .storage = *binding,
                    .history = output.history,
                    .latency = output.latency,
                    .absolute_pace_exponent = output.absolute_pace_exponent,
                });
                primitive.output_sample_representations.push_back(*output.storage);
                ++compact;
            }

            auto event_storage = [&](std::size_t representation) {
                auto const& port = events.ports[representation];
                return ReflectedEventPortStorageBinding{
                    .storage = nullptr,
                    .count_offset = port.count_relative_offset,
                    .read_index_offset = port.read_index_relative_offset,
                    .write_index_offset = port.write_index_relative_offset,
                    .events_offset = port.events_relative_offset,
                    .event_capacity = port.event_capacity,
                    .type = port.type,
                    .persistent_ring = port.persistent_ring,
                };
            };
            for (std::size_t input_index = 0; input_index < event.inputs.size(); ++input_index) {
                auto const& input = event.inputs[input_index];
                auto const offset = primitive.event_inputs_offset
                    + input_index * sizeof(ReflectedEventInputPortBinding);
                if (!input.storage) {
                    // A slot-fed input without history is replaced by its
                    // slot; a Random Access input reads only its view.
                    if (!input.invocation_supplied && !input.random_access) {
                        return invalid("GraphJit event input has no storage");
                    }
                    write_template(offset - area, ReflectedEventInputPortBinding{});
                    continue;
                }
                if (*input.storage >= events.ports.size()) {
                    return invalid("GraphJit event port references missing storage");
                }
                add_pointer(
                    offset + offsetof(ReflectedEventInputPortBinding, storage)
                        + offsetof(ReflectedEventPortStorageBinding, storage),
                    BindingPointerBase::event_representation,
                    *input.storage,
                    0);
                auto const& port = events.ports[*input.storage];
                if (port.has_source_indices) {
                    add_pointer(
                        offset
                            + offsetof(ReflectedEventInputPortBinding, semantic_source_indices),
                        BindingPointerBase::event_representation,
                        *input.storage,
                        port.source_indices_relative_offset);
                }
                write_template(offset - area, ReflectedEventInputPortBinding{
                    .storage = event_storage(*input.storage),
                });
            }

            compact = 0;
            for (auto const& output : event.outputs) {
                if (!output.realtime) continue;
                if (!output.storage) return invalid("GraphJit event output has no storage");
                if (*output.storage >= events.ports.size()) {
                    return invalid("GraphJit event port references missing storage");
                }
                auto const& port = events.ports[*output.storage];
                if (!port.has_producer_overflow_counter) {
                    return invalid("GraphJit event output storage lost producer telemetry");
                }
                auto const offset = primitive.event_outputs_offset
                    + compact * sizeof(ReflectedEventOutputPortBinding);
                add_pointer(
                    offset + offsetof(ReflectedEventOutputPortBinding, storage)
                        + offsetof(ReflectedEventPortStorageBinding, storage),
                    BindingPointerBase::event_representation,
                    *output.storage,
                    0);
                add_pointer(
                    offset + offsetof(ReflectedEventOutputPortBinding, overflow_count),
                    BindingPointerBase::node_storage,
                    0,
                    port.overflow_count_storage_offset);
                write_template(offset - area, ReflectedEventOutputPortBinding{
                    .storage = event_storage(*output.storage),
                    .source_type = output.source_type,
                    .history = output.history,
                    .latency = output.latency,
                    .append_existing = output.append_existing,
                });
                primitive.output_event_representations.push_back(*output.storage);
                ++compact;
            }

            // The context addresses this call's binding arrays. build_steps()
            // adds the State view and initialize_tick_slots() the Random
            // Access views.
            auto context = lowering_.configurations.nodes[index].tick_context_template;
            context.continuous_input_bindings = {nullptr, sample.inputs.size()};
            context.continuous_output_bindings = {nullptr, primitive.sample_output_count};
            context.event_input_bindings = {nullptr, event.inputs.size()};
            context.event_output_bindings = {nullptr, primitive.event_output_count};
            write_template(primitive.context_offset - area, context);
            auto bind_span = [&](std::size_t span_offset,
                            std::size_t count,
                            std::size_t array_offset) {
                if (count == 0) return;
                add_pointer(
                    primitive.context_offset + span_offset,
                    BindingPointerBase::scratch,
                    0,
                    array_offset);
            };
            bind_span(offsetof(ReflectedNodeTickContext, continuous_input_bindings)
                    + offsetof(
                        ReflectedSpan<ReflectedContinuousInputPortBinding const>,
                        pointer),
                sample.inputs.size(),
                primitive.sample_inputs_offset);
            bind_span(offsetof(ReflectedNodeTickContext, continuous_output_bindings)
                    + offsetof(
                        ReflectedSpan<ReflectedContinuousOutputPortBinding const>,
                        pointer),
                primitive.sample_output_count,
                primitive.sample_outputs_offset);
            bind_span(offsetof(ReflectedNodeTickContext, event_input_bindings)
                    + offsetof(ReflectedSpan<ReflectedEventInputPortBinding const>, pointer),
                event.inputs.size(),
                primitive.event_inputs_offset);
            bind_span(offsetof(ReflectedNodeTickContext, event_output_bindings)
                    + offsetof(ReflectedSpan<ReflectedEventOutputPortBinding const>, pointer),
                primitive.event_output_count,
                primitive.event_outputs_offset);
        }
        return {};
    }

    // --- execution --------------------------------------------------------

    // Appends validated event operations in the root's operation form.
    Checked append_event_operations(
        std::span<detail::EventOperationRef const> operations,
        std::vector<RootOperation>& result) const
    {
        for (auto const operation : operations) {
            if (auto valid = validate_event_operation(operation); !valid) return valid;
            auto kind = RootOperationKind::event_sequence_reset;
            switch (operation.kind) {
            case detail::EventOperationKind::sequence_reset:
                kind = RootOperationKind::event_sequence_reset;
                break;
            case detail::EventOperationKind::persistent_ring_prune:
                kind = RootOperationKind::event_ring_prune;
                break;
            case detail::EventOperationKind::carry_restore:
                kind = RootOperationKind::event_carry_restore;
                break;
            case detail::EventOperationKind::carry_commit:
                kind = RootOperationKind::event_carry_commit;
                break;
            case detail::EventOperationKind::merge:
                kind = RootOperationKind::event_merge;
                break;
            case detail::EventOperationKind::materialize:
                kind = RootOperationKind::event_materialize;
                break;
            case detail::EventOperationKind::feedback_append:
                kind = RootOperationKind::event_feedback_append;
                break;
            }
            result.push_back({.kind = kind, .index = operation.index});
        }
        return {};
    }

    template<class Prepare>
    Checked append_sample_operations(
        std::span<std::size_t const> operations,
        RootOperationKind kind,
        Prepare prepare,
        std::vector<RootOperation>& result)
    {
        for (auto const operation : operations) {
            if (auto valid = prepare(operation); !valid) return valid;
            result.push_back({.kind = kind, .index = operation});
        }
        return {};
    }

    Checked build_steps()
    {
        auto const& steps = plan_.execution.primitive_steps;
        auto const& primitive_storage = lowering_.declarations.primitive_storage;
        auto const area = plan_.binding_offset;
        auto carry = [&](std::size_t index) { return prepare_sample_carry(index); };
        auto materialization = [&](std::size_t index) {
            return prepare_materialization(index);
        };
        auto composition = [&](std::size_t index) { return prepare_composition(index); };
        auto feedback = [&](std::size_t index) { return prepare_feedback_write(index); };
        plan_.steps.resize(steps.size());
        for (std::size_t index = 0; index < steps.size(); ++index) {
            auto const& step = steps[index];
            if (step.configuration_index >= plan_.primitives.size()) {
                return invalid("GraphJit execution plan references a missing node configuration");
            }
            if (step.storage_index >= primitive_storage.size()) {
                return invalid(
                    "GraphJit execution plan references a missing canonical storage plan");
            }
            if (step.maximum_block_size == 0) {
                return invalid(
                    "GraphJit execution plan contains an invalid primitive maximum block size");
            }
            if (auto valid = prepare_executed_primitive(step.configuration_index);
                !valid) {
                return valid;
            }
            auto const& primitive = plan_.primitives[step.configuration_index];

            // The primitive's State view completes its context.
            auto const& state = primitive_storage[step.storage_index];
            if (state.has_state) {
                auto const span_offset = primitive.context_offset
                    + offsetof(ReflectedNodeTickContext, state);
                write_template(
                    span_offset - area + offsetof(ReflectedSpan<std::byte>, extent),
                    state.state_size);
                add_pointer(
                    span_offset + offsetof(ReflectedSpan<std::byte>, pointer),
                    BindingPointerBase::node_storage,
                    0,
                    state.state_offset);
            }

            auto& root_step = plan_.steps[index];
            root_step.primitive = step.configuration_index;
            root_step.maximum_block_size = step.maximum_block_size;
            root_step.captures = primitive.tick
                && (!primitive.tick->sample_capture_outputs.empty()
                    || !primitive.tick->event_capture_outputs.empty());
            auto& before = root_step.before;
            auto& after = root_step.after;
            // The order of PrimitiveExecutionStep: event operations, then the
            // sample operations of each phase.
            if (auto valid = append_event_operations(step.event_operations_before, before);
                !valid) {
                return valid;
            }
            if (auto valid = append_sample_operations(
                    step.sample_carry_restores_before,
                    RootOperationKind::sample_carry_restore, carry, before);
                !valid) {
                return valid;
            }
            if (auto valid = append_sample_operations(
                    step.sample_materializations_before,
                    RootOperationKind::sample_materialization, materialization, before);
                !valid) {
                return valid;
            }
            if (auto valid = append_sample_operations(
                    step.sample_compositions_before,
                    RootOperationKind::sample_composition, composition, before);
                !valid) {
                return valid;
            }
            if (auto valid = append_sample_operations(
                    step.sample_transition_materializations_before,
                    RootOperationKind::sample_materialization, materialization, before);
                !valid) {
                return valid;
            }
            if (auto valid = append_event_operations(step.event_operations_after, after);
                !valid) {
                return valid;
            }
            if (auto valid = append_sample_operations(
                    step.sample_materializations_after,
                    RootOperationKind::sample_materialization, materialization, after);
                !valid) {
                return valid;
            }
            if (auto valid = append_sample_operations(
                    step.sample_feedback_writes_after,
                    RootOperationKind::sample_feedback_write, feedback, after);
                !valid) {
                return valid;
            }
            if (auto valid = append_sample_operations(
                    step.sample_compositions_after,
                    RootOperationKind::sample_composition, composition, after);
                !valid) {
                return valid;
            }
            if (auto valid = append_sample_operations(
                    step.sample_transition_materializations_after,
                    RootOperationKind::sample_materialization, materialization, after);
                !valid) {
                return valid;
            }
            if (auto valid = append_sample_operations(
                    step.sample_carry_commits_after,
                    RootOperationKind::sample_carry_commit, carry, after);
                !valid) {
                return valid;
            }
        }
        return {};
    }

    Checked build_groups()
    {
        auto const& execution = plan_.execution;
        plan_.regions.resize(execution.regions.size());
        for (auto const& group : execution.ready_groups) {
            if (!group.cyclic_region) {
                for (auto const step : group.primitive_steps) {
                    if (step >= execution.primitive_steps.size()) {
                        return invalid("GraphJit ready group references a missing primitive step");
                    }
                }
                if (group.primitive_steps.size() > 1) {
                    if (auto valid = validate_batch(group); !valid) return valid;
                    plan_.maximum_batch_lanes = std::max(
                        plan_.maximum_batch_lanes, group.primitive_steps.size());
                }
                continue;
            }
            if (*group.cyclic_region >= execution.regions.size()) {
                return invalid("GraphJit ready group references a missing cyclic region");
            }
            auto const& region = execution.regions[*group.cyclic_region];
            if (!region.cyclic || !group.primitive_steps.empty()) {
                return invalid("GraphJit cyclic ready group has invalid execution contents");
            }
            if (region.maximum_block_size == 0 || region.scc_feedback_latency == 0) {
                return invalid("GraphJit cyclic execution region has invalid slice semantics");
            }
            for (auto const step : region.primitive_steps) {
                if (step >= execution.primitive_steps.size()) {
                    return invalid("GraphJit cyclic region references a missing primitive step");
                }
                if (execution.primitive_steps[step].maximum_block_size
                    < region.maximum_block_size) {
                    return invalid(
                        "GraphJit SCC quantum exceeds a primitive maximum block size");
                }
            }
            auto& operations = plan_.regions[*group.cyclic_region];
            if (auto valid = append_event_operations(
                    region.event_operations_before, operations.before);
                !valid) {
                return valid;
            }
            if (auto valid = append_event_operations(
                    region.event_operations_after, operations.after);
                !valid) {
                return valid;
            }
        }
        return {};
    }

    // Planning admits a multi-step ready group only when it can run as one
    // native batch; see realtime_ready_group_batches().
    Checked validate_batch(detail::ExecutionReadyGroupPlan const& group) const
    {
        auto const& steps = plan_.execution.primitive_steps;
        details::NodeCompilerOperations const* first = nullptr;
        auto has_activity = false;
        for (auto const step_index : group.primitive_steps) {
            auto const& step = steps[step_index];
            auto const& primitive = plan_.primitives[step.configuration_index];
            auto const& callbacks = primitive.callbacks;
            if (!first) first = &callbacks;
            has_activity = has_activity
                || plan_.primitive_activity[step.configuration_index].has_value();
            if (!callbacks.tick_block_batch
                || callbacks.tick_block_batch != first->tick_block_batch
                || callbacks.skip_block_batch != first->skip_block_batch) {
                return invalid("GraphJit realtime batch contains incompatible native callbacks");
            }
            if (step.maximum_block_size < lowering_.declarations.node_layout.max_block_size
                || !step.sample_carry_restores_before.empty()
                || !step.sample_materializations_before.empty()
                || !step.sample_compositions_before.empty()
                || !step.sample_transition_materializations_before.empty()
                || !step.sample_feedback_writes_after.empty()
                || !step.sample_materializations_after.empty()
                || !step.sample_compositions_after.empty()
                || !step.sample_transition_materializations_after.empty()
                || !step.sample_carry_commits_after.empty()
                || !step.event_operations_before.empty()
                || !step.event_operations_after.empty()
                || (primitive.tick
                    && (!primitive.tick->sample_capture_outputs.empty()
                        || !primitive.tick->event_capture_outputs.empty()))) {
                return invalid("GraphJit realtime batch lane needs scalar execution");
            }
        }
        if (has_activity && !first->skip_block_batch) {
            return invalid("GraphJit realtime batch has no skip batch callback");
        }
        return {};
    }

    // --- frame layout -----------------------------------------------------

    Checked build_layout()
    {
        auto& layout = plan_.frame;
        plan_.scratch_alignment = std::max(plan_.root_stack.alignment, binding_alignment_);
        plan_.scratch_size = plan_.binding_offset + plan_.binding_template.size();
        auto cursor = plan_.scratch_size;
        auto take = [&](std::size_t count, std::size_t size, std::size_t alignment) {
            plan_.scratch_alignment = std::max(plan_.scratch_alignment, alignment);
            cursor = align_up(cursor, alignment);
            auto const offset = cursor;
            cursor += count * size;
            return offset;
        };
        auto const& storage = plan_.sample_ports.storage;
        layout.sample_representations = take(
            storage.representations.size(), sizeof(std::byte*), alignof(std::byte*));
        layout.event_representations = take(
            plan_.event_ports.ports.size(), sizeof(std::byte*), alignof(std::byte*));
        layout.authored_extents = take(
            storage.producer_authored_extents.size(),
            sizeof(std::size_t),
            alignof(std::size_t));
        layout.pace_cursors = take(
            storage.sequential_pace_cursors.size(),
            sizeof(std::uint64_t),
            alignof(std::uint64_t));
        layout.sample_epochs = take(
            plan_.tracks_activity ? storage.representations.size() : 0,
            sizeof(std::size_t),
            alignof(std::size_t));
        layout.event_epochs = take(
            plan_.tracks_activity ? plan_.event_ports.ports.size() : 0,
            sizeof(std::size_t),
            alignof(std::size_t));
        layout.continuous_summaries = take(
            plan_.tracks_activity ? plan_.activity_frontiers.size() : 0,
            sizeof(ContinuousActivitySummary),
            alignof(ContinuousActivitySummary));
        layout.event_summaries = take(
            plan_.tracks_activity ? plan_.event_ports.ports.size() : 0,
            sizeof(EventActivitySummary),
            alignof(EventActivitySummary));
        layout.tick_lanes = take(
            plan_.maximum_batch_lanes,
            sizeof(ReflectedNodeTickBatchLane),
            alignof(ReflectedNodeTickBatchLane));
        layout.skip_lanes = take(
            plan_.tracks_activity ? plan_.maximum_batch_lanes : 0,
            sizeof(ReflectedNodeTickBatchLane),
            alignof(ReflectedNodeTickBatchLane));
        layout.tick_decisions = take(
            plan_.tracks_activity ? plan_.maximum_batch_lanes : 0,
            sizeof(PrimitiveActivityDecision),
            alignof(PrimitiveActivityDecision));
        layout.tick_lane_primitives = take(
            plan_.tracks_activity ? plan_.maximum_batch_lanes : 0,
            sizeof(std::size_t),
            alignof(std::size_t));
        layout.size = cursor;
        if (!std::has_single_bit(plan_.scratch_alignment)) {
            return invalid("GraphJit realtime root scratch alignment is not a power of two");
        }
        return {};
    }
};
} // namespace

std::expected<std::shared_ptr<RealtimeRootPlan const>, std::string>
build_realtime_root_plan(LoweringInput const& input, detail::LoweringPlan& plan)
{
    auto root = std::make_shared<RealtimeRootPlan>();
    root->sample_ports = std::move(plan.sample_ports);
    root->event_ports = std::move(plan.event_ports);
    root->execution = std::move(plan.execution);
    root->root_stack = plan.root_stack;
    root->primitive_activity = std::move(plan.declarations.primitive_activity);
    root->activity_frontiers = std::move(plan.declarations.activity_frontiers);
    if (auto built = RootPlanBuilder(input, plan, *root).build(); !built) {
        return std::unexpected(std::move(built.error()));
    }
    return std::shared_ptr<RealtimeRootPlan const>(std::move(root));
}

void execute_realtime_root(
    RealtimeRootPlan const* root,
    std::byte* storage_base,
    TickInvocationCall const* invocation,
    GlobalIndex sample_index,
    std::size_t block_size) noexcept
{
    auto const& plan = *root;
    auto const& layout = plan.frame;
    auto const& storage = plan.sample_ports.storage;
    auto const activity_sample_count = plan.tracks_activity
        ? storage.representations.size()
        : std::size_t{0};
    auto const activity_event_count = plan.tracks_activity
        ? plan.event_ports.ports.size()
        : std::size_t{0};
    auto const activity_frontier_count = plan.tracks_activity
        ? plan.activity_frontiers.size()
        : std::size_t{0};
    auto const activity_batch_count = plan.tracks_activity
        ? plan.maximum_batch_lanes
        : std::size_t{0};
    IV_ASSERT(invocation != nullptr || !plan.has_tick_slots,
        "GraphJit Tick runtime slots require an invocation record");
    auto* allocation = static_cast<std::byte*>(
        alloca(layout.size + plan.scratch_alignment - 1));
    auto* scratch = reinterpret_cast<std::byte*>(align_up(
        reinterpret_cast<std::uintptr_t>(allocation), plan.scratch_alignment));

    RootFrame frame{
        .plan = plan,
        .storage_base = storage_base,
        .scratch = scratch,
        .sample_representations = frame_array<std::byte*>(
            scratch, layout.sample_representations, storage.representations.size()),
        .event_representations = frame_array<std::byte*>(
            scratch, layout.event_representations, plan.event_ports.ports.size()),
        .producer_authored_extents = frame_array<std::size_t>(
            scratch, layout.authored_extents, storage.producer_authored_extents.size()),
        .sequential_pace_cursors = frame_array<std::uint64_t>(
            scratch, layout.pace_cursors, storage.sequential_pace_cursors.size()),
        .sample_representation_epochs = frame_array<std::size_t>(
            scratch, layout.sample_epochs, activity_sample_count),
        .event_representation_epochs = frame_array<std::size_t>(
            scratch, layout.event_epochs, activity_event_count),
        .continuous_activity_summaries = frame_array<ContinuousActivitySummary>(
            scratch, layout.continuous_summaries, activity_frontier_count),
        .event_activity_summaries = frame_array<EventActivitySummary>(
            scratch, layout.event_summaries, activity_event_count),
        .tick_lanes = frame_array<ReflectedNodeTickBatchLane>(
            scratch, layout.tick_lanes, plan.maximum_batch_lanes),
        .skip_lanes = frame_array<ReflectedNodeTickBatchLane>(
            scratch, layout.skip_lanes, activity_batch_count),
        .tick_decisions = frame_array<PrimitiveActivityDecision>(
            scratch, layout.tick_decisions, activity_batch_count),
        .tick_lane_primitives = frame_array<std::size_t>(
            scratch, layout.tick_lane_primitives, activity_batch_count),
        .invocation = invocation,
        .root_index = sample_index,
    };
    std::ranges::fill(frame.sample_representation_epochs, 0);
    std::ranges::fill(frame.event_representation_epochs, 0);
    std::ranges::fill(frame.continuous_activity_summaries, ContinuousActivitySummary{});
    std::ranges::fill(frame.event_activity_summaries, EventActivitySummary{});
    for (std::size_t index = 0; index < plan.sample_addresses.size(); ++index) {
        frame.sample_representations[index] = resolve_address(
            plan.sample_addresses[index], storage_base, scratch);
    }
    for (std::size_t index = 0; index < plan.event_addresses.size(); ++index) {
        frame.event_representations[index] = resolve_address(
            plan.event_addresses[index], storage_base, scratch);
    }
    for (std::size_t index = 0; index < storage.producer_authored_extents.size(); ++index) {
        frame.producer_authored_extents[index] = load_raw<std::size_t>(
            storage_base, storage.producer_authored_extents[index].storage_offset);
    }
    for (std::size_t index = 0; index < storage.sequential_pace_cursors.size(); ++index) {
        frame.sequential_pace_cursors[index] = load_raw<std::uint64_t>(
            storage_base, storage.sequential_pace_cursors[index].storage_offset);
    }
    initialize_bindings(frame);
    initialize_tick_slots(frame);

    auto const& execution = plan.execution;
    for (auto const& group : execution.ready_groups) {
        if (!group.cyclic_region) {
            if (group.primitive_steps.size() > 1) {
                run_batch(frame, group, sample_index, block_size);
                continue;
            }
            for (auto const step : group.primitive_steps) {
                run_step(frame, step, sample_index, block_size, true);
            }
            continue;
        }
        auto const& region = execution.regions[*group.cyclic_region];
        auto const& operations = plan.regions[*group.cyclic_region];
        run_operations(frame, operations.before, sample_index, block_size);
        for (std::size_t offset = 0; offset < block_size;
             offset += region.maximum_block_size) {
            auto const slice = sample_index.advanced_by(offset);
            auto const count = std::min(region.maximum_block_size, block_size - offset);
            for (auto const step : region.primitive_steps) {
                run_step(frame, step, slice, count, false);
            }
        }
        run_operations(frame, operations.after, sample_index, block_size);
    }

    for (std::size_t index = 0; index < storage.producer_authored_extents.size(); ++index) {
        auto const& extent = storage.producer_authored_extents[index];
        auto const current = std::min(
            frame.producer_authored_extents[index], extent.maximum_revision_frames);
        auto const projected = project_lattice(
            sample_index, block_size, extent.absolute_pace_exponent).value_count;
        store_raw<std::size_t>(
            storage_base,
            extent.storage_offset,
            current + std::min(projected, extent.maximum_revision_frames - current));
    }
    for (std::size_t index = 0; index < storage.sequential_pace_cursors.size(); ++index) {
        auto const& cursor = storage.sequential_pace_cursors[index];
        store_raw<std::uint64_t>(
            storage_base,
            cursor.storage_offset,
            frame.sequential_pace_cursors[index]
                + project_lattice(
                    sample_index, block_size, cursor.absolute_pace_exponent)
                    .value_count);
    }
}
} // namespace iv::graph_jit
