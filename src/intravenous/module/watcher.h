#pragma once

#include <intravenous/linux_file_descriptor.h>
#include <intravenous/module/dependency.h>

#include <filesystem>
#include <unordered_set>
#include <vector>

#if !defined(__linux__)
#error "Intravenous dependency watching requires Linux inotify"
#endif

namespace iv {
    class DependencyWatcher {
        std::vector<ModuleDependency> _dependencies;
        UniqueFileDescriptor _fd;
        std::unordered_set<int> _watch_descriptors;
        bool _rescan_pending = false;

        void clear_watches();
        void add_directory_recursive(std::filesystem::path const& dir);
        [[nodiscard]] std::vector<ModuleDependency> scan_changed_dependencies() const;
        [[nodiscard]] bool consume_events();

    public:
        DependencyWatcher();
        ~DependencyWatcher() = default;

        DependencyWatcher(DependencyWatcher&& other) noexcept = default;
        DependencyWatcher& operator=(DependencyWatcher&& other) noexcept = default;

        DependencyWatcher(DependencyWatcher const&) = delete;
        DependencyWatcher& operator=(DependencyWatcher const&) = delete;

        void update(std::vector<ModuleDependency> dependencies);
        [[nodiscard]] std::vector<ModuleDependency> changed_dependencies();
        bool has_changes();
        [[nodiscard]] int native_handle() const noexcept { return _fd.get(); }
    };

    DependencyWatcher make_dependency_watcher();
}
