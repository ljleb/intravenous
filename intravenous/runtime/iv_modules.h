#pragma once

#include "devices/audio_device.h"
#include "graph/build_types.h"
#include "module/dependency.h"
#include "runtime/config.h"
#include "runtime/timeline.h"

#include <condition_variable>
#include <exception>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace iv {
using AudioDeviceFactory = std::function<std::optional<LogicalAudioDevice>()>;

struct RuntimeIvModuleDefinition {
    std::string definition_id{};
    std::filesystem::path module_root{};
    std::string module_id{};
    GraphIntrospectionMetadata introspection{};
    std::vector<ModuleDependency> dependencies{};
};

struct RuntimeIvModuleDefinitionsChanged {
    std::vector<RuntimeIvModuleDefinition> added{};
    std::vector<RuntimeIvModuleDefinition> updated{};
    std::vector<std::string> removed_definition_ids{};
};

struct RuntimeIvModulesMessage {
    std::string level = "info";
    std::string message{};
    std::filesystem::path module_root{};
};

struct RuntimeIvModulesStatus {
    std::string level = "info";
    std::string code{};
    std::string message{};
    std::filesystem::path module_root{};
    std::vector<std::string> created_definition_ids{};
    std::vector<std::string> deleted_definition_ids{};
};

using RuntimeIvModulesNotification =
    std::variant<RuntimeIvModulesMessage, RuntimeIvModulesStatus>;

class NodeExecutor;

class RuntimeIvModules {
    Timeline &timeline;
    std::filesystem::path workspace_root;
    std::filesystem::path discovery_start;
    std::vector<std::filesystem::path> extra_search_roots;
    AudioDeviceFactory audio_device_factory;

    mutable std::mutex mutex;
    std::condition_variable initialized_cv;
    std::optional<RuntimeProjectConfig> config;
    std::optional<RuntimeIvModuleDefinition> root_definition;
    std::exception_ptr pending_exception;
    bool initialized = false;
    bool shutdown_requested = false;
    std::optional<std::jthread> runtime_thread;
    NodeExecutor *executor_state = nullptr;

    void emit_notification(RuntimeIvModulesNotification notification) const;
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
    explicit RuntimeIvModules(
        Timeline &timeline,
        std::filesystem::path workspace_root,
        std::filesystem::path discovery_start,
        std::vector<std::filesystem::path> extra_search_roots = {},
        AudioDeviceFactory audio_device_factory = {});
    ~RuntimeIvModules();
    RuntimeIvModules(RuntimeIvModules &&) = delete;
    RuntimeIvModules &operator=(RuntimeIvModules &&) = delete;
    RuntimeIvModules(RuntimeIvModules const &) = delete;
    RuntimeIvModules &operator=(RuntimeIvModules const &) = delete;

    RuntimeIvModuleDefinition initialize();
    void request_shutdown();
};
} // namespace iv
