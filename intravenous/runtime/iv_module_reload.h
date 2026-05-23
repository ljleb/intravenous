#pragma once

#include "graph/build_types.h"
#include "graph/builder.h"
#include "module/dependency.h"
#include "module/loader.h"
#include "module/watcher.h"
#include "runtime/iv_module_definitions.h"
#include "runtime/startup_config.h"

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace iv {
struct RuntimeIvModuleReloadedDefinition {
    std::string definition_id{};
    std::filesystem::path module_root{};
    std::string module_id{};
    GraphIntrospectionMetadata introspection{};
    std::vector<ModuleDependency> dependencies{};
    std::vector<ModuleRef> module_refs{};
    GraphBuilder canonical_builder{};
};

struct RuntimeIvModuleReloadFailure {
    std::string definition_id{};
    std::filesystem::path module_root{};
    std::string message{};
};

struct RuntimeIvModuleReloadResults {
    std::vector<RuntimeIvModuleReloadedDefinition> loaded{};
    std::vector<RuntimeIvModuleReloadFailure> failed{};
};

class RuntimeIvModuleReload {
    StartupConfigState startup_config;
    mutable std::mutex mutex;
    std::unordered_map<std::string, RuntimeIvModuleDefinitionDeclaration> declarations_by_id;
    std::unordered_map<std::string, std::vector<ModuleDependency>> dependencies_by_definition_id;
    DependencyWatcher watcher;

    void reload_declarations(
        std::vector<RuntimeIvModuleDefinitionDeclaration> const &declarations);
    void refresh_watched_dependencies_locked();

public:
    explicit RuntimeIvModuleReload(StartupConfigState startup_config_);

    void handle_definition_declarations_changed(
        RuntimeIvModuleDefinitionDeclarationsChanged const &diff);

    bool has_changes();
    void reload_changed_definitions();
};
} // namespace iv
