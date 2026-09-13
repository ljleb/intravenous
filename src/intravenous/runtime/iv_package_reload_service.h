#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace iv {
class IvModuleDefinitions;
class IvModuleReload;

// Owns the server-lifetime control loop for package discovery, compilation,
// and publication. The registry and reloader retain their respective state;
// this service owns only polling state and its worker lifetime.
class IvPackageReloadService {
    IvModuleReload* reload_ = nullptr;
    IvModuleDefinitions* definitions_ = nullptr;
    std::filesystem::path project_root_;
    std::vector<std::filesystem::path> shared_roots_;
    std::optional<std::vector<std::pair<std::string, std::filesystem::path>>>
        last_package_declarations_;
    std::optional<std::string> last_discovery_error_;
    std::optional<std::jthread> worker_ {};

    [[nodiscard]] bool synchronize_discovered_packages();
    void report_discovery_error(std::string message);

public:
    IvPackageReloadService(
        IvModuleReload& reload,
        IvModuleDefinitions& definitions,
        std::filesystem::path project_root,
        std::vector<std::filesystem::path> shared_roots);
    ~IvPackageReloadService();

    IvPackageReloadService(IvPackageReloadService const&) = delete;
    IvPackageReloadService& operator=(IvPackageReloadService const&) = delete;

    // Discovery, build, and registry publication begin only after RPC
    // readiness. Persisted module instances retain their own package
    // declarations, so startup does not need a synchronous filesystem scan.
    void start();
    void request_shutdown();
};
} // namespace iv
