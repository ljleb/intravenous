#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace iv {
class IvModuleDefinitions;
class IvPackageLookupBuilder;
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
};

class IvPackages {
    std::filesystem::path project_root_;
    std::vector<std::filesystem::path> shared_roots_;
    IvModuleDefinitions const* definitions_ = nullptr;
public:
    IvPackages(
        std::filesystem::path project_root,
        std::vector<std::filesystem::path> shared_roots,
        IvModuleDefinitions const* definitions = nullptr);
    [[nodiscard]] std::vector<IvPackageInfo> list_packages() const;
    // Finds the package that currently owns a published IV module. Ownership is
    // compiler-produced and only available after a successful package build.
    [[nodiscard]] std::optional<IvPackageInfo> find_package(
        std::string const& module_id) const;
    // Produces the complete package declaration snapshot. The caller applies it
    // transactionally to the loaded package set.
    [[nodiscard]] std::vector<std::pair<std::string, std::filesystem::path>>
    package_declarations() const;
    [[nodiscard]] IvPackageInfo create_project_package(std::string const& name) const;

    void handle_iv_package_lookup(
        std::string const &module_id,
        IvPackageLookupBuilder &builder) const;
    void handle_socket_rpc_get_iv_packages(
        GetIvPackagesRequest const &request,
        SocketRpcIvPackagesResultBuilder &builder) const;
    void handle_socket_rpc_create_iv_package(
        CreateIvPackageRequest const &request,
        SocketRpcIvPackageResultBuilder &builder) const;
};
} // namespace iv
