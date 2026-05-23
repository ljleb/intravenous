#pragma once

#include "devices/audio_device.h"
#include "graph/build_types.h"
#include "module/dependency.h"
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace iv {
class GraphBuilder;
class NodeExecutor;
struct RuntimeProjectConfig;
struct RuntimeIvModuleDefinitionState;

using AudioDeviceFactory = std::function<std::optional<LogicalAudioDevice>()>;

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

class RuntimeIvModuleDefinitions {
    std::filesystem::path workspace_root;
    std::filesystem::path discovery_start;
    std::vector<std::filesystem::path> extra_search_roots;
    AudioDeviceFactory audio_device_factory;

    mutable std::mutex mutex;
    std::condition_variable initialized_cv;
    std::unique_ptr<RuntimeProjectConfig> config;
    std::unique_ptr<RuntimeIvModuleDefinitionState> root_definition;
    std::exception_ptr pending_exception;
    bool initialized = false;
    bool shutdown_requested = false;
    std::optional<std::jthread> runtime_thread;
    NodeExecutor *executor_state = nullptr;

    void emit_notification(RuntimeIvModuleDefinitionsNotification notification) const;
    void emit_message(std::string level, std::string message) const;
    void emit_status(
        std::string code,
        std::string level,
        std::string message,
        std::filesystem::path module_root = {},
        std::vector<std::string> created_definition_ids = {},
        std::vector<std::string> deleted_definition_ids = {}) const;
    void run_runtime();

public:
    explicit RuntimeIvModuleDefinitions(
        std::filesystem::path workspace_root,
        std::filesystem::path discovery_start,
        std::vector<std::filesystem::path> extra_search_roots = {},
        AudioDeviceFactory audio_device_factory = {});
    ~RuntimeIvModuleDefinitions();
    RuntimeIvModuleDefinitions(RuntimeIvModuleDefinitions &&) = delete;
    RuntimeIvModuleDefinitions &operator=(RuntimeIvModuleDefinitions &&) = delete;
    RuntimeIvModuleDefinitions(RuntimeIvModuleDefinitions const &) = delete;
    RuntimeIvModuleDefinitions &operator=(RuntimeIvModuleDefinitions const &) = delete;

    RuntimeIvModuleDefinition initialize();
    void request_shutdown();
};
} // namespace iv
