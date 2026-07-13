#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace iv {

// These types describe authored lane-model state. They deliberately make no
// reference to a presentation: clients may independently decide how (or
// whether) to render a lane model identified by its optional type id.
struct LaneUiStateSnapshot {
    std::uint64_t revision = 0;
    std::string serialized_state {};
};

struct LaneUiStateWrite {
    std::optional<std::uint64_t> expected_revision {};
    std::string_view serialized_state {};
};

enum class LaneUiStateEffect : std::uint8_t {
    ui_only,
    execution_content_changed,
    graph_shape_changed,
};

struct LaneUiStateApplyResult {
    bool accepted = false;
    std::uint64_t revision = 0;
    LaneUiStateEffect effect = LaneUiStateEffect::ui_only;
    std::string error_message {};
};

} // namespace iv
