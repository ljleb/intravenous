#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace iv {
class NodeDefinitions;
class PackageReload;

// Owns the server-lifetime control loop for package discovery, compilation,
// and publication. The registry and reloader retain their respective state;
// this service owns only polling state and its worker lifetime.
class PackageReloadService {
    PackageReload* reload_ = nullptr;
    NodeDefinitions* definitions_ = nullptr;
    std::filesystem::path project_root_;
    std::vector<std::filesystem::path> shared_roots_;
    std::optional<std::vector<std::pair<std::string, std::filesystem::path>>>
        last_package_declarations_;
    std::optional<std::string> last_discovery_error_;
    std::optional<std::jthread> worker_ {};

    [[nodiscard]] bool synchronize_discovered_packages();
    void report_discovery_error(std::string message);

public:
    PackageReloadService(
        PackageReload& reload,
        NodeDefinitions& definitions,
        std::filesystem::path project_root,
        std::vector<std::filesystem::path> shared_roots);
    ~PackageReloadService();

    PackageReloadService(PackageReloadService const&) = delete;
    PackageReloadService& operator=(PackageReloadService const&) = delete;

    // Discovery, build, and registry publication begin only after RPC
    // readiness. Persisted module instances retain their own package
    // declarations, so startup does not need a synchronous filesystem scan.
    void start();
    void request_shutdown();
};
} // namespace iv
