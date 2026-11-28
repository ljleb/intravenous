#pragma once

#include <intravenous/global_index.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace iv::graph_jit {
struct LoweringInput;
struct TickInvocationCall;
class RegionSpecializationProvider;
namespace detail {
struct LoweringPlan;
}

// Immutable graph-dependent data of one realtime root realization. The root's
// code, execute_realtime_root(), is part of the application binary, so a graph
// edit builds a new RealtimeRootPlan and compiles no realtime code. The
// definition is shared privately by construction and execution.
struct RealtimeRootPlan;

// Builds the realtime root that executes plan. The root takes ownership of the
// plan sections that only it reads: sample_ports, event_ports, execution,
// root_stack, declarations.primitive_activity, and
// declarations.activity_frontiers. Every other section stays in plan for the
// generated background roots and LoweringOutput. Every plan invariant the root
// relies on is checked here, so a root call checks no plan structure. With
// specializations, each cyclic region it compiles code for runs that code
// instead of the interpreter's slice loop.
std::expected<std::shared_ptr<RealtimeRootPlan const>, std::string>
build_realtime_root_plan(
    LoweringInput const& input,
    detail::LoweringPlan& plan,
    RegionSpecializationProvider* specializations = nullptr);

// Executes one root block of plan, which is never null. block_size > 0. If
// the plan has Tick runtime slots, invocation is non-null and carries the
// complete kind-specific spans described by
// BackgroundEvaluationPlan::tick_runtime.
void execute_realtime_root(
    RealtimeRootPlan const* plan,
    std::byte* storage_base,
    TickInvocationCall const* invocation,
    GlobalIndex sample_index,
    std::size_t block_size) noexcept;

[[nodiscard]] bool has_early_tick(RealtimeRootPlan const* plan) noexcept;
// Number of consecutive project frames that can be prepared without
// overwriting values still required at the beginning of the prefix.
[[nodiscard]] std::size_t independent_early_frame_limit(RealtimeRootPlan const* plan) noexcept;

// An invocation owns the transient Sample and Event representations read by
// both roots. The same aligned bytes are bound in the worker and device calls.
class TickTransferBuffer {
    std::vector<std::byte> bytes_{};
    std::size_t capacity_ = 0;
    std::size_t alignment_ = 1;

public:
    void reserve(RealtimeRootPlan const* plan);
    [[nodiscard]] std::span<std::byte> view(RealtimeRootPlan const& plan) noexcept;
};

enum class TickRootPart : std::uint8_t { complete, early, foreground };

struct TickInvocationSlotRange {
    std::size_t begin = 0;
    std::size_t count = 0;
};

struct TickInvocationSlotPlan {
    std::vector<TickInvocationSlotRange> sequential_samples{};
    std::vector<TickInvocationSlotRange> sequential_events{};
    std::vector<TickInvocationSlotRange> random_access_samples{};
    std::vector<TickInvocationSlotRange> random_access_events{};
    std::vector<TickInvocationSlotRange> sample_captures{};
    std::vector<TickInvocationSlotRange> event_captures{};
};

// A split root binds only the runtime slots that its callbacks can consume.
[[nodiscard]] TickInvocationSlotPlan const& tick_invocation_slots(
    RealtimeRootPlan const& plan, TickRootPart part) noexcept;

// Mutable progress of early Tick. Prepare its storage away from the audio
// thread. A fresh range starts from the committed root cursors. Subsequent
// early callbacks advance these positions without moving the foreground.
class EarlyTickCursor {
    std::vector<std::uint64_t> pace_indices_{};
    std::vector<std::size_t> authored_extents_{};
    RealtimeRootPlan const* root_ = nullptr;

    friend void execute_early_tick_root(
        RealtimeRootPlan const&, std::byte*, TickInvocationCall const*,
        GlobalIndex, std::size_t, EarlyTickCursor&, std::span<std::byte>) noexcept;

public:
    void reserve(RealtimeRootPlan const* root);
    void start_from_committed_position() noexcept { root_ = nullptr; }
};

void execute_early_tick_root(
    RealtimeRootPlan const& plan,
    std::byte* storage_base,
    TickInvocationCall const* invocation,
    GlobalIndex sample_index,
    std::size_t block_size,
    EarlyTickCursor& cursor,
    std::span<std::byte> transfer) noexcept;

void execute_foreground_tick_root(
    RealtimeRootPlan const* plan,
    std::byte* storage_base,
    TickInvocationCall const* invocation,
    GlobalIndex sample_index,
    std::size_t block_size,
    std::span<std::byte> transfer) noexcept;
} // namespace iv::graph_jit
