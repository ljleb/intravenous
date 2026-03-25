#include "module/loader.h"
#include "module/watcher.h"
#include "runtime/crash_handlers.h"
#include "runtime/system.h"

#include <filesystem>
#include <cstdlib>
#include <csignal>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

using namespace iv;

namespace {
    iv::System* g_active_system = nullptr;

    void handle_sigint(int)
    {
        if (g_active_system) {
            g_active_system->request_shutdown();
        }
    }

#if defined(_WIN32)
    BOOL WINAPI handle_console_ctrl(DWORD ctrl_type)
    {
        if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT || ctrl_type == CTRL_CLOSE_EVENT) {
            handle_sigint(0);
            return TRUE;
        }
        return FALSE;
    }
#endif

    void install_shutdown_handlers(iv::System& system)
    {
        g_active_system = &system;
        std::signal(SIGINT, handle_sigint);
#if defined(_WIN32)
        SetConsoleCtrlHandler(handle_console_ctrl, TRUE);
#endif
    }
}

std::vector<std::filesystem::path> parse_search_path_env(char const* name)
{
    std::vector<std::filesystem::path> roots;
    char const* value = std::getenv(name);
    if (!value || !*value) {
        return roots;
    }

#if defined(_WIN32)
    constexpr char separator = ';';
#else
    constexpr char separator = ':';
#endif

    std::string_view remaining(value);
    while (!remaining.empty()) {
        size_t split = remaining.find(separator);
        std::string_view token = remaining.substr(0, split);
        if (!token.empty()) {
            roots.emplace_back(std::string(token));
        }
        if (split == std::string_view::npos) {
            break;
        }
        remaining.remove_prefix(split + 1);
    }

    return roots;
}

int main(int argc, char** argv)
{
#ifndef NDEBUG
    install_crash_handlers();
#endif

    if (argc < 2) {
        std::cerr << "usage: intravenous <module-path> [search-root...]\n";
        std::cerr << "env:   IV_MODULE_SEARCH_PATH="
#if defined(_WIN32)
                  << "root1;root2"
#else
                  << "root1:root2"
#endif
                  << '\n';
        return 2;
    }

    std::cout << "Audio running. Press Enter to quit.\n";

    System system;
    install_shutdown_handlers(system);
    std::filesystem::path const module_path = argv[1];
    std::vector<std::filesystem::path> search_roots = parse_search_path_env("IV_MODULE_SEARCH_PATH");
    for (int i = 2; i < argc; ++i) {
        search_roots.emplace_back(argv[i]);
    }

    ModuleLoader loader(std::filesystem::current_path(), std::move(search_roots));
    auto watcher = make_dependency_watcher();
    system.activate_root(true);
    auto live_graph = loader.load_root(module_path, system);
    watcher->update(live_graph.dependencies);
    system.activate_root(live_graph.sink_count != 0);
    auto processor = std::make_unique<NodeProcessor>(
        system.make_processor(std::move(live_graph.root), std::move(live_graph.module_refs))
    );

    struct PendingReload {
        std::unique_ptr<NodeProcessor> processor;
        std::vector<ModuleDependency> dependencies;
        bool uses_audio_device = false;
    };

    std::mutex reload_mutex;
    std::optional<PendingReload> pending_reload;
    bool build_in_progress = false;

    std::jthread reload_thread([&](std::stop_token stop_token) {
        while (!stop_token.stop_requested() && !system.is_shutdown_requested()) {
            bool should_build = false;
            {
                std::lock_guard lock(reload_mutex);
                should_build =
                    !build_in_progress &&
                    !pending_reload.has_value() &&
                    watcher->has_changes();
                if (should_build) {
                    build_in_progress = true;
                }
            }

            if (should_build) {
                try {
                    auto next_graph = loader.load_root(module_path, system);
                    PendingReload next_reload {
                        .processor = std::make_unique<NodeProcessor>(system.make_processor(
                            std::move(next_graph.root),
                            std::move(next_graph.module_refs)
                        )),
                        .dependencies = std::move(next_graph.dependencies),
                        .uses_audio_device = next_graph.sink_count != 0,
                    };

                    std::lock_guard lock(reload_mutex);
                    pending_reload = std::move(next_reload);
                } catch (std::exception const& e) {
                    std::cerr << "reload failed: " << e.what() << '\n';
                }

                std::lock_guard lock(reload_mutex);
                build_in_progress = false;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });

    size_t execution_block_size = processor->max_block_size();
    validate_block_size(execution_block_size, "audio device block size must be a power of 2");

    for (size_t global_index = 0; !system.is_shutdown_requested(); global_index += execution_block_size) {
        processor->tick_block({}, global_index, execution_block_size);

        std::optional<PendingReload> next_reload;
        {
            std::lock_guard lock(reload_mutex);
            if (pending_reload.has_value()) {
                next_reload = std::move(pending_reload);
                pending_reload.reset();
            }
        }

        if (next_reload.has_value()) {
            system.activate_root(next_reload->uses_audio_device);
            processor = std::move(next_reload->processor);
            watcher->update(std::move(next_reload->dependencies));
            execution_block_size = processor->max_block_size();
            std::cout << "reloaded " << module_path.string() << '\n';
        }
    }

    g_active_system = nullptr;
    return 0;
}
