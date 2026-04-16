#pragma once

#include "devices/audio_device.h"
#include "fake_audio_device.h"
#include "module/loader.h"
#include "node/executor.h"
#include "runtime/handlers.h"
#include "juce/vst_runtime.h"

#include <chrono>
#include <cstdlib>
#include <cctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

namespace iv::test {
    inline std::filesystem::path repo_root()
    {
        return std::filesystem::path(__FILE__).parent_path().parent_path();
    }

    inline std::filesystem::path test_modules_root()
    {
        return repo_root() / "tests" / "test_modules";
    }

    inline std::filesystem::path duplicate_modules_root()
    {
        return repo_root() / "tests" / "test_modules_duplicate";
    }

    inline std::filesystem::path runtime_modules_root()
    {
        return repo_root() / "build" / "test_runtime_modules";
    }

    inline std::filesystem::path runtime_module_cache_root()
    {
        return repo_root() / "build" / "iv_runtime_modules";
    }

    inline std::string sanitize_module_id(std::string_view id)
    {
        std::string sanitized(id);
        if (sanitized.empty()) {
            sanitized = "module";
        }

        for (char& c : sanitized) {
            bool good =
                (c >= 'a' && c <= 'z') ||
                (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9');
            if (!good) {
                c = '_';
            }
        }

        return sanitized;
    }

    inline char const* active_build_config()
    {
#if defined(NDEBUG)
        return "Release";
#else
        return "Debug";
#endif
    }

    inline std::string stable_path_hash(std::filesystem::path const& path)
    {
        auto normalized = std::filesystem::weakly_canonical(path).generic_string();
        uint64_t hash = 1469598103934665603ull;
        for (unsigned char c : normalized) {
            hash ^= c;
            hash *= 1099511628211ull;
        }

        std::ostringstream out;
        out << std::hex << hash;
        return out.str();
    }

    inline std::filesystem::path runtime_module_workspace(std::string_view id, std::filesystem::path const& module_dir)
    {
        return runtime_module_cache_root() / (sanitize_module_id(id) + "_" + stable_path_hash(module_dir)) / active_build_config();
    }

    inline void require(bool condition, char const* message)
    {
        if (!condition) {
            std::cerr << message << '\n';
            std::exit(1);
        }
    }

    inline void require_contains(std::string const& text, std::string const& needle, char const* message)
    {
        require(text.contains(needle), message);
    }

    inline std::string read_text(std::filesystem::path const& path)
    {
        std::ifstream in(path, std::ios::binary);
        require(static_cast<bool>(in), "failed to open input file");
        return std::string(
            std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()
        );
    }

    inline void write_text(std::filesystem::path const& path, std::string const& text)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        require(static_cast<bool>(out), "failed to open output file");
        out << text;
    }

    inline std::filesystem::file_time_type write_time(std::filesystem::path const& path)
    {
        std::error_code ec;
        auto stamp = std::filesystem::last_write_time(path, ec);
        require(!ec, "failed to read timestamp");
        return stamp;
    }

    inline void copy_directory(std::filesystem::path const& from, std::filesystem::path const& to)
    {
        std::filesystem::create_directories(to);
        for (auto const& entry : std::filesystem::recursive_directory_iterator(from)) {
            auto relative = std::filesystem::relative(entry.path(), from);
            auto target = to / relative;
            if (entry.is_directory()) {
                std::filesystem::create_directories(target);
            } else if (entry.is_regular_file()) {
                std::filesystem::create_directories(target.parent_path());
                std::filesystem::copy_file(entry.path(), target, std::filesystem::copy_options::overwrite_existing);
            }
        }
    }

    template<typename Fn>
    inline void expect_failure(Fn&& fn, std::string const& needle, char const* message)
    {
        try {
            fn();
        } catch (std::exception const& e) {
            require_contains(e.what(), needle, message);
            return;
        }

        std::cerr << message << '\n';
        std::exit(1);
    }

    inline iv::ModuleLoader make_loader(std::vector<std::filesystem::path> extra_roots = { test_modules_root() })
    {
        return iv::ModuleLoader(repo_root(), std::move(extra_roots));
    }

    template<typename Device>
    inline iv::DeviceOrchestrator make_audio_device_provider(Device& audio_device)
    {
        struct RefBackend {
            Device* device = nullptr;

            auto const& config() const
            {
                return device->config();
            }

            std::span<iv::Sample> wait_for_block_request()
            {
                return device->wait_for_block_request();
            }

            void submit_response()
            {
                device->submit_response();
            }
        };

        std::vector<iv::OutputDeviceMixer> mixers;
        mixers.emplace_back(
            iv::LogicalAudioDevice(RefBackend{ &audio_device }),
            [](iv::OrchestratorBuilder& builder, iv::OutputDeviceMixer&& mixer) {
                builder.add_audio_mixer(0, std::move(mixer));
            }
        );
        return iv::DeviceOrchestrator(std::move(mixers));
    }

    template<typename Device>
    inline iv::ModuleRenderConfig module_render_config(Device const& audio_device)
    {
        return iv::ModuleRenderConfig{
            .sample_rate = audio_device.config().sample_rate,
            .num_channels = audio_device.config().num_channels,
            .max_block_frames = audio_device.config().max_block_frames,
        };
    }

    template<typename Device>
    inline iv::ResourceContext make_resource_context(Device const& audio_device)
    {
        iv::ResourceContext resources {};
#if IV_ENABLE_JUCE_VST
        static iv::JuceVstRuntimeManager juce_vst_runtime_manager;
        static std::vector<std::unique_ptr<iv::JuceVstRuntimeSupport>> juce_vst_runtime_supports;
        juce_vst_runtime_supports.push_back(std::make_unique<iv::JuceVstRuntimeSupport>(
            juce_vst_runtime_manager,
            static_cast<double>(audio_device.config().sample_rate)
        ));
        resources = juce_vst_runtime_supports.back()->resources();
#endif
        return resources;
    }

    template<typename Device>
    inline iv::NodeExecutor make_executor(
        Device& audio_device,
        iv::DeviceOrchestrator device_orchestrator,
        iv::ModuleLoader& loader,
        std::filesystem::path const& module_path
    )
    {
        auto graph = loader.load_root(
            module_path,
            module_render_config(audio_device),
            &audio_device.sample_period()
        );

        auto resources = make_resource_context(audio_device);
        auto executor = iv::NodeExecutor::create(
            std::move(graph.root),
            std::move(resources),
            std::move(device_orchestrator).to_builder(),
            std::move(graph.module_refs)
        );
        return executor;
    }

    template<typename Device>
    inline iv::NodeExecutor make_executor(
        Device& audio_device,
        iv::DeviceOrchestrator device_orchestrator,
        [[maybe_unused]] size_t executor_id,
        iv::ModuleLoader& loader,
        std::filesystem::path const& module_path
    )
    {
        return make_executor(audio_device, std::move(device_orchestrator), loader, module_path);
    }

    template<typename Device>
    inline iv::NodeExecutor make_executor(
        iv::ModuleLoader& loader,
        Device& audio_device,
        iv::DeviceOrchestrator device_orchestrator,
        std::filesystem::path const& module_path
    )
    {
        return make_executor(audio_device, std::move(device_orchestrator), loader, module_path);
    }

    template<typename Device>
    inline iv::NodeExecutor make_executor(
        iv::ModuleLoader& loader,
        Device& audio_device,
        iv::DeviceOrchestrator device_orchestrator,
        [[maybe_unused]] size_t executor_id,
        std::filesystem::path const& module_path
    )
    {
        return make_executor(audio_device, std::move(device_orchestrator), loader, module_path);
    }

    template<typename Executor>
    inline void stop_running_executor(FakeAudioDevice& audio_device, Executor& processor, std::future<void>& worker)
    {
        processor.request_shutdown();

        bool posted_unblock_request = false;
        if (worker.wait_for(std::chrono::milliseconds(10)) != std::future_status::ready) {
            audio_device.begin_requested_block(0, 1);
            posted_unblock_request = true;
            require(
                worker.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready,
                "executor did not exit after shutdown"
            );
        }

        worker.get();
        if (posted_unblock_request) {
            audio_device.finish_requested_block();
        }
    }

    template<typename Executor>
    inline void run_processor_ticks(FakeAudioDevice& audio_device, Executor& processor, size_t ticks = 16)
    {
        auto worker = std::async(std::launch::async, [&] {
            processor.execute();
        });

        for (size_t i = 0; i < ticks; ++i) {
            audio_device.begin_requested_block(i, 1);
            require(audio_device.wait_until_block_ready(), "processor tick did not satisfy fake audio request");
            audio_device.finish_requested_block();
        }

        stop_running_executor(audio_device, processor, worker);
    }

    template<typename Executor>
    inline void run_processor_blocks(
        FakeAudioDevice& audio_device,
        Executor& processor,
        std::span<size_t const> block_sizes,
        size_t start_index = 0
    )
    {
        auto worker = std::async(std::launch::async, [&] {
            processor.execute();
        });

        size_t index = start_index;
        for (size_t block_size : block_sizes) {
            audio_device.begin_requested_block(index, block_size);
            require(audio_device.wait_until_block_ready(), "processor block did not satisfy fake audio request");
            audio_device.finish_requested_block();
            index += block_size;
        }

        stop_running_executor(audio_device, processor, worker);
    }

    inline void install_crash_handlers()
    {
        iv::install_crash_handlers();
    }
}
