#pragma once

#include <intravenous/graph/build_types.h>
#include <intravenous/graph/builder.h>
#include <intravenous/module/dependency.h>
#include <intravenous/module/loader.h>
#include <intravenous/module/watcher.h>
#include <intravenous/runtime/iv_module_definitions.h>
#include <intravenous/runtime/startup_config.h>

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

struct IvModuleReloadStatus {
    std::string level = "info";
    std::string code{};
    std::string message{};
    std::filesystem::path module_root{};
};

class IvModuleReload {
    StartupConfigState startup_config;
    mutable std::mutex mutex;
    std::unordered_map<std::string, IvModuleDefinitionDeclaration> declarations_by_id;
    std::unordered_map<std::string, std::vector<ModuleDependency>> dependencies_by_definition_id;
    std::unordered_set<std::string> dirty_definition_ids;
    IvModuleReloadResults pending_results;
    DependencyWatcher watcher;
    Sample device_sample_period_ {};

    [[nodiscard]] IvModuleReloadResults reload_declarations(
        std::vector<IvModuleDefinitionDeclaration> const &declarations);
    void refresh_watched_dependencies_locked();

public:
    explicit IvModuleReload(StartupConfigState startup_config_);

    void set_toolchain_config(ModuleLoaderToolchainConfig toolchain);
    [[nodiscard]] ModuleLoaderToolchainConfig toolchain_config() const;
    void handle_definition_declarations_changed(
        IvModuleDefinitionDeclarationsChanged const &diff);

    [[nodiscard]] bool has_dirty_definitions() const;
    bool has_changes();
    void compile_dirty_definitions();
    void reload_changed_definitions();
    [[nodiscard]] bool has_pending_results() const;
    void apply_pending_results();
};
} // namespace iv
