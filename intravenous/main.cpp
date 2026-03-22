#include "module/loader.h"
#include "runtime/system.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stacktrace>
#include <thread>

using namespace iv;

[[noreturn]] void terminate_stacktrace()
{
    std::cerr << "\nstd::terminate called\n";

    if (auto ep = std::current_exception()) {
        try {
            std::rethrow_exception(ep);
        } catch (std::exception const& e) {
            std::cerr << std::stacktrace::current() << '\n';
            std::cerr << "uncaught exception: " << e.what() << '\n';
        } catch (...) {
            std::cerr << "uncaught non-std exception\n";
        }
    } else {
        std::cerr << "no active exception\n";
    }

    std::abort();
}

int main(int argc, char** argv)
{
#ifndef NDEBUG
    std::set_terminate(terminate_stacktrace);
#endif

    if (argc < 2) {
        std::cerr << "usage: intravenous <module-path>\n";
        return 2;
    }

    std::cout << "Audio running. Press Enter to quit.\n";

    System system;
    ModuleLoader loader;
    std::filesystem::path const module_path = argv[1];
    auto live_graph = loader.load_root(module_path, system);
    system.activate_root(live_graph.sink_count != 0);
    auto processor = std::make_unique<NodeProcessor>(
        system.wrap_root(std::move(live_graph.root), live_graph.sink_count != 0),
        std::move(live_graph.module_refs)
    );

    struct PendingReload {
        std::unique_ptr<NodeProcessor> processor;
        std::filesystem::file_time_type stamp {};
        bool uses_audio_device = false;
    };

    std::mutex reload_mutex;
    std::optional<PendingReload> pending_reload;
    std::filesystem::file_time_type live_stamp = live_graph.source_stamp;
    bool build_in_progress = false;

    std::jthread reload_thread([&](std::stop_token stop_token) {
        while (!stop_token.stop_requested() && !system.is_shutdown_requested()) {
            bool should_build = false;
            std::filesystem::file_time_type observed_live_stamp {};
            {
                std::lock_guard lock(reload_mutex);
                observed_live_stamp = live_stamp;
                should_build =
                    !build_in_progress &&
                    !pending_reload.has_value() &&
                    loader.source_changed(module_path, observed_live_stamp);
                if (should_build) {
                    build_in_progress = true;
                }
            }

            if (should_build) {
                try {
                    auto next_graph = loader.load_root(module_path, system);
                    PendingReload next_reload {
                        .processor = std::make_unique<NodeProcessor>(
                            system.wrap_root(std::move(next_graph.root), next_graph.sink_count != 0),
                            std::move(next_graph.module_refs)
                        ),
                        .stamp = next_graph.source_stamp,
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

    for (size_t global_index = 0; !system.is_shutdown_requested(); ++global_index) {
        processor->tick({}, global_index);

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
            {
                std::lock_guard lock(reload_mutex);
                live_stamp = next_reload->stamp;
            }
            std::cout << "reloaded " << module_path.string() << '\n';
        }
    }

    return 0;
}
