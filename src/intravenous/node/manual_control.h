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

// Cold metadata that lets GraphExecutor update the state owned by one active
// NodeStorage without inspecting GraphJit's internal lowering plan.
struct ManualControlRuntimeBinding {
    ConfiguredManualControl configured{};
    std::size_t state_storage_offset = 0;
    bool participates = false;

    bool operator==(ManualControlRuntimeBinding const&) const = default;
};

} // namespace iv
