#pragma once

#include "devices/audio_device.h"
#include "fake_audio_device.h"
#include "module/loader.h"
#include "node/executor.h"
#include "runtime/handlers.h"
#include "runtime/iv_module_definitions.h"
#include "runtime/iv_module_reload.h"
#include "runtime/startup_config.h"
#include "runtime/timeline.h"
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
#include <source_location>
#include <sstream>
#include <string>
#include <thread>

#if defined(_WIN32)
#define NOMINMAX
#include <Windows.h>
#else
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace iv::test {
    inline void copy_directory(std::filesystem::path const& from, std::filesystem::path const& to);

    inline std::filesystem::path repo_root()
    {
        return std::filesystem::path(__FILE__).lexically_normal().parent_path().parent_path();
    }

    inline std::filesystem::path test_modules_root()
    {
        return repo_root() / "tests" / "test_modules";
    }

    inline std::filesystem::path duplicate_modules_root()
    {
        return repo_root() / "tests" / "test_modules_duplicate";
    }

    inline std::string test_process_namespace()
    {
        static std::string cached = [] {
            auto sanitize = [](std::string text) {
                if (text.empty()) {
                    text = "unknown_test";
                }
                for (char& c : text) {
                    bool good =
                        (c >= 'a' && c <= 'z') ||
                        (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9');
                    if (!good) {
                        c = '_';
                    }
                }
                return text;
            };

            std::filesystem::path executable = "unknown_test";
#if defined(_WIN32)
            std::string buffer(MAX_PATH, '\0');
            auto length = GetModuleFileNameA(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (length > 0) {
                buffer.resize(length);
                executable = buffer;
            }
            auto pid = static_cast<unsigned long>(GetCurrentProcessId());
#else
            std::error_code ec;
            executable = std::filesystem::read_symlink("/proc/self/exe", ec);
            auto pid = static_cast<unsigned long>(::getpid());
#endif
            auto name = executable.stem().string();
            if (name.empty()) {
                name = "unknown_test";
            }

            std::ostringstream out;
            out << sanitize(std::move(name)) << "_" << pid;
            return out.str();
        }();
        return cached;
    }

    inline std::filesystem::path runtime_modules_root()
    {
        return repo_root() / "build" / "test_runtime_modules" / test_process_namespace();
    }

    inline std::filesystem::path runtime_module_cache_root()
    {
        return repo_root() / "build" / "iv_runtime_modules";
    }

    inline std::filesystem::path shared_test_fixtures_root()
    {
        return repo_root() / "build" / "test_shared_fixtures";
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

    inline std::string sanitize_test_token(std::string_view text)
    {
        std::string sanitized(text);
        if (sanitized.empty()) {
            sanitized = "test";
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

    inline std::string test_location_id(
        std::string_view base_name,
        std::source_location location = std::source_location::current()
    )
    {
        std::filesystem::path file = location.file_name();
        std::ostringstream out;
        out << sanitize_test_token(base_name)
            << "_"
            << sanitize_test_token(file.stem().string())
            << "_L"
            << location.line();
        return out.str();
    }

    inline std::filesystem::path fresh_test_workspace(
        std::string_view base_name,
        std::source_location location = std::source_location::current()
    )
    {
        auto const workspace = runtime_modules_root() / test_location_id(base_name, location);
        std::filesystem::remove_all(workspace);
        std::filesystem::create_directories(workspace);
        return workspace;
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

    inline std::filesystem::path runtime_module_workspace_root(std::string_view id, std::filesystem::path const& module_dir)
    {
        return runtime_module_cache_root() / (sanitize_module_id(id) + "_" + stable_path_hash(module_dir));
    }

    inline std::filesystem::path runtime_module_workspace(std::string_view id, std::filesystem::path const& module_dir)
    {
        auto const base = runtime_module_cache_root() / (sanitize_module_id(id) + "_" + stable_path_hash(module_dir));
        auto const debug = base / "Debug";
        auto const release = base / "Release";
        auto const has_debug = std::filesystem::exists(debug);
        auto const has_release = std::filesystem::exists(release);
        if (has_debug && !has_release) {
            return debug;
        }
        if (!has_debug && has_release) {
            return release;
        }
        return base / active_build_config();
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

    inline std::string configured_build_generator()
    {
        auto const cache_path = repo_root() / "build" / "CMakeCache.txt";
        std::ifstream in(cache_path);
        require(static_cast<bool>(in), "failed to open top-level CMakeCache.txt");

        for (std::string line; std::getline(in, line);) {
            static std::string const prefix = "CMAKE_GENERATOR:INTERNAL=";
            if (line.starts_with(prefix)) {
                return line.substr(prefix.size());
            }
        }

        require(false, "failed to find CMAKE_GENERATOR in top-level CMakeCache.txt");
        return {};
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

    class ScopedFileLock {
#if !defined(_WIN32)
        int _fd = -1;
#endif

    public:
        explicit ScopedFileLock(std::filesystem::path const& path)
        {
#if !defined(_WIN32)
            std::filesystem::create_directories(path.parent_path());
            _fd = ::open(path.c_str(), O_CREAT | O_RDWR, 0666);
            require(_fd >= 0, "failed to open lock file");
            require(::flock(_fd, LOCK_EX) == 0, "failed to lock file");
#else
            (void)path;
#endif
        }

        ~ScopedFileLock()
        {
#if !defined(_WIN32)
            if (_fd >= 0) {
                ::flock(_fd, LOCK_UN);
                ::close(_fd);
            }
#endif
        }

        ScopedFileLock(ScopedFileLock const&) = delete;
        ScopedFileLock& operator=(ScopedFileLock const&) = delete;
    };

    inline std::filesystem::path copy_fixture_workspace(
        std::string_view test_name,
        std::string const& fixture_name,
        std::source_location location = std::source_location::current())
    {
        auto const workspace = fresh_test_workspace(test_name, location);
        copy_directory(test_modules_root() / fixture_name, workspace);
        return workspace;
    }

    inline std::filesystem::path shared_fixture_workspace(std::string const& fixture_name)
    {
        auto const workspace = shared_test_fixtures_root() / sanitize_test_token(fixture_name);
        auto const lock = ScopedFileLock(shared_test_fixtures_root() / (sanitize_test_token(fixture_name) + ".lock"));
        if (!std::filesystem::exists(workspace)) {
            copy_directory(test_modules_root() / fixture_name, workspace);
        }
        return workspace;
    }

    inline std::filesystem::path shared_project_fixture_workspace(std::string const& fixture_name)
    {
        auto const workspace = shared_fixture_workspace(fixture_name);
        auto const lock = ScopedFileLock(
            shared_test_fixtures_root() /
            (sanitize_test_token(fixture_name) + ".project.lock"));
        write_text(workspace / ".intravenous", "");
        return workspace;
    }

    inline std::filesystem::path make_inline_module_workspace(
        std::string_view test_name,
        std::string const& module_text,
        std::source_location location = std::source_location::current())
    {
        auto const workspace = fresh_test_workspace(test_name, location);
        std::filesystem::create_directories(workspace);
        write_text(workspace / ".intravenous", "");
        write_text(workspace / "module.cpp", module_text);
        return workspace;
    }

    inline std::filesystem::path shared_inline_module_workspace(
        std::string_view test_name,
        std::string const& module_text)
    {
        auto const workspace = shared_test_fixtures_root() / sanitize_test_token(test_name);
        auto const lock = ScopedFileLock(shared_test_fixtures_root() / (sanitize_test_token(test_name) + ".lock"));
        std::filesystem::create_directories(workspace);
        write_text(workspace / ".intravenous", "");
        write_text(workspace / "module.cpp", module_text);
        return workspace;
    }

    inline std::string find_program(std::string const& name)
    {
        std::string command = "command -v " + name;
        FILE* pipe = popen(command.c_str(), "r");
        if (!pipe) {
            return {};
        }

        std::string output;
        char buffer[256];
        while (fgets(buffer, sizeof(buffer), pipe)) {
            output += buffer;
        }
        pclose(pipe);

        while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
            output.pop_back();
        }
        return output;
    }

    inline std::string configured_program_or_find(
        std::string const& name,
        char const* configured_path)
    {
        if (configured_path != nullptr && *configured_path != '\0') {
            return configured_path;
        }
        return find_program(name);
    }

    struct ScopedEnvVar {
        std::string key;
        std::optional<std::string> original;

        ScopedEnvVar(std::string key_, std::string value)
            : key(std::move(key_))
        {
            if (char const* existing = std::getenv(key.c_str())) {
                original = existing;
            }
            setenv(key.c_str(), value.c_str(), 1);
        }

        ~ScopedEnvVar()
        {
            if (original.has_value()) {
                setenv(key.c_str(), original->c_str(), 1);
            } else {
                unsetenv(key.c_str());
            }
        }
    };

    inline iv::RuntimeIvModuleReloadedDefinition load_runtime_iv_module_definition(
        iv::StartupConfigState const& config,
        std::filesystem::path module_root)
    {
        auto const normalized_module_root = std::filesystem::weakly_canonical(module_root).lexically_normal();
        auto const load_lock = ScopedFileLock(
            runtime_module_cache_root() /
            ("load_" + stable_path_hash(normalized_module_root) + ".lock"));
        iv::ModuleLoader loader(
            config.discovery_start,
            config.search_roots,
            config.toolchain);
        iv::RenderConfig const render_config{};
        iv::Sample device_sample_period = iv::sample_period(render_config);
        auto loaded_graph = loader.load_root(
            module_root,
            {
                .sample_rate = render_config.sample_rate,
                .num_channels = render_config.num_channels,
                .max_block_frames = render_config.max_block_frames,
            },
            &device_sample_period);
        return iv::RuntimeIvModuleReloadedDefinition{
            .definition_id = normalized_module_root.string(),
            .module_root = normalized_module_root,
            .module_id = loaded_graph.module_id,
            .introspection = loaded_graph.introspection,
            .dependencies = loaded_graph.dependencies,
            .module_refs = loaded_graph.module_refs,
            .canonical_builder = *loaded_graph.canonical_builder,
        };
    }

    inline iv::RuntimeIvModuleReloadedDefinition make_loaded_definition(
        std::filesystem::path module_root,
        std::string module_id = "iv.test.module",
        iv::GraphIntrospectionMetadata introspection = {},
        std::vector<iv::ModuleDependency> dependencies = {})
    {
        auto const normalized_module_root = std::filesystem::weakly_canonical(module_root).lexically_normal();
        return iv::RuntimeIvModuleReloadedDefinition{
            .definition_id = normalized_module_root.string(),
            .module_root = normalized_module_root,
            .module_id = std::move(module_id),
            .introspection = std::move(introspection),
            .dependencies = std::move(dependencies),
            .module_refs = {},
            .canonical_builder = {},
        };
    }

    inline void advance_write_time(std::filesystem::path const& path, std::chrono::seconds delta = std::chrono::seconds(2))
    {
        std::error_code ec;
        auto const current = std::filesystem::last_write_time(path, ec);
        if (ec) {
            std::cerr << "failed to read timestamp for '" << path.string() << "': " << ec.message() << '\n';
            std::exit(1);
        }
        std::filesystem::last_write_time(path, current + delta, ec);
        if (ec) {
            std::cerr << "failed to advance timestamp for '" << path.string() << "': " << ec.message() << '\n';
            std::exit(1);
        }
    }

    inline void write_text_advancing_timestamp(
        std::filesystem::path const& path,
        std::string const& text,
        std::chrono::seconds delta = std::chrono::seconds(2)
    )
    {
        write_text(path, text);
        advance_write_time(path, delta);
    }

    inline std::filesystem::file_time_type write_time(std::filesystem::path const& path)
    {
        std::error_code ec;
        auto stamp = std::filesystem::last_write_time(path, ec);
        if (ec) {
            std::cerr << "failed to read timestamp for '" << path.string() << "': " << ec.message() << '\n';
            std::exit(1);
        }
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
    inline iv::ModuleExecutorTarget module_executor_target(Device const& audio_device)
    {
        return iv::ModuleExecutorTarget{
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
            module_executor_target(audio_device),
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

namespace iv::test_support {
    using iv::test::configured_program_or_find;
    using iv::test::copy_directory;
    using iv::test::copy_fixture_workspace;
    using iv::test::find_program;
    using iv::test::fresh_test_workspace;
    using iv::test::load_runtime_iv_module_definition;
    using iv::test::make_loaded_definition;
    using iv::test::make_inline_module_workspace;
    using iv::test::read_text;
    using iv::test::ScopedEnvVar;
    using iv::test::shared_fixture_workspace;
    using iv::test::shared_project_fixture_workspace;
    using iv::test::shared_inline_module_workspace;
    using iv::test::test_modules_root;
    using iv::test::write_text;

    inline std::filesystem::path make_workspace(
        std::string_view name,
        std::source_location location = std::source_location::current())
    {
        return iv::test::fresh_test_workspace(name, location);
    }
}
