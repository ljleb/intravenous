#pragma once

#include <intravenous/channel_ports.h>
#include <intravenous/graph/port_ids.h>
#include <intravenous/node/address.h>
#include <intravenous/sample.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace iv {

// Controls whether a stored manual value contributes while an authored graph
// connection also supplies the selected input.
enum class ManualControlParticipation {
    disconnected_only,
    participate_with_connections,
};

// Durable semantic identity of one manually controlled authored input. The
// NodeAddress is resolved again for every ProjectGraph generation; no
// GraphBuilder-local bundle or port index crosses that boundary.
struct ManualControlId {
    NodeAddress node{};
    std::string input_name{};

    bool operator==(ManualControlId const&) const = default;
};

// One frontend gesture. client_id prevents two attached clients from sharing
// an interaction lifetime accidentally; interaction_id is client-local.
struct ManualControlInteractionId {
    std::string client_id{};
    std::string interaction_id{};

    bool operator==(ManualControlInteractionId const&) const = default;
};

// Project-owned state. Front 14 initially supports scalar Sample controls. A
// scalar value is broadcast to every channel of the resolved Sequential Sample
// input and enters that input's authored combine operation.
struct ProjectManualControl {
    ManualControlId id{};
    Sample stored_value{};
    ManualControlParticipation participation =
        ManualControlParticipation::disconnected_only;
    bool enabled = true;

    bool operator==(ProjectManualControl const&) const = default;
};

// One ProjectGraph-specialization binding after NodeAddress and input-name
// resolution. Several records can retain one ManualControlId when its semantic
// address intentionally selects several concrete nodes.
struct ConfiguredManualControl {
    ManualControlId id{};
    NodeBundlePortId input{};
    std::size_t concrete_selection_index = 0;
    Sample settled_value{};
    ManualControlParticipation participation =
        ManualControlParticipation::disconnected_only;
    bool dynamic = false;
    // Transient source replacement for a live gesture. Independent of the
    // durable participation policy; never serialized into the project.
    bool override_connections = false;

    bool operator==(ConfiguredManualControl const&) const = default;
};

// Compiler-owned persistent state shared by constant and dynamic code
// specializations. target_bits is written through std::atomic_ref by the
// control path and loaded atomically by generated realtime code. effective_bits
// is owned by realtime code and preserves the interpolation origin.
struct ManualControlRuntimeState {
    std::uint32_t target_bits = 0;
    std::uint32_t effective_bits = 0;
};
static_assert(sizeof(ManualControlRuntimeState) == 2 * sizeof(std::uint32_t));
static_assert(std::atomic_ref<std::uint32_t>::is_always_lock_free,
    "manual-control target updates must stay lock-free");

// The latest value of the open gestures on one semantic control. One cell per
// ManualControlId is shared by every execution generation that binds the control,
// so a gesture update needs no generation lookup, executor guard, or project lock:
// the control path stores target_bits, and while open_gestures is nonzero the audio
// thread copies it into the active NodeStorage before each root call. Completion
// also publishes a sequence so the next callback repairs a copy that raced with
// the final commit, even after the last gesture has closed.
struct ManualControlLiveTarget {
    std::atomic<std::uint32_t> target_bits{0};
    std::atomic<std::uint32_t> open_gestures{0};
    // A callback that began copying before end committed must observe that
    // final value on its next call, even though the gesture is now closed.
    std::atomic<std::uint64_t> completion_sequence{0};
};
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
    "manual-control live targets must stay lock-free");

// How a control-path target change relates to a manual-control gesture.
enum class ManualControlGesturePhase {
    // A settled value for one named execution generation.
    none,
    // A gesture opens at this value; its live target follows later updates.
    begin,
    // A gesture closes at this committed value. Every accepted generation that
    // binds the control receives it before the live target is released.
    end,
};

// Cold metadata that lets GraphExecutor update the state owned by one active
// NodeStorage without inspecting GraphJit's internal lowering plan.
struct ManualControlRuntimeBinding {
    ConfiguredManualControl configured{};
    std::size_t state_storage_offset = 0;
    bool participates = false;

    bool operator==(ManualControlRuntimeBinding const&) const = default;
};

} // namespace iv
