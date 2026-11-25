#pragma once

#include <intravenous/ports.h>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace iv {

enum class GraphJitDiagnosticStage : std::uint8_t {
    input_capture,
    package_llvm,
    lowering,
    optimization,
    materialization,
};

struct GraphJitConnectionReference {
    PortKind kind;
    std::size_t index;

    auto operator<=>(GraphJitConnectionReference const&) const = default;
};

struct GraphJitDiagnostic {
    GraphJitDiagnosticStage stage = GraphJitDiagnosticStage::input_capture;
    std::string message{};
    std::size_t node_bundle = static_cast<std::size_t>(-1);
    std::string node_type_id{};
    std::string package_root{};
    // Indices into the configured Sample and Event connection lists. Compiler
    // stages preserve the original list prefixes when adding their own wiring.
    std::vector<GraphJitConnectionReference> feedback_connections{};
};

} // namespace iv
