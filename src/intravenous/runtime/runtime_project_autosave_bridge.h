#pragma once

namespace iv {
    class ProjectAutosave;

    void bind_runtime_project_autosave_bridge(ProjectAutosave& project_autosave);
    void unbind_runtime_project_autosave_bridge(ProjectAutosave const& project_autosave);
} // namespace iv
