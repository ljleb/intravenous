#pragma once

#include "module/dependency.h"
#include "module/loader.h"
#include "module/watcher.h"

#include <chrono>
#include <exception>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace iv {
    class ReloadWorker {
        struct PendingReload {
            ModuleLoader::LoadedGraph graph;
            std::vector<ModuleDependency> dependencies;
        };

        ModuleLoader* _loader = nullptr;
        DependencyWatcher* _watcher = nullptr;
        std::filesystem::path _module_path;
        std::function<ModuleLoader::LoadedGraph()> _build_reload;

        mutable std::mutex _mutex;
        std::optional<PendingReload> _pending_reload;
        std::exception_ptr _pending_exception;
        bool _build_in_progress = false;
        bool _shutdown_requested = false;
        std::optional<std::jthread> _thread;

    public:
        ReloadWorker(
            ModuleLoader& loader,
            DependencyWatcher& watcher,
            std::filesystem::path module_path,
            std::function<ModuleLoader::LoadedGraph()> build_reload
        ) :
            _loader(&loader),
            _watcher(&watcher),
            _module_path(std::move(module_path)),
            _build_reload(std::move(build_reload))
        {}

        void start()
        {
            if (_thread.has_value()) {
                return;
            }

            _thread.emplace([this](std::stop_token stop_token) {
                while (!stop_token.stop_requested()) {
                    {
                        std::lock_guard lock(_mutex);
                        if (_shutdown_requested) {
                            break;
                        }
                    }

                    bool should_build = false;
                    {
                        std::lock_guard lock(_mutex);
                        should_build =
                            !_build_in_progress &&
                            !_pending_reload.has_value() &&
                            _watcher->has_changes();
                        if (should_build) {
                            _build_in_progress = true;
                        }
                    }

                    if (should_build) {
                        try {
                            auto next_graph = _build_reload();
                            auto next_dependencies = next_graph.dependencies;
                            PendingReload next_reload {
                                .graph = std::move(next_graph),
                                .dependencies = std::move(next_dependencies),
                            };

                            std::lock_guard lock(_mutex);
                            if (!_shutdown_requested) {
                                _pending_reload = std::move(next_reload);
                            }
                            _build_in_progress = false;
                        } catch (...) {
                            std::lock_guard lock(_mutex);
                            _build_in_progress = false;
                            _pending_exception = std::current_exception();
                        }
                    }

                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            });
        }

        ~ReloadWorker()
        {
            request_shutdown();
        }

        void request_shutdown()
        {
            {
                std::lock_guard lock(_mutex);
                _shutdown_requested = true;
            }

            if (_thread.has_value()) {
                _thread->request_stop();
            }
        }

        std::optional<ModuleLoader::LoadedGraph> take_completed_reload(std::vector<ModuleDependency>* dependencies = nullptr)
        {
            std::lock_guard lock(_mutex);
            if (!_pending_reload.has_value()) {
                return std::nullopt;
            }

            if (dependencies) {
                *dependencies = _pending_reload->dependencies;
            }

            auto graph = std::move(_pending_reload->graph);
            _pending_reload.reset();
            return graph;
        }

        std::exception_ptr take_exception()
        {
            std::lock_guard lock(_mutex);
            auto exception = _pending_exception;
            _pending_exception = nullptr;
            return exception;
        }
    };
}
