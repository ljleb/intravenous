#pragma once

#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace iv {
struct IvModuleDefinitionsChanged;
struct IvNodeTypeDefinitionsChanged;
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

[[nodiscard]] std::vector<IvPackageInfo> discover_iv_packages(
    std::filesystem::path const& project_root,
    std::vector<std::filesystem::path> const& shared_roots);
[[nodiscard]] std::vector<std::pair<std::string, std::filesystem::path>>
discover_iv_package_declarations(
    std::filesystem::path const& project_root,
    std::vector<std::filesystem::path> const& shared_roots);

class IvPackages {
    std::filesystem::path project_root_;
    std::vector<std::filesystem::path> shared_roots_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::string> module_package_ids_;
    std::unordered_map<std::string, std::string> node_type_package_ids_;
public:
    IvPackages(
        std::filesystem::path project_root,
        std::vector<std::filesystem::path> shared_roots);
    [[nodiscard]] std::vector<IvPackageInfo> list_packages() const;
    [[nodiscard]] IvPackageInfo create_project_package(std::string const& name) const;

    void handle_iv_module_definitions_changed(
        IvModuleDefinitionsChanged const& diff);
    void handle_iv_node_type_definitions_changed(
        IvNodeTypeDefinitionsChanged const& diff);
    void handle_socket_rpc_get_iv_packages(
        GetIvPackagesRequest const &request,
        SocketRpcIvPackagesResultBuilder &builder) const;
    void handle_socket_rpc_create_iv_package(
        CreateIvPackageRequest const &request,
        SocketRpcIvPackageResultBuilder &builder) const;
};
} // namespace iv
