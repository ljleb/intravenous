#pragma once

#include "module/dependency.h"

#include <filesystem>
#include <vector>

#if defined(__linux__)
#include <unistd.h>
#endif

namespace iv {
    class DependencyWatcher {
        std::vector<ModuleDependency> _dependencies;

#if defined(__linux__)
        int _fd = -1;

        void reset();
        void add_directory_recursive(std::filesystem::path const& dir);
#endif

    public:
        DependencyWatcher();
        ~DependencyWatcher();

        DependencyWatcher(DependencyWatcher&& other) noexcept;
        DependencyWatcher& operator=(DependencyWatcher&& other) noexcept;

        DependencyWatcher(DependencyWatcher const&) = delete;
        DependencyWatcher& operator=(DependencyWatcher const&) = delete;

        void update(std::vector<ModuleDependency> dependencies);
        bool has_changes();
    };

    DependencyWatcher make_dependency_watcher();
}
