#pragma once

#include "module/module.h"
#include "runtime/system.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <dlfcn.h>
#endif

namespace iv {
    class ModuleLoader {
    public:
        struct LoadedGraph {
            TypeErasedNode root;
            std::vector<std::shared_ptr<void>> module_refs;
            std::filesystem::path module_path;
            std::filesystem::file_time_type source_stamp {};
            size_t sink_count = 0;

            LoadedGraph(
                TypeErasedNode root_,
                std::vector<std::shared_ptr<void>> module_refs_,
                std::filesystem::path module_path_,
                std::filesystem::file_time_type source_stamp_,
                size_t sink_count_
            ) :
                root(std::move(root_)),
                module_refs(std::move(module_refs_)),
                module_path(std::move(module_path_)),
                source_stamp(source_stamp_),
                sink_count(sink_count_)
            {}
        };

    private:
        struct DynamicLibrary {
#if defined(_WIN32)
            HMODULE handle = nullptr;
#else
            void* handle = nullptr;
#endif

            explicit DynamicLibrary(std::filesystem::path const& path)
            {
#if defined(_WIN32)
                handle = LoadLibraryW(path.c_str());
                if (!handle) {
                    throw std::runtime_error("LoadLibraryW failed for '" + path.string() + "'");
                }
#else
                handle = dlopen(path.c_str(), RTLD_NOW);
                if (!handle) {
                    throw std::runtime_error("dlopen failed for '" + path.string() + "': " + dlerror());
                }
#endif
            }

            ~DynamicLibrary()
            {
#if defined(_WIN32)
                if (handle) {
                    FreeLibrary(handle);
                }
#else
                if (handle) {
                    dlclose(handle);
                }
#endif
            }

            DynamicLibrary(DynamicLibrary&& other) noexcept :
                handle(std::exchange(other.handle, nullptr))
            {}

            DynamicLibrary& operator=(DynamicLibrary&& other) noexcept
            {
                if (this == &other) {
                    return *this;
                }

#if defined(_WIN32)
                if (handle) {
                    FreeLibrary(handle);
                }
#else
                if (handle) {
                    dlclose(handle);
                }
#endif
                handle = std::exchange(other.handle, nullptr);
                return *this;
            }

            DynamicLibrary(DynamicLibrary const&) = delete;
            DynamicLibrary& operator=(DynamicLibrary const&) = delete;

            void* symbol(char const* name) const
            {
#if defined(_WIN32)
                return reinterpret_cast<void*>(GetProcAddress(handle, name));
#else
                return dlsym(handle, name);
#endif
            }
        };

        struct ResolvedModule {
            std::filesystem::path request_path;
            std::filesystem::path module_dir;
            std::filesystem::path entry_file;
            std::filesystem::path cmake_dir;
            bool has_custom_cmake = false;
            std::filesystem::file_time_type source_stamp {};
        };

        struct LoadedBinary {
            std::filesystem::path module_dir;
            std::filesystem::path artifact_path;
            std::shared_ptr<DynamicLibrary> library;
            iv_module_descriptor_v1 const* descriptor = nullptr;
        };

        struct BuildSession {
            ModuleLoader* loader = nullptr;
            System* system = nullptr;
            std::vector<std::shared_ptr<void>> module_refs;
            std::unordered_map<std::filesystem::path, std::shared_ptr<LoadedBinary>> binaries_by_entry;
            std::vector<std::filesystem::path> module_dir_stack;
            size_t sink_count = 0;

            TypeErasedModule load_module(std::filesystem::path const& requested_path)
            {
                std::filesystem::path resolved_request = requested_path;
                if (!module_dir_stack.empty() && requested_path.is_relative()) {
                    resolved_request = module_dir_stack.back() / requested_path;
                }

                ResolvedModule resolved = loader->resolve_module_path(resolved_request);
                auto it = binaries_by_entry.find(resolved.entry_file);
                if (it != binaries_by_entry.end()) {
                    auto binary = it->second;
                    return TypeErasedModule([this, binary](ModuleContext const& context) {
                        struct DirectoryScope {
                            std::vector<std::filesystem::path>& stack;

                            DirectoryScope(std::vector<std::filesystem::path>& stack_, std::filesystem::path path) :
                                stack(stack_)
                            {
                                stack.push_back(std::move(path));
                            }

                            ~DirectoryScope()
                            {
                                stack.pop_back();
                            }
                        } scope(module_dir_stack, binary->module_dir);

                        return binary->descriptor->build(context);
                    });
                }

                std::shared_ptr<LoadedBinary> binary = loader->build_and_load_binary(resolved);
                binaries_by_entry.emplace(resolved.entry_file, binary);
                module_refs.push_back(binary);

                return TypeErasedModule([this, binary](ModuleContext const& context) {
                    struct DirectoryScope {
                        std::vector<std::filesystem::path>& stack;

                        DirectoryScope(std::vector<std::filesystem::path>& stack_, std::filesystem::path path) :
                            stack(stack_)
                        {
                            stack.push_back(std::move(path));
                        }

                        ~DirectoryScope()
                        {
                            stack.pop_back();
                        }
                    } scope(module_dir_stack, binary->module_dir);

                    return binary->descriptor->build(context);
                });
            }
        };

        std::filesystem::path _repo_root;
        std::filesystem::path _core_include_dir;
        std::filesystem::path _third_party_include_dir;
        std::filesystem::path _cache_root;
        mutable std::mutex _build_mutex;

        static TypeErasedModule load_from_context(void* session_ptr, std::string_view path)
        {
            return static_cast<BuildSession*>(session_ptr)->load_module(std::filesystem::path(path));
        }

        static NodeRef sink_from_context(void* session_ptr, GraphBuilder& builder, size_t channel, size_t)
        {
            auto& session = *static_cast<BuildSession*>(session_ptr);
            ++session.sink_count;
            return builder.node<ChannelBufferSink>(session.system->audio_device().output_target(), channel);
        }

        static std::filesystem::path normalize_path(std::filesystem::path const& path)
        {
            std::error_code ec;
            auto canonical = std::filesystem::weakly_canonical(path, ec);
            if (!ec) {
                return canonical;
            }

            return std::filesystem::absolute(path).lexically_normal();
        }

        static std::string sanitize_name(std::filesystem::path const& path)
        {
            std::string text = path.filename().string();
            if (text.empty()) {
                text = "module";
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
        }

        static std::string quote(std::filesystem::path const& path)
        {
            std::string text = path.generic_string();
            std::string escaped;
            escaped.reserve(text.size() + 2);
            escaped.push_back('"');
            for (char c : text) {
                if (c == '"') {
                    escaped += "\\\"";
                } else {
                    escaped.push_back(c);
                }
            }
            escaped.push_back('"');
            return escaped;
        }

        static std::string shared_library_filename(std::string_view base_name)
        {
#if defined(_WIN32)
            return std::string(base_name) + ".dll";
#elif defined(__APPLE__)
            return "lib" + std::string(base_name) + ".dylib";
#else
            return "lib" + std::string(base_name) + ".so";
#endif
        }

        static char const* active_build_config()
        {
#if defined(NDEBUG)
            return "Release";
#else
            return "Debug";
#endif
        }

        static std::filesystem::file_time_type compute_stamp_for_file(std::filesystem::path const& file)
        {
            std::error_code ec;
            auto stamp = std::filesystem::last_write_time(file, ec);
            if (ec) {
                throw std::runtime_error("failed to read timestamp for '" + file.string() + "'");
            }
            return stamp;
        }

        static std::filesystem::file_time_type compute_stamp_for_directory(std::filesystem::path const& dir)
        {
            std::filesystem::file_time_type latest {};
            bool saw_file = false;

            for (auto const& entry : std::filesystem::recursive_directory_iterator(dir)) {
                if (!entry.is_regular_file()) {
                    continue;
                }

                auto stamp = compute_stamp_for_file(entry.path());
                latest = saw_file ? std::max(latest, stamp) : stamp;
                saw_file = true;
            }

            if (!saw_file) {
                return compute_stamp_for_file(dir);
            }

            return latest;
        }

        static std::filesystem::path discover_repo_root(std::filesystem::path start)
        {
            start = normalize_path(start);
            if (std::filesystem::is_regular_file(start)) {
                start = start.parent_path();
            }

            for (auto current = start; !current.empty(); current = current.parent_path()) {
                if (
                    std::filesystem::exists(current / "CMakeLists.txt") &&
                    std::filesystem::exists(current / "intravenous" / "dsl.h")
                ) {
                    return current;
                }

                if (current == current.root_path()) {
                    break;
                }
            }

            throw std::runtime_error("failed to discover repo root from '" + start.string() + "'");
        }

        ResolvedModule resolve_module_path(std::filesystem::path const& requested_path) const
        {
            std::filesystem::path normalized = normalize_path(requested_path);

            if (std::filesystem::is_directory(normalized)) {
                std::filesystem::path entry = normalized / "module.cpp";
                if (!std::filesystem::exists(entry)) {
                    throw std::runtime_error(
                        "module directory '" + normalized.string() + "' does not contain module.cpp"
                    );
                }

                std::filesystem::path cmake = normalized / "CMakeLists.txt";
                return ResolvedModule {
                    .request_path = normalized,
                    .module_dir = normalized,
                    .entry_file = normalize_path(entry),
                    .cmake_dir = normalized,
                    .has_custom_cmake = std::filesystem::exists(cmake),
                    .source_stamp = compute_stamp_for_directory(normalized),
                };
            }

            if (!std::filesystem::exists(normalized)) {
                throw std::runtime_error("module path '" + normalized.string() + "' does not exist");
            }

            if (normalized.extension() != ".cpp") {
                throw std::runtime_error("module file '" + normalized.string() + "' must be a .cpp file");
            }

            std::filesystem::path module_dir = normalized.parent_path();
            std::filesystem::path cmake = module_dir / "CMakeLists.txt";
            return ResolvedModule {
                .request_path = normalized,
                .module_dir = module_dir,
                .entry_file = normalized,
                .cmake_dir = module_dir,
                .has_custom_cmake = std::filesystem::exists(cmake),
                .source_stamp = compute_stamp_for_file(normalized),
            };
        }

        std::filesystem::path write_template_source(
            ResolvedModule const& resolved,
            std::filesystem::path const& source_dir,
            std::filesystem::path const& output_dir,
            std::string const& output_name) const
        {
            std::filesystem::create_directories(source_dir);

            std::ofstream out(source_dir / "CMakeLists.txt", std::ios::binary);
            if (!out) {
                throw std::runtime_error("failed to write generated module CMakeLists.txt");
            }

            out
                << "cmake_minimum_required(VERSION 3.20)\n"
                << "project(iv_runtime_module LANGUAGES CXX)\n"
                << "add_library(iv_runtime_module SHARED " << quote(resolved.entry_file) << ")\n"
                << "target_compile_features(iv_runtime_module PRIVATE cxx_std_23)\n"
                << "set_target_properties(iv_runtime_module PROPERTIES\n"
                << "    CXX_STANDARD 23\n"
                << "    CXX_STANDARD_REQUIRED ON\n"
                << "    CXX_EXTENSIONS OFF\n"
                << "    OUTPUT_NAME " << output_name << "\n"
                << "    RUNTIME_OUTPUT_DIRECTORY " << quote(output_dir) << "\n"
                << "    RUNTIME_OUTPUT_DIRECTORY_DEBUG " << quote(output_dir) << "\n"
                << "    RUNTIME_OUTPUT_DIRECTORY_RELEASE " << quote(output_dir) << "\n"
                << "    LIBRARY_OUTPUT_DIRECTORY " << quote(output_dir) << "\n"
                << "    LIBRARY_OUTPUT_DIRECTORY_DEBUG " << quote(output_dir) << "\n"
                << "    LIBRARY_OUTPUT_DIRECTORY_RELEASE " << quote(output_dir) << "\n"
                << ")\n"
                << "target_include_directories(iv_runtime_module PRIVATE "
                << quote(_core_include_dir) << " " << quote(resolved.module_dir) << ")\n"
                << "target_include_directories(iv_runtime_module SYSTEM PRIVATE "
                << quote(_third_party_include_dir) << ")\n"
                << "if(MSVC)\n"
                << "    target_compile_options(iv_runtime_module PRIVATE /W4 /permissive-)\n"
                << "else()\n"
                << "    target_compile_options(iv_runtime_module PRIVATE -Wall -Wextra -Wpedantic)\n"
                << "endif()\n";

            return source_dir;
        }

        static void run_command(std::string const& command)
        {
            int const result = std::system(command.c_str());
            if (result != 0) {
                throw std::runtime_error("command failed with exit code " + std::to_string(result) + ": " + command);
            }
        }

        std::filesystem::path build_artifact(ResolvedModule const& resolved) const
        {
            std::lock_guard lock(_build_mutex);

            auto generation = std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()
            );

            std::string base_name = sanitize_name(resolved.module_dir);
            std::string output_name = base_name + "_" + generation;
            std::filesystem::path generation_root = _cache_root / base_name / generation;
            std::filesystem::path source_dir = generation_root / "src";
            std::filesystem::path build_dir = generation_root / "build";
            std::filesystem::path output_dir = generation_root / "out";
            std::filesystem::create_directories(output_dir);

            std::filesystem::path configure_source =
                resolved.has_custom_cmake
                    ? resolved.cmake_dir
                    : write_template_source(resolved, source_dir, output_dir, output_name);

            std::ostringstream configure;
            configure
                << "cmake -S " << quote(configure_source)
                << " -B " << quote(build_dir)
                << " -DIV_MODULE_ENTRY_FILE=" << quote(resolved.entry_file)
                << " -DIV_CORE_INCLUDE_DIR=" << quote(_core_include_dir)
                << " -DIV_THIRD_PARTY_INCLUDE_DIR=" << quote(_third_party_include_dir)
                << " -DIV_MODULE_OUTPUT_DIR=" << quote(output_dir)
                << " -DIV_MODULE_OUTPUT_NAME=" << quote(output_name);
            run_command(configure.str());

            std::ostringstream build;
            build << "cmake --build " << quote(build_dir) << " --config " << active_build_config();
            run_command(build.str());

            std::filesystem::path artifact = output_dir / shared_library_filename(output_name);
            std::filesystem::path config_artifact =
                output_dir / active_build_config() / shared_library_filename(output_name);

            if (!std::filesystem::exists(artifact) && std::filesystem::exists(config_artifact)) {
                artifact = config_artifact;
            }

            if (!std::filesystem::exists(artifact)) {
                throw std::runtime_error(
                    "module build did not produce expected artifact '" + artifact.string() + "'"
                );
            }

            return artifact;
        }

        std::shared_ptr<LoadedBinary> build_and_load_binary(ResolvedModule const& resolved) const
        {
            std::filesystem::path artifact = build_artifact(resolved);
            auto library = std::make_shared<DynamicLibrary>(artifact);
            auto* get_descriptor = reinterpret_cast<iv_get_module_descriptor_fn_v1>(
                library->symbol("iv_get_module_descriptor_v1")
            );
            if (!get_descriptor) {
                throw std::runtime_error(
                    "module '" + artifact.string() + "' does not export iv_get_module_descriptor_v1"
                );
            }

            iv_module_descriptor_v1 const* descriptor = get_descriptor();
            if (!descriptor) {
                throw std::runtime_error("module '" + artifact.string() + "' returned a null descriptor");
            }
            if (descriptor->abi_version != IV_MODULE_ABI_VERSION_V1) {
                throw std::runtime_error(
                    "module '" + artifact.string() + "' uses unsupported ABI version " +
                    std::to_string(descriptor->abi_version)
                );
            }
            if (!descriptor->build) {
                throw std::runtime_error("module '" + artifact.string() + "' has no build function");
            }

            return std::make_shared<LoadedBinary>(LoadedBinary {
                .module_dir = resolved.module_dir,
                .artifact_path = artifact,
                .library = std::move(library),
                .descriptor = descriptor,
            });
        }

    public:
        explicit ModuleLoader(std::filesystem::path const& discovery_start = std::filesystem::current_path()) :
            _repo_root(discover_repo_root(discovery_start)),
            _core_include_dir(_repo_root / "intravenous"),
            _third_party_include_dir(_repo_root / "intravenous" / "third_party"),
            _cache_root(_repo_root / "build" / "iv_runtime_modules")
        {
            std::filesystem::create_directories(_cache_root);
        }

        LoadedGraph load_root(std::filesystem::path const& module_path, System& system) const
        {
            ResolvedModule resolved = resolve_module_path(module_path);
            BuildSession session {
                .loader = const_cast<ModuleLoader*>(this),
                .system = &system,
            };
            session.module_dir_stack.push_back(resolved.module_dir);

            GraphBuilder builder;
            ModuleSystem module_system(
                &session,
                ModuleRenderConfig {
                    .sample_rate = system.render_config().sample_rate,
                    .num_channels = system.render_config().num_channels,
                    .max_block_frames = system.render_config().max_block_frames,
                },
                &system.sample_period(),
                &ModuleLoader::sink_from_context
            );
            ModuleContext context(builder, module_system, &ModuleLoader::load_from_context, &session);
            TypeErasedModule module = session.load_module(resolved.request_path);
            return LoadedGraph(
                module.build(context),
                std::move(session.module_refs),
                resolved.request_path,
                resolved.source_stamp,
                session.sink_count
            );
        }

        std::filesystem::file_time_type source_stamp(std::filesystem::path const& module_path) const
        {
            return resolve_module_path(module_path).source_stamp;
        }

        bool source_changed(std::filesystem::path const& module_path, std::filesystem::file_time_type known_stamp) const
        {
            return source_stamp(module_path) != known_stamp;
        }
    };
}
