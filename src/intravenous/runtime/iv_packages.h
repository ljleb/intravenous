#pragma once

#include <intravenous/runtime/iv_module_reload.h>

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace iv {
class IvModuleDefinitions;
class SocketRpcIvPackageResultBuilder;
class SocketRpcIvPackagesResultBuilder;
struct CreateIvPackageRequest;
struct GetIvPackagesRequest;

struct IvPackageInfo {
    // One independently discoverable IV package. Package definition IDs are
    // compiler-produced data published by IvModuleDefinitions.
    std::string package_id;
    std::filesystem::path package_root;
    bool project_local = false;
    // Published definitions owned by this package. Module IDs are instantiable;
    // node type IDs are exposed separately so node-only packages remain visible.
    std::vector<std::string> module_ids;
    std::vector<std::string> node_type_ids;
    // Publication and compilation are separate states. A package with no
    // modules may be a valid node-only/empty package, still building, or have
    // failed before its compiler-produced definitions could be published.
    IvPackageBuildState build_state = IvPackageBuildState::queued;
    std::string build_message{};
    // A completed artifact can still be withheld by the definition registry.
    // This is distinct from a compiler failure and from a package that simply
    // contains no IV_MODULE/IV_NODE registrations.
    std::string publication_message{};
};

[[nodiscard]] std::vector<IvPackageInfo> discover_iv_packages(
    std::filesystem::path const& project_root,
    std::vector<std::filesystem::path> const& shared_roots);
[[nodiscard]] std::vector<std::pair<std::string, std::filesystem::path>>
discover_iv_package_declarations(
    std::filesystem::path const& project_root,
    std::vector<std::filesystem::path> const& shared_roots);

class IvPackages {
    std::filesystem::path project_root_;
    IvModuleDefinitions& definitions_;
    IvModuleReload const& reload_;
public:
    IvPackages(
        std::filesystem::path project_root,
        IvModuleDefinitions& definitions,
        IvModuleReload const& reload);
    [[nodiscard]] std::vector<IvPackageInfo> list_packages() const;
    [[nodiscard]] IvPackageInfo create_project_package(std::string const& name);

    void handle_socket_rpc_get_iv_packages(
        GetIvPackagesRequest const &request,
        SocketRpcIvPackagesResultBuilder &builder) const;
    void handle_socket_rpc_create_iv_package(
        CreateIvPackageRequest const &request,
        SocketRpcIvPackageResultBuilder &builder) const;
};
} // namespace iv
