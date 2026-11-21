#pragma once

#include <intravenous/linux_file_descriptor.h>

#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if !defined(__linux__)
#error "Intravenous package discovery requires Linux inotify"
#endif

namespace iv {
struct PackageDiscoveryEvents {
    bool coverage_may_have_changed = false;
    bool declarations_may_have_changed = false;
};

class PackageDiscoveryWatcher {
    UniqueFileDescriptor fd_;
    std::vector<std::filesystem::path> roots_;
    std::unordered_map<std::string, int> watch_by_path_;
    std::unordered_map<int, std::string> path_by_watch_;
    std::unordered_multiset<int> retired_watch_descriptors_;

    void ensure_watch(
        std::filesystem::path const& directory,
        std::unordered_set<std::string>& desired_paths);
    void add_root_coverage(
        std::filesystem::path const& root,
        std::unordered_set<std::string>& desired_paths);
    void remove_stale_watches(std::unordered_set<std::string> const& desired_paths);
    void forget_watch(int descriptor) noexcept;

public:
    explicit PackageDiscoveryWatcher(std::vector<std::filesystem::path> roots);
    ~PackageDiscoveryWatcher() = default;

    PackageDiscoveryWatcher(PackageDiscoveryWatcher const&) = delete;
    PackageDiscoveryWatcher& operator=(PackageDiscoveryWatcher const&) = delete;
    PackageDiscoveryWatcher(PackageDiscoveryWatcher&&) = delete;
    PackageDiscoveryWatcher& operator=(PackageDiscoveryWatcher&&) = delete;

    [[nodiscard]] int native_handle() const noexcept { return fd_.get(); }

    // Reconciles recursive inotify coverage with the current discovery tree.
    // Existing roots are watched before descending so topology mutations during
    // traversal remain observable; obsolete ancestor/subtree watches are pruned.
    void refresh_coverage();

    // Drains queued inotify events and classifies only changes that can affect
    // discovery. Ordinary writes to existing source files do not request a
    // declaration rescan.
    [[nodiscard]] PackageDiscoveryEvents consume_events();
};
} // namespace iv
