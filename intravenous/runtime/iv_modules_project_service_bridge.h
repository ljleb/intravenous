#pragma once

namespace iv {
class RuntimeProjectService;

void bind_iv_modules_project_service_bridge(RuntimeProjectService &service);
void unbind_iv_modules_project_service_bridge(RuntimeProjectService const &service);
} // namespace iv
