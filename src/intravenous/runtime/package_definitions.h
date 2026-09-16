#pragma once

#include <intravenous/runtime/package_pipeline_types.h>

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace iv {
class SocketRpcIvPackageResultBuilder;
class SocketRpcIvPackageDefinitionsResultBuilder;
struct CreateIvPackageRequest;
struct GetIvPackageDefinitionsRequest;

struct IvPackageInfo {
    std::string package_id = {};
    std::filesystem::path package_root = {};
    bool project_local = false;
    std::vector<std::string> module_ids = {};
    std::vector<std::string> node_type_ids = {};
    PackageBuildState build_state = PackageBuildState::queued;
    std::string build_message = {};
    std::string publication_message = {};
};

class PackageDefinitions {
    std::filesystem::path project_root_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, IvPackageInfo> packages_by_id_;
    std::unordered_map<std::string, std::shared_ptr<PackageRevision const>>
        accepted_revisions_by_package_id_;
    std::uint64_t snapshot_generation_ = 0;
    std::shared_ptr<PackageDefinitionsSnapshot const> snapshot_;

    [[nodiscard]] IvPackageInfo& ensure_package_locked(
        std::string const& package_id,
        std::filesystem::path const& package_root);
    void rebuild_snapshot_locked();
    void apply_publication_result(PackageDefinitionsPublicationRequest const& request);

public:
    explicit PackageDefinitions(std::filesystem::path project_root);

    [[nodiscard]] std::vector<IvPackageInfo> list_packages() const;
    [[nodiscard]] IvPackageInfo create_project_package(std::string const& name) const;
    [[nodiscard]] std::shared_ptr<PackageDefinitionsSnapshot const> snapshot() const;

    void handle_package_refresh(PackageRefreshTransaction const& transaction);
    void handle_socket_rpc_get_iv_package_definitions(
        GetIvPackageDefinitionsRequest const& request,
        SocketRpcIvPackageDefinitionsResultBuilder& builder) const;
    void handle_socket_rpc_create_iv_package(
        CreateIvPackageRequest const& request,
        SocketRpcIvPackageResultBuilder& builder) const;
};
} // namespace iv
