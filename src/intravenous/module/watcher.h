#pragma once

#include <intravenous/linux_file_descriptor.h>
#include <intravenous/module/dependency.h>

#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if !defined(__linux__)
#error "Intravenous dependency watching requires Linux inotify"
#endif

namespace iv {
    class DependencyWatcher {
        struct Watch {
            int descriptor = -1;
            std::unordered_set<std::size_t> dependency_indices;
        };

        std::vector<ModuleDependency> _dependencies;
        UniqueFileDescriptor _fd;
        std::unordered_map<std::string, Watch> _watch_by_path;
        std::unordered_map<int, std::string> _path_by_watch;
        std::unordered_multiset<int> _retired_watch_descriptors;
        bool _rescan_pending = false;

        void clear_watches();
        void ensure_watch(
            std::filesystem::path const& directory,
            std::size_t dependency_index,
            std::unordered_map<std::string, std::unordered_set<std::size_t>>& desired_paths);
        void add_dependency_coverage(
            std::size_t dependency_index,
            std::unordered_map<std::string, std::unordered_set<std::size_t>>& desired_paths);
        void remove_stale_watches(
            std::unordered_map<std::string, std::unordered_set<std::size_t>> const& desired_paths);
        void forget_watch(int descriptor) noexcept;
        void refresh_coverage();
        [[nodiscard]] std::vector<ModuleDependency> scan_changed_dependencies() const;
        [[nodiscard]] std::vector<ModuleDependency> consume_events();

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
