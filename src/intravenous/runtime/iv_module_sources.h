#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace iv {
class IvModuleSourceLookupBuilder;
class SocketRpcIvModuleSourceResultBuilder;
class SocketRpcIvModuleSourcesResultBuilder;
struct CreateIvModuleSourceRequest;
struct GetIvModuleSourcesRequest;

struct IvModuleSourceInfo {
    enum class DefinitionKind {
        node_type,
        module,
    };

    // One source-info item exists for each discovered registered unit.  This
    // makes node-only IV sources first-class rather than invisible because
    // they happen not to provide an iv module.
    std::string definition_id;
    DefinitionKind definition_kind = DefinitionKind::module;
    std::filesystem::path source_root;
    bool project_local = false;
};

class IvModuleSources {
    std::filesystem::path project_root_;
    std::vector<std::filesystem::path> shared_roots_;
public:
    IvModuleSources(std::filesystem::path project_root, std::vector<std::filesystem::path> shared_roots);
    [[nodiscard]] std::vector<IvModuleSourceInfo> list_sources() const;
    [[nodiscard]] std::optional<IvModuleSourceInfo> find_source(
        std::string const& definition_id) const;
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
