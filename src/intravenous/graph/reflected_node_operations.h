#pragma once

// Compiler-selected operations and binding records for one concrete node.

#include <intravenous/node/compiler_record.h>
#include <intravenous/node/coverage_port_context.h>
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

struct ReflectedSampleChannelStorageBinding {
    // Resolved address of logical frame zero for one semantic channel.
    // frame_stride is measured in Sample elements, so planar channels normally
    // use stride 1 while an interleaved N-channel representation uses stride N.
    // Different channels may name unrelated producer representations.
    std::byte* storage = nullptr;
    std::size_t frame_capacity = 0;
    std::size_t frame_stride = 1;
    // Additional logical delay between the port timeline and this backing
    // channel. Zero preserves the ordinary whole-representation mapping; a
    // future channel alias may bind an independently delayed producer channel.
    std::size_t frame_delay = 0;
};

struct ReflectedSamplePortStorageBinding {
    // Channel-granular resolved backing. The first channel_count(channel_layout)
    // entries are active; unused entries remain null. This deliberately avoids
    // requiring one contiguous representation base for a logical sample port.
    std::array<
        ReflectedSampleChannelStorageBinding,
        maximum_supported_channel_count> channels{};
    std::size_t frame_capacity = 0;
    std::size_t storage_latency = 0;
    ChannelLayout channel_layout {
        .channel_type = ChannelTypeId::mono,
        .sample_layout = SampleStreamLayout::planar,
    };
};

struct ReflectedSampleInputPortBinding {
    ReflectedSamplePortStorageBinding storage {};
    std::size_t history = 0;
    std::size_t read_latency = 0;
};

struct ReflectedSampleOutputPortBinding {
    ReflectedSamplePortStorageBinding storage {};
    std::size_t history = 0;
    std::size_t latency = 0;
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
    // Direct flow owns one primitive invocation, so the wrapper may clear the
    // sequence when reconstructing the output facade. Materialized flow can
    // span several primitive slices; in that case lowering clears the raw
    // sequence once before the producer step and every slice appends.
    bool append_existing = false;
};

static_assert(std::is_standard_layout_v<ReflectedSampleChannelStorageBinding>);
static_assert(std::is_trivially_copyable_v<ReflectedSampleChannelStorageBinding>);
static_assert(std::is_standard_layout_v<ReflectedSamplePortStorageBinding>);
static_assert(std::is_trivially_copyable_v<ReflectedSamplePortStorageBinding>);
static_assert(
    sizeof(std::array<
        ReflectedSampleChannelStorageBinding,
        maximum_supported_channel_count>)
    == sizeof(ReflectedSampleChannelStorageBinding)
        * maximum_supported_channel_count);
static_assert(std::is_standard_layout_v<ReflectedSampleInputPortBinding>);
static_assert(std::is_trivially_copyable_v<ReflectedSampleInputPortBinding>);
static_assert(std::is_standard_layout_v<ReflectedSampleOutputPortBinding>);
static_assert(std::is_trivially_copyable_v<ReflectedSampleOutputPortBinding>);
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
    ReflectedSpan<ReflectedSampleInputPortBinding const> sample_input_bindings {};
    // Output spans are compact realtime-only indices. Background outputs are
    // bound only by background callback contexts.
    ReflectedSpan<ReflectedSampleOutputPortBinding const> sample_output_bindings {};

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

static_assert(std::is_standard_layout_v<ReflectedNodeTockCoverageContext>);
static_assert(std::is_trivially_copyable_v<ReflectedNodeTockCoverageContext>);
static_assert(std::is_standard_layout_v<ReflectedNodeForwardCoverageContext>);
static_assert(std::is_trivially_copyable_v<ReflectedNodeForwardCoverageContext>);
static_assert(std::is_standard_layout_v<ReflectedNodeReverseCoverageContext>);
static_assert(std::is_trivially_copyable_v<ReflectedNodeReverseCoverageContext>);

struct ReflectedNodeRuntimeOperations {
    void const* node_data = nullptr;
    NodeStateStructures const* state_structures = nullptr;
    std::size_t (*declare_node)(
        void const*, NodeStateStructures const*, NodeLayoutBuilder&) = nullptr;
    void (*tick_block)(
        void const*, ReflectedNodeTickContext const&, std::size_t, std::size_t) = nullptr;
    void (*skip_block)(
        void const*, ReflectedNodeTickContext const&, std::size_t, std::size_t) = nullptr;

    constexpr bool valid() const
    {
        return declare_node != nullptr && tick_block != nullptr
            && skip_block != nullptr;
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
