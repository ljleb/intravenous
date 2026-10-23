#pragma once

// Compiler-selected operations and binding records for one concrete node.

#include <intravenous/node/compiler_record.h>
#include <intravenous/node/continuous_port.h>
#include <intravenous/node/coverage_port_context.h>
#include <intravenous/node/port_constraints.h>
#include <intravenous/ports.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>

namespace iv {

// Compiler-facing span ABI. Whole-project LLVM may materialize these fields
// directly, so their object representation is explicit rather than inheriting
// an implementation-defined std::span layout. The implicit conversion keeps
// the reflected adapter compatible with the ordinary TickContext interface.
template<typename T>
struct ReflectedSpan {
    T* pointer = nullptr;
    std::size_t extent = 0;

    constexpr ReflectedSpan() noexcept = default;
    constexpr ReflectedSpan(T* pointer, std::size_t extent) noexcept
        : pointer(pointer)
        , extent(extent)
    {}
    constexpr ReflectedSpan(std::span<T> value) noexcept
        : pointer(value.data())
        , extent(value.size())
    {}

    template<typename Range>
        requires requires(Range&& range) {
            std::span<T>(std::forward<Range>(range));
        }
    constexpr ReflectedSpan(Range&& range)
        noexcept(noexcept(std::span<T>(std::forward<Range>(range))))
        : ReflectedSpan(std::span<T>(std::forward<Range>(range)))
    {}

    [[nodiscard]] constexpr T* data() const noexcept { return pointer; }
    [[nodiscard]] constexpr std::size_t size() const noexcept { return extent; }
    [[nodiscard]] constexpr bool empty() const noexcept { return extent == 0; }

    constexpr operator std::span<T>() const noexcept
    {
        return {pointer, extent};
    }
};

static_assert(std::is_standard_layout_v<ReflectedSpan<std::byte>>);
static_assert(std::is_trivially_copyable_v<ReflectedSpan<std::byte>>);

using ReflectedContinuousChannelStorageBinding =
    ContinuousChannelStorageBinding;
using ReflectedContinuousPortStorageBinding = ContinuousPortStorageBinding;
using ReflectedContinuousInputPortBinding = ContinuousInputPortBinding;

struct ReflectedContinuousOutputPortBinding {
    ReflectedContinuousPortStorageBinding storage {};
    std::size_t history = 0;
    std::size_t latency = 0;
    std::int32_t absolute_pace_exponent = 0;
    // Physical invocation-order ordinal selected by generated execution for
    // this callback. The node-facing coordinate remains the projected global
    // lattice coordinate carried separately by the Tick context.
    mutable std::uint64_t sequential_begin_ordinal = 0;
    // For Tick/persisted outputs only, the imported primitive wrapper resets
    // this before each node invocation and passes its address only to that
    // output's statically specialized facade. Ephemeral/Tock output facades
    // carry no disposition state. The generated post-step operation observes
    // the finalized value.
    mutable OutputDisposition disposition = OutputDisposition::untouched;
};

struct ReflectedContinuousPortRealization {
    ContinuousValueStorageGeometry geometry{};
    std::size_t sample_support = 0;
    bool has_sample_support = false;
    std::int32_t local_pace_exponent = 0;
    std::int32_t absolute_pace_exponent = 0;
    std::size_t history = 0;
    std::size_t latency = 0;
    CoverageOrdering ordering = CoverageOrdering::unspecified;
};

struct ReflectedNodePortRealization {
    ReflectedSpan<ReflectedContinuousPortRealization const> inputs{};
    ReflectedSpan<ReflectedContinuousPortRealization const> outputs{};
};

// Fixed compiler-owned event storage binding. Ordinary bounded sequences
// store one event-count word followed by a TimedEvent array. Persistent event
// rings instead store monotonic read/write indices followed by the same bounded
// power-of-two TimedEvent array. EventInputPort and EventOutputPort remain
// invocation-local facades reconstructed by the imported primitive wrapper; no
// facade/cursor objects persist in NodeStorage.
static_assert(std::is_trivially_copyable_v<TimedEvent>,
    "GraphJit raw event storage requires TimedEvent to remain byte-storable");

struct ReflectedEventPortStorageBinding {
    // Already-resolved representation base. Field offsets below are relative
    // to this one bounded sequence/ring allocation.
    std::byte* storage = nullptr;
    std::size_t count_offset = 0;
    std::size_t read_index_offset = 0;
    std::size_t write_index_offset = 0;
    std::size_t events_offset = 0;
    std::size_t event_capacity = 0;
    EventTypeId type = EventTypeId::empty;
    bool persistent_ring = false;
};

struct ReflectedEventInputPortBinding {
    ReflectedEventPortStorageBinding storage {};
};

struct ReflectedEventOutputPortBinding {
    ReflectedEventPortStorageBinding storage {};
    // Per-logical-output producer overflow telemetry. Derived event
    // representations never allocate or bind their own copy of this counter.
    std::uint64_t* overflow_count = nullptr;
    EventTypeId source_type = EventTypeId::empty;
    std::size_t history = 0;
    std::size_t latency = 0;
    // An unretained acyclic output can use this sequence as its complete
    // root-call aggregate. If that primitive is sliced, every slice appends.
    // Retained and cyclic outputs instead use one callback-local sequence per
    // invocation and compiler operations accumulate it into canonical storage.
    bool append_existing = false;
    // Invocation-local authored state whose address is given only to the
    // Tick/persisted facade and its generated post-step operation.
    mutable OutputDisposition disposition = OutputDisposition::untouched;
};

static_assert(std::is_standard_layout_v<ReflectedContinuousChannelStorageBinding>);
static_assert(std::is_trivially_copyable_v<ReflectedContinuousChannelStorageBinding>);
static_assert(std::is_standard_layout_v<ReflectedContinuousPortStorageBinding>);
static_assert(std::is_trivially_copyable_v<ReflectedContinuousPortStorageBinding>);
static_assert(
    sizeof(std::array<
        ReflectedContinuousChannelStorageBinding,
        maximum_supported_channel_count>)
    == sizeof(ReflectedContinuousChannelStorageBinding)
        * maximum_supported_channel_count);
static_assert(std::is_standard_layout_v<ReflectedContinuousInputPortBinding>);
static_assert(std::is_trivially_copyable_v<ReflectedContinuousInputPortBinding>);
static_assert(std::is_standard_layout_v<ReflectedContinuousOutputPortBinding>);
static_assert(std::is_trivially_copyable_v<ReflectedContinuousOutputPortBinding>);
static_assert(std::is_standard_layout_v<ReflectedEventPortStorageBinding>);
static_assert(std::is_trivially_copyable_v<ReflectedEventPortStorageBinding>);
static_assert(std::is_standard_layout_v<ReflectedEventInputPortBinding>);
static_assert(std::is_trivially_copyable_v<ReflectedEventInputPortBinding>);
static_assert(std::is_standard_layout_v<ReflectedEventOutputPortBinding>);
static_assert(std::is_trivially_copyable_v<ReflectedEventOutputPortBinding>);

struct ReflectedNodeTickContext {
    // Whole-project sample bindings are compiler records resolved once in the
    // generated root frame. Imported
    // primitive wrappers reconstruct short-lived InputPort/OutputPort values
    // from these bindings and the current absolute sample index. This avoids
    // persistent façade/cursor state and makes implementation constants visible
    // to whole-project O3 after inlining.
    ReflectedSpan<ReflectedContinuousInputPortBinding const> continuous_input_bindings {};
    // Output spans are compact realtime-only indices. Background outputs are
    // bound only by background callback contexts.
    ReflectedSpan<ReflectedContinuousOutputPortBinding const> continuous_output_bindings {};

    // GraphJit event bindings mirror the sample binding architecture.
    ReflectedSpan<ReflectedEventInputPortBinding const> event_input_bindings {};
    // Like sample outputs, this span contains realtime outputs only.
    ReflectedSpan<ReflectedEventOutputPortBinding const> event_output_bindings {};
    ReflectedSpan<RandomAccessSampleInputPort const> random_access_inputs {};
    ReflectedSpan<RandomAccessEventInputPort const> random_access_event_inputs {};
    std::size_t sample_rate = 48000;
    std::size_t scc_feedback_latency = 0;
    ReflectedSpan<std::byte> state {};
};

static_assert(std::is_standard_layout_v<ReflectedNodeTickContext>);
static_assert(std::is_trivially_copyable_v<ReflectedNodeTickContext>);

// Stable compiler-facing background callback contexts. These mirror the public
// Node-specialized contexts without exposing std::span's implementation-defined
// representation to package LLVM or whole-graph lowering.
struct ReflectedNodeTockCoverageContext {
    ReflectedSpan<RandomAccessSampleInputPort const> inputs {};
    ReflectedSpan<TockSampleOutputPort> outputs {};
    ReflectedSpan<RandomAccessEventInputPort const> event_inputs {};
    ReflectedSpan<TockEventOutputPort> event_outputs {};
    ReflectedSpan<std::byte> background_state_storage {};
    std::size_t sample_rate = 48000;
};

struct ReflectedNodeForwardCoverageContext {
    ReflectedSpan<InputCoverageChange const> inputs {};
    ReflectedSpan<OutputCoverageChange> outputs {};
    ReflectedSpan<InputCoverageChange const> event_inputs {};
    ReflectedSpan<OutputCoverageChange> event_outputs {};
    bool local_state_changed = false;
    std::size_t sample_rate = 48000;
};

struct ReflectedNodeReverseCoverageContext {
    ReflectedSpan<InputCoverageRequirement> inputs {};
    ReflectedSpan<OutputCoverageRequirement const> outputs {};
    ReflectedSpan<InputCoverageRequirement> event_inputs {};
    ReflectedSpan<OutputCoverageRequirement const> event_outputs {};
    std::size_t sample_rate = 48000;
};

// Compact compiler-facing batch records. These carry only one configured node
// pointer and its scalar reflected context per lane. The package wrapper
// reconstructs the public range and lane proxies; whole-project LLVM never
// depends on a C++ batch-context or iterator representation.
struct ReflectedNodeTickBatchLane {
    void const* node_data = nullptr;
    ReflectedNodeTickContext context {};
    GlobalIndex index {};
    std::size_t block_size = 0;
};

struct ReflectedNodeTickBatchContext {
    ReflectedSpan<ReflectedNodeTickBatchLane const> lanes {};
};

struct ReflectedNodeTockCoverageBatchLane {
    void const* node_data = nullptr;
    ReflectedNodeTockCoverageContext context {};
};

struct ReflectedNodeTockCoverageBatchContext {
    ReflectedSpan<ReflectedNodeTockCoverageBatchLane const> lanes {};
};

struct ReflectedNodeForwardCoverageBatchLane {
    void const* node_data = nullptr;
    ReflectedNodeForwardCoverageContext context {};
};

struct ReflectedNodeForwardCoverageBatchContext {
    ReflectedSpan<ReflectedNodeForwardCoverageBatchLane const> lanes {};
};

struct ReflectedNodeReverseCoverageBatchLane {
    void const* node_data = nullptr;
    ReflectedNodeReverseCoverageContext context {};
};

struct ReflectedNodeReverseCoverageBatchContext {
    ReflectedSpan<ReflectedNodeReverseCoverageBatchLane const> lanes {};
};

static_assert(std::is_standard_layout_v<ReflectedNodeTockCoverageContext>);
static_assert(std::is_trivially_copyable_v<ReflectedNodeTockCoverageContext>);
static_assert(std::is_standard_layout_v<ReflectedNodeForwardCoverageContext>);
static_assert(std::is_trivially_copyable_v<ReflectedNodeForwardCoverageContext>);
static_assert(std::is_standard_layout_v<ReflectedNodeReverseCoverageContext>);
static_assert(std::is_trivially_copyable_v<ReflectedNodeReverseCoverageContext>);
static_assert(std::is_standard_layout_v<ReflectedNodeTickBatchLane>);
static_assert(std::is_trivially_copyable_v<ReflectedNodeTickBatchLane>);
static_assert(std::is_standard_layout_v<ReflectedNodeTickBatchContext>);
static_assert(std::is_trivially_copyable_v<ReflectedNodeTickBatchContext>);
static_assert(std::is_standard_layout_v<ReflectedNodeTockCoverageBatchLane>);
static_assert(std::is_trivially_copyable_v<ReflectedNodeTockCoverageBatchLane>);
static_assert(std::is_standard_layout_v<ReflectedNodeTockCoverageBatchContext>);
static_assert(std::is_trivially_copyable_v<ReflectedNodeTockCoverageBatchContext>);
static_assert(std::is_standard_layout_v<ReflectedNodeForwardCoverageBatchLane>);
static_assert(std::is_trivially_copyable_v<ReflectedNodeForwardCoverageBatchLane>);
static_assert(std::is_standard_layout_v<ReflectedNodeForwardCoverageBatchContext>);
static_assert(std::is_trivially_copyable_v<ReflectedNodeForwardCoverageBatchContext>);
static_assert(std::is_standard_layout_v<ReflectedNodeReverseCoverageBatchLane>);
static_assert(std::is_trivially_copyable_v<ReflectedNodeReverseCoverageBatchLane>);
static_assert(std::is_standard_layout_v<ReflectedNodeReverseCoverageBatchContext>);
static_assert(std::is_trivially_copyable_v<ReflectedNodeReverseCoverageBatchContext>);
static_assert(offsetof(ReflectedNodeTockCoverageBatchLane, node_data)
    == offsetof(ReflectedNodeForwardCoverageBatchLane, node_data));
static_assert(offsetof(ReflectedNodeTockCoverageBatchLane, node_data)
    == offsetof(ReflectedNodeReverseCoverageBatchLane, node_data));
static_assert(offsetof(ReflectedNodeTockCoverageBatchLane, context)
        + sizeof(ReflectedNodeTockCoverageContext)
    == sizeof(ReflectedNodeTockCoverageBatchLane));
static_assert(offsetof(ReflectedNodeForwardCoverageBatchLane, context)
        + sizeof(ReflectedNodeForwardCoverageContext)
    == sizeof(ReflectedNodeForwardCoverageBatchLane));
static_assert(offsetof(ReflectedNodeReverseCoverageBatchLane, context)
        + sizeof(ReflectedNodeReverseCoverageContext)
    == sizeof(ReflectedNodeReverseCoverageBatchLane));
static_assert(offsetof(ReflectedNodeTockCoverageBatchContext, lanes)
    == offsetof(ReflectedNodeForwardCoverageBatchContext, lanes));
static_assert(offsetof(ReflectedNodeTockCoverageBatchContext, lanes)
    == offsetof(ReflectedNodeReverseCoverageBatchContext, lanes));
static_assert(sizeof(ReflectedNodeTockCoverageBatchContext)
    == sizeof(ReflectedNodeForwardCoverageBatchContext));
static_assert(sizeof(ReflectedNodeTockCoverageBatchContext)
    == sizeof(ReflectedNodeReverseCoverageBatchContext));

struct ReflectedNodeRuntimeOperations {
    void const* node_data = nullptr;
    NodeStateStructures const* state_structures = nullptr;
    std::size_t (*declare_node)(
        void const*,
        NodeStateStructures const*,
        NodeLayoutBuilder&,
        ReflectedNodePortRealization const&) = nullptr;
    void (*tick_block)(
        void const*, ReflectedNodeTickContext const&, GlobalIndex, std::size_t) = nullptr;
    void (*skip_block)(
        void const*, ReflectedNodeTickContext const&, GlobalIndex, std::size_t) = nullptr;

    constexpr bool valid() const
    {
        // Tock-only primitives retain declaration/configuration operations in
        // the configured graph but have no realtime callbacks. The complete
        // compiler record validates that their Tock family is present.
        return declare_node != nullptr
            && (tick_block != nullptr) == (skip_block != nullptr);
    }
};

struct ReflectedNodeOperations {
    ReflectedNodeRuntimeOperations runtime {};

    constexpr bool valid() const
    {
        return runtime.valid();
    }
};

namespace details {

constexpr ReflectedNodeRuntimeOperations make_runtime_operations(
    NodeCompilerRecord const& record,
    void const* node_data,
    NodeStateStructures const* state_structures = nullptr)
{
    return {
        .node_data = node_data,
        .state_structures = state_structures,
        .declare_node = record.operations.declare_node,
        .tick_block = record.operations.tick_block,
        .skip_block = record.operations.skip_block,
    };
}

} // namespace details
} // namespace iv
