#pragma once

#include "graph/build_types.h"
#include "graph/builder.h"
#include "module/dependency.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace iv {
class GraphBuilder;
using ModuleRef = std::shared_ptr<void>;

struct RuntimeIvModuleDefinitionDeclaration {
    std::string definition_id{};
    std::filesystem::path module_root{};
};

struct RuntimeIvModuleDefinitionDeclarationsChanged {
    std::vector<RuntimeIvModuleDefinitionDeclaration> created{};
    std::vector<RuntimeIvModuleDefinitionDeclaration> updated{};
    std::vector<std::string> deleted_definition_ids{};
};

struct RuntimeIvModuleDefinition {
    std::string definition_id{};
    std::filesystem::path module_root{};
    std::string module_id{};
    GraphIntrospectionMetadata introspection{};
    std::vector<ModuleDependency> dependencies{};
    GraphBuilder const *canonical_builder = nullptr;
};

struct RuntimeIvModuleDefinitionsChanged {
    std::vector<RuntimeIvModuleDefinition> created{};
    std::vector<RuntimeIvModuleDefinition> updated{};
    std::vector<std::string> deleted_definition_ids{};
};

struct RuntimeIvModuleDefinitionsMessage {
    std::string level = "info";
    std::string message{};
    std::filesystem::path module_root{};
};

struct RuntimeIvModuleDefinitionsStatus {
    std::string level = "info";
    std::string code{};
    std::string message{};
    std::filesystem::path module_root{};
    std::vector<std::string> created_definition_ids{};
    std::vector<std::string> deleted_definition_ids{};
};

using RuntimeIvModuleDefinitionsNotification =
    std::variant<RuntimeIvModuleDefinitionsMessage, RuntimeIvModuleDefinitionsStatus>;

struct RuntimeIvModuleRequiredDefinitionsChanged;
struct RuntimeIvModuleReloadResults;
struct RuntimeIvModuleReloadedDefinition;

class RuntimeIvModuleDefinitions {
public:
    struct DefinitionState {
        std::vector<ModuleRef> module_refs{};
        RuntimeIvModuleDefinition snapshot{};
        GraphBuilder canonical_builder{};
    };

private:
    mutable std::mutex mutex;
    std::unordered_map<std::string, RuntimeIvModuleDefinitionDeclaration> declarations_by_id;
    std::unordered_map<std::string, std::unique_ptr<DefinitionState>> loaded_definitions_by_id;

    void emit_notification(RuntimeIvModuleDefinitionsNotification notification) const;
    void emit_message(std::string level, std::string message, std::filesystem::path module_root = {}) const;
    void emit_status(
        std::string code,
        std::string level,
        std::string message,
        std::filesystem::path module_root = {},
        std::vector<std::string> created_definition_ids = {},
        std::vector<std::string> deleted_definition_ids = {}) const;

public:
    RuntimeIvModuleDefinitions() = default;
    ~RuntimeIvModuleDefinitions();

    std::string declare_definition(std::filesystem::path module_root);
    void remove_definition(std::string const &definition_id);

    void handle_required_definitions_changed(
        RuntimeIvModuleRequiredDefinitionsChanged const &diff);
    void handle_reload_results(RuntimeIvModuleReloadResults const &results);

    void seed_loaded_definition(RuntimeIvModuleReloadedDefinition loaded_definition);

    [[nodiscard]] std::vector<RuntimeIvModuleDefinition> loaded_definitions() const;
};
} // namespace iv
