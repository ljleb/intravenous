#pragma once

#include <intravenous/runtime/package_reload_types.h>
#include <intravenous/runtime/node_definitions.h>

#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <string>
#include <utility>
#include <vector>

namespace iv {
class SocketRpcIvPackageResultBuilder;
class SocketRpcIvPackageDefinitionsResultBuilder;
struct CreateIvPackageRequest;
struct GetIvPackageDefinitionsRequest;

struct IvPackageInfo {
    // One independently discoverable IV package. Package definition IDs are
    // compiler-produced data published by NodeDefinitions.
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
    PackageBuildState build_state = PackageBuildState::queued;
    std::string build_message{};
    // A completed artifact can still be withheld by the definition registry.
    // This is distinct from a compiler failure and from a package that simply
    // contains no IV_MODULE/IV_NODE registrations.
    std::string publication_message{};
};

[[nodiscard]] std::vector<IvPackageInfo> discover_iv_package_definitions(
    std::filesystem::path const& project_root,
    std::vector<std::filesystem::path> const& shared_roots);
[[nodiscard]] std::vector<std::pair<std::string, std::filesystem::path>>
discover_iv_package_declarations(
    std::filesystem::path const& project_root,
    std::vector<std::filesystem::path> const& shared_roots);

class IvPackageDefinitions {
    std::filesystem::path project_root_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, IvPackageInfo> packages_by_id_;

    [[nodiscard]] IvPackageInfo& ensure_package(
        std::string const &package_id,
        std::filesystem::path const &package_root);

public:
    explicit IvPackageDefinitions(std::filesystem::path project_root);
    [[nodiscard]] std::vector<IvPackageInfo> list_packages() const;
    [[nodiscard]] IvPackageInfo create_project_package(std::string const& name) const;

    void handle_package_declarations_changed(IvPackageDeclarationsChanged const &diff);
    void handle_package_definitions_changed(IvPackageDefinitionsChanged const &diff);
    void handle_package_build_statuses_changed(
        std::vector<PackageBuildStatus> const &statuses);
    void handle_socket_rpc_get_iv_package_definitions(
        GetIvPackageDefinitionsRequest const &request,
        SocketRpcIvPackageDefinitionsResultBuilder &builder) const;
    void handle_socket_rpc_create_iv_package(
        CreateIvPackageRequest const &request,
        SocketRpcIvPackageResultBuilder &builder) const;
};
} // namespace iv
