#pragma once

#include <intravenous/graph/build_types.h>
#include <intravenous/graph/builder.h>
#include <intravenous/module/dependency.h>
#include <intravenous/module/loader.h>
#include <intravenous/module/watcher.h>
#include <intravenous/runtime/iv_module_definitions.h>
#include <intravenous/runtime/startup_config.h>

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace iv {
struct IvModuleReloadedDefinition {
    std::string definition_id{};
    std::filesystem::path module_root{};
    std::string module_id{};
    GraphIntrospectionMetadata introspection{};
    std::vector<ModuleDependency> dependencies{};
    std::vector<ModuleRef> module_refs{};
    GraphBuilder canonical_builder{};
};

struct IvModuleReloadFailure {
    std::string definition_id{};
    std::filesystem::path module_root{};
    std::string message{};
};

struct IvModuleReloadResults {
    std::vector<IvModuleReloadedDefinition> loaded{};
    std::vector<IvModuleReloadFailure> failed{};
};

class IvModuleReload {
    StartupConfigState startup_config;
    mutable std::mutex mutex;
    std::unordered_map<std::string, IvModuleDefinitionDeclaration> declarations_by_id;
    std::unordered_map<std::string, std::vector<ModuleDependency>> dependencies_by_definition_id;
    DependencyWatcher watcher;

    void reload_declarations(
        std::vector<IvModuleDefinitionDeclaration> const &declarations);
    void refresh_watched_dependencies_locked();

public:
    explicit IvModuleReload(StartupConfigState startup_config_);

    void set_toolchain_config(ModuleLoader::ToolchainConfig toolchain);
    [[nodiscard]] ModuleLoader::ToolchainConfig toolchain_config() const;
    void handle_definition_declarations_changed(
        IvModuleDefinitionDeclarationsChanged const &diff);

    bool has_changes();
    void reload_changed_definitions();
};
} // namespace iv
