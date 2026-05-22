#pragma once

namespace iv {
class RuntimeGraphInputLanes;

void bind_iv_modules_graph_input_lanes_bridge(RuntimeGraphInputLanes &lanes);
void unbind_iv_modules_graph_input_lanes_bridge(RuntimeGraphInputLanes const &lanes);
} // namespace iv
