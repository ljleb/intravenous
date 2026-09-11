#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace iv {
class IvModuleDefinitions;
class IvModuleSourceLookupBuilder;
class SocketRpcIvModuleSourceResultBuilder;
class SocketRpcIvModuleSourcesResultBuilder;
struct CreateIvModuleSourceRequest;
struct GetIvModuleSourcesRequest;

struct IvModuleSourceInfo {
    // This describes an independently discoverable IV *source package*, not
    // an inferred C++ registration. Registered IDs are compiler-produced
    // source-artifact data published by IvModuleDefinitions.
    std::string source_id;
    std::filesystem::path source_root;
    bool project_local = false;
    // Published registrations owned by this source package. Module IDs are
    // instantiable project definitions; node-type IDs are exposed separately
    // so node-only sources remain visible without pretending to be modules.
    std::vector<std::string> module_ids;
    std::vector<std::string> node_type_ids;
};

class IvModuleSources {
    std::filesystem::path project_root_;
    std::vector<std::filesystem::path> shared_roots_;
    IvModuleDefinitions const* definitions_ = nullptr;
public:
    IvModuleSources(
        std::filesystem::path project_root,
        std::vector<std::filesystem::path> shared_roots,
        IvModuleDefinitions const* definitions = nullptr);
    [[nodiscard]] std::vector<IvModuleSourceInfo> list_sources() const;
    // Finds the source package which currently owns a published IV module.
    // This never parses registration macros; it requires the source registry
    // to have completed a successful source generation first.
    [[nodiscard]] std::optional<IvModuleSourceInfo> find_source(
        std::string const& module_id) const;
    // Produces the complete source-package declaration snapshot for the
    // reload registry. The caller owns applying the snapshot transactionally.
    [[nodiscard]] std::vector<std::pair<std::string, std::filesystem::path>>
    source_declarations() const;
    [[nodiscard]] IvModuleSourceInfo create_project_source(std::string const& name) const;

    void handle_iv_module_source_lookup(
        std::string const &module_id,
        IvModuleSourceLookupBuilder &builder) const;
    void handle_socket_rpc_get_iv_module_sources(
        GetIvModuleSourcesRequest const &request,
        SocketRpcIvModuleSourcesResultBuilder &builder) const;
    void handle_socket_rpc_create_iv_module_source(
        CreateIvModuleSourceRequest const &request,
        SocketRpcIvModuleSourceResultBuilder &builder) const;
};
} // namespace iv
