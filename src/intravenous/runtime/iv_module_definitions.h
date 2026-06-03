#pragma once

#include <intravenous/graph/build_types.h>
#include <intravenous/graph/builder.h>
#include <intravenous/module/dependency.h>

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace iv {
using ModuleRef = std::shared_ptr<void>;

struct IvModuleDefinitionDeclaration {
    std::string definition_id{};
    std::filesystem::path module_root{};
};

struct IvModuleDefinitionDeclarationsChanged {
    std::vector<IvModuleDefinitionDeclaration> created{};
    std::vector<IvModuleDefinitionDeclaration> updated{};
    std::vector<std::string> deleted_definition_ids{};
};

struct IvModuleDefinition {
    std::string definition_id{};
    std::filesystem::path module_root{};
    std::string module_id{};
    GraphIntrospectionMetadata introspection{};
    std::vector<ModuleDependency> dependencies{};
    std::vector<ModuleRef> module_refs{};
    GraphBuilder const *canonical_builder = nullptr;
};

struct IvModuleDefinitionsChanged {
    std::vector<IvModuleDefinition> created{};
    std::vector<IvModuleDefinition> updated{};
    std::vector<std::string> deleted_definition_ids{};
};

struct IvModuleDefinitionsMessage {
    std::string level = "info";
    std::string message{};
    std::filesystem::path module_root{};
};

struct IvModuleDefinitionsStatus {
    std::string level = "info";
    std::string code{};
    std::string message{};
    std::filesystem::path module_root{};
    std::vector<std::string> created_definition_ids{};
    std::vector<std::string> deleted_definition_ids{};
};

using IvModuleDefinitionsNotification =
    std::variant<IvModuleDefinitionsMessage, IvModuleDefinitionsStatus>;

struct IvModuleRequiredDefinitionsChanged;
struct IvModuleReloadResults;
struct IvModuleReloadedDefinition;

class IvModuleDefinitions {
public:
    struct DefinitionState {
        std::vector<ModuleRef> module_refs{};
        IvModuleDefinition snapshot{};
        GraphBuilder canonical_builder{};
    };

private:
    mutable std::mutex mutex;
    std::unordered_map<std::string, IvModuleDefinitionDeclaration> declarations_by_id;
    std::unordered_map<std::string, std::unique_ptr<DefinitionState>> loaded_definitions_by_id;

    void emit_notification(IvModuleDefinitionsNotification notification) const;
    void emit_message(std::string level, std::string message, std::filesystem::path module_root = {}) const;
    void emit_status(
        std::string code,
        std::string level,
        std::string message,
        std::filesystem::path module_root = {},
        std::vector<std::string> created_definition_ids = {},
        std::vector<std::string> deleted_definition_ids = {}) const;

public:
    IvModuleDefinitions() = default;
    ~IvModuleDefinitions();

    std::string declare_definition(std::filesystem::path module_root);
    void remove_definition(std::string const &definition_id);

    void handle_required_definitions_changed(
        IvModuleRequiredDefinitionsChanged const &diff);
    void handle_reload_results(IvModuleReloadResults const &results);

    void seed_loaded_definition(IvModuleReloadedDefinition loaded_definition);

    [[nodiscard]] std::vector<IvModuleDefinition> loaded_definitions() const;
};
} // namespace iv
