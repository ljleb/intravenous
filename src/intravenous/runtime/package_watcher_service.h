#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace iv {
class PackageWatcher;

// Process-lifetime filesystem/discovery loop. Durable source/dependency state
// remains in PackageWatcher; this object owns only the blocking worker lifetime
// and transient discovery-error suppression.
class PackageWatcherService {
    PackageWatcher* watcher_ = nullptr;
    std::filesystem::path project_root_;
    std::vector<std::filesystem::path> shared_roots_;
    std::optional<std::vector<std::pair<std::string, std::filesystem::path>>>
        last_package_declarations_;
    std::optional<std::string> last_discovery_error_;
    std::optional<std::jthread> worker_{};

    [[nodiscard]] bool synchronize_discovered_packages();
    void report_discovery_error(std::string message);

public:
    PackageWatcherService(
        PackageWatcher& watcher,
        std::filesystem::path project_root,
        std::vector<std::filesystem::path> shared_roots);
    ~PackageWatcherService();

    PackageWatcherService(PackageWatcherService const&) = delete;
    PackageWatcherService& operator=(PackageWatcherService const&) = delete;

    void start();
    void request_shutdown();
};
} // namespace iv
