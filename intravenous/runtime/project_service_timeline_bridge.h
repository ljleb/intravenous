#pragma once

namespace iv {
class Timeline;

void bind_project_service_timeline_bridge(Timeline &timeline);
void unbind_project_service_timeline_bridge(Timeline const &timeline);
} // namespace iv
