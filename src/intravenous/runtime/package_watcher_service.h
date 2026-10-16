#pragma once

#include <intravenous/linux_file_descriptor.h>
#include <intravenous/runtime/package_discovery_watcher.h>
#include <intravenous/runtime/package_pipeline_events.h>

#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if !defined(__linux__)
#error "Intravenous package watching requires Linux inotify"
#endif

namespace iv {
class PackageWatcher;

// Process-lifetime filesystem/discovery loop. Durable source/dependency state
// remains in PackageWatcher; this object owns blocking poll/wakeup mechanics and
// transient service/discovery error suppression.
class PackageWatcherService {
    PackageWatcher* watcher_ = nullptr;
    std::filesystem::path project_root_;
    std::vector<std::filesystem::path> shared_roots_;
    PackageDiscoveryWatcher discovery_watcher_;
    EventSignal work_signal_{"package watcher work"};
    EventSignal shutdown_signal_{"package watcher shutdown"};
    std::optional<std::vector<std::pair<std::string, std::filesystem::path>>>
        last_package_declarations_;
    std::optional<std::string> last_discovery_error_;
    std::optional<std::string> last_service_error_;
    std::optional<std::jthread> worker_{};

    [[nodiscard]] bool synchronize_discovered_packages();
    void report_discovery_error(std::string message);
    void report_service_error(std::string message);
    void process_pending_refresh();

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
    void handle_work_available(PackageWatcherWorkAvailable const&) noexcept;
};
} // namespace iv
