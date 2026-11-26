#pragma once

#include <intravenous/channel_ports.h>
#include <intravenous/graph/port_ids.h>
#include <intravenous/node/address.h>
#include <intravenous/sample.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <optional>

namespace iv {

// The default resolves known gaps. The explicit additive policy remains
// available for Sequential inputs that mix a manual and connected value.
enum class ManualControlParticipation {
    disconnected_only,
    participate_with_connections,
};

// Durable semantic identity of one authored input control or output gain. The
// NodeAddress is resolved again for every ProjectGraph generation; no
// GraphBuilder-local bundle or port index crosses that boundary.
enum class ManualControlKind : std::uint8_t { input, output_gain };

struct ManualControlId {
    NodeAddress node{};
    std::string port_name{};
    ManualControlKind kind = ManualControlKind::input;

    bool operator==(ManualControlId const&) const = default;
};

inline bool is_symbolic_output_gain(ManualControlId const& id)
{
    return id.kind == ManualControlKind::output_gain && !id.node.path.empty()
        && std::holds_alternative<ProjectSymbolicNodeSelector>(id.node.path.back());
}

// One frontend gesture. client_id prevents two attached clients from sharing
// an interaction lifetime accidentally; interaction_id is client-local.
struct ManualControlInteractionId {
    std::string client_id{};
    std::string interaction_id{};

    bool operator==(ManualControlInteractionId const&) const = default;
};

// Project-owned scalar state. Input controls broadcast a Sample to the resolved
// Sequential or Coverage input. Output gains multiply each channel through its value type's
// explicit gain operation, independently of input participation policy.
struct ProjectManualControl {
    ManualControlId id{};
    Sample stored_value{};
    ManualControlParticipation participation =
        ManualControlParticipation::disconnected_only;
    bool enabled = true;

    bool operator==(ProjectManualControl const&) const = default;
};

inline bool supports_coverage_manual_control(ContinuousInputConfig const& config)
{
    return input_access(config) == InputAccessKind::coverage
        && config.content_access == InputContentAccess::windowed
        && continuous_value_type_domain(config).sole_type() == ContinuousValueTypeId::sample;
}

inline std::optional<CircularSampleRange> circular_manual_control_range(ContinuousInputConfig const& config)
{
    if (!is_sequential(config)) return std::nullopt;
    auto const& metadata = sequential_input_properties(config).value_metadata;
    if (metadata.size() != 1) return std::nullopt;
    auto const* sample = std::get_if<ContinuousValueType<Sample>::SequentialInputMetadata>(&metadata.front());
    if (!sample || !sample->circular) return std::nullopt;
    return CircularSampleRange{sample->min, sample->max};
}

// One ProjectGraph-specialization binding after NodeAddress and port-name
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
    // Transient replacement of incoming wires at the knob being edited.
    bool override_connections = false;
    // Public output gain before compiler-private transform expansion. The
    // authored input field is unused for this control until that expansion.
    std::optional<NodeBundlePortId> output{};
    // Symbolic record supplying this output gain, after address resolution.
    std::optional<std::size_t> output_symbolic_node{};
    // Symbolic record owning this input control, after address resolution.
    std::optional<std::size_t> input_symbolic_node{};
    // Its value is frozen into a prepared graph, never copied from an atomic
    // live target into an already active background-dependent executable.
    bool requires_preparation = false;
    // The compiler's Coverage fallback producer for this input, if any.
    std::optional<NodeBundleHandle> coverage_source{};
    bool enabled = true;
    std::optional<CircularSampleRange> circular_range{};
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
