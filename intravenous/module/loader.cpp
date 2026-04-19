#define IV_INTERNAL_TRANSLATION_UNIT

#include "module/loader.h"
#include "compat.h"

#include "devices/channel_buffer_sink.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace iv {
    namespace {
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
            std::string id;
            std::filesystem::path request_path;
            std::filesystem::path module_dir;
            std::filesystem::path entry_file;
            std::filesystem::path cmake_dir;
            bool has_custom_cmake = false;
            std::filesystem::file_time_type source_stamp {};
        };

        struct LoadedBinary {
            std::string id;
            std::filesystem::path module_dir;
            std::filesystem::path artifact_path;
            std::shared_ptr<DynamicLibrary> library;
            iv_module_descriptor_v1 const* descriptor = nullptr;
        };

        std::filesystem::path normalize_path(std::filesystem::path const& path)
        {
            std::error_code ec;
            auto canonical = std::filesystem::weakly_canonical(path, ec);
            if (!ec) {
                return canonical;
            }

            return std::filesystem::absolute(path).lexically_normal();
        }

        std::string read_text(std::filesystem::path const& path)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in) {
                throw std::runtime_error("failed to open '" + path.string() + "'");
            }
            return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }

        void write_text_if_different(std::filesystem::path const& path, std::string const& text)
        {
            if (std::filesystem::exists(path) && read_text(path) == text) {
                return;
            }

            std::filesystem::create_directories(path.parent_path());
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (!out) {
                throw std::runtime_error("failed to write '" + path.string() + "'");
            }
            out << text;
        }

        void copy_file_if_different(std::filesystem::path const& from, std::filesystem::path const& to)
        {
            write_text_if_different(to, read_text(from));
        }

        std::optional<std::string> parse_exported_module_id(std::filesystem::path const& file)
        {
            std::string text = read_text(file);
            std::string const token = "IV_EXPORT_MODULE";
            size_t token_pos = text.find(token);
            if (token_pos == std::string::npos) {
                return std::nullopt;
            }

            size_t quote_start = text.find('"', token_pos);
            if (quote_start == std::string::npos) {
                return std::nullopt;
            }

            size_t quote_end = quote_start + 1;
            while (quote_end < text.size()) {
                if (text[quote_end] == '"' && text[quote_end - 1] != '\\') {
                    break;
                }
                ++quote_end;
            }

            if (quote_end >= text.size()) {
                return std::nullopt;
            }

            return text.substr(quote_start + 1, quote_end - quote_start - 1);
        }

        std::filesystem::file_time_type compute_stamp_for_file(std::filesystem::path const& file)
        {
            std::error_code ec;
            auto stamp = std::filesystem::last_write_time(file, ec);
            if (ec) {
                throw std::runtime_error("failed to read timestamp for '" + file.string() + "'");
            }
            return stamp;
        }

        std::filesystem::file_time_type compute_stamp_for_directory(std::filesystem::path const& dir)
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

        std::filesystem::path discover_repo_root(std::filesystem::path start)
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

        std::string quote(std::filesystem::path const& path)
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

        std::string sanitize_identifier(std::string_view text)
        {
            std::string sanitized(text);
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

        std::string stable_path_hash(std::filesystem::path const& path)
        {
            std::string const text = normalize_path(path).generic_string();
            uint64_t hash = 1469598103934665603ull;
            for (unsigned char c : text) {
                hash ^= c;
                hash *= 1099511628211ull;
            }

            std::ostringstream out;
            out << std::hex << hash;
            return out.str();
        }

        std::string shared_library_filename(std::string_view base_name)
        {
#if defined(_WIN32)
            return std::string(base_name) + ".dll";
#elif defined(__APPLE__)
            return "lib" + std::string(base_name) + ".dylib";
#else
            return "lib" + std::string(base_name) + ".so";
#endif
        }

        std::vector<std::filesystem::path> path_entries()
        {
            char const* value = std::getenv("PATH");
            if (!value || !*value) {
                return {};
            }

#if defined(_WIN32)
            constexpr char separator = ';';
#else
            constexpr char separator = ':';
#endif

            std::vector<std::filesystem::path> entries;
            std::string_view remaining(value);
            while (!remaining.empty()) {
                size_t split = remaining.find(separator);
                std::string_view token = remaining.substr(0, split);
                if (!token.empty()) {
                    entries.emplace_back(std::string(token));
                }
                if (split == std::string_view::npos) {
                    break;
                }
                remaining.remove_prefix(split + 1);
            }

            return entries;
        }

        std::optional<std::filesystem::path> find_program_on_path(std::string_view name)
        {
#if defined(_WIN32)
            std::vector<std::string> suffixes;
            char const* pathext = std::getenv("PATHEXT");
            if (pathext && *pathext) {
                std::string_view remaining(pathext);
                while (!remaining.empty()) {
                    size_t split = remaining.find(';');
                    std::string_view token = remaining.substr(0, split);
                    if (!token.empty()) {
                        suffixes.emplace_back(token);
                    }
                    if (split == std::string_view::npos) {
                        break;
                    }
                    remaining.remove_prefix(split + 1);
                }
            }
            if (suffixes.empty()) {
                suffixes = { ".exe", ".cmd", ".bat" };
            }
#endif

            for (auto const& entry : path_entries()) {
                std::filesystem::path base = entry / std::string(name);
                std::error_code ec;
                if (std::filesystem::exists(base, ec)) {
                    return base;
                }
#if defined(_WIN32)
                for (auto const& suffix : suffixes) {
                    std::filesystem::path candidate = base;
                    candidate += suffix;
                    if (std::filesystem::exists(candidate, ec)) {
                        return candidate;
                    }
                }
#endif
            }

            return std::nullopt;
        }

        char const* active_build_config()
        {
#if defined(NDEBUG)
            return "Release";
#else
            return "Debug";
#endif
        }

        void run_command(std::string const& command, ModuleLoader::LogSink const& log_sink = {})
        {
            auto capture_path = []() {
                auto path = std::filesystem::temp_directory_path() / "intravenous_command_XXXXXX";
#if defined(_WIN32)
                path += ".log";
                return path;
#else
                std::string templ = path.string();
                std::vector<char> buffer(templ.begin(), templ.end());
                buffer.push_back('\0');
                int fd = mkstemp(buffer.data());
                if (fd < 0) {
                    throw std::runtime_error("failed to create temporary command log file");
                }
                close(fd);
                return std::filesystem::path(buffer.data());
#endif
            };

            auto const log_path = capture_path();
            {
                auto& out = diagnostic_stream();
                out << "running command: " << command << '\n';
                out.flush();
            }
            if (log_sink) {
                log_sink("running command: " + command);
            }
            std::string const redirected =
                command + " > " + quote(log_path) + " 2>&1";
            int const result = std::system(redirected.c_str());
            std::string output;
            try {
                output = read_text(log_path);
            } catch (...) {
            }
            if (!output.empty()) {
                auto& out = diagnostic_stream();
                out << output;
                if (output.back() != '\n') {
                    out << '\n';
                }
                out.flush();
                if (log_sink) {
                    log_sink(output);
                }
            }
            std::error_code ec;
            std::filesystem::remove(log_path, ec);
            if (result != 0) {
                if (!output.empty()) {
                    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
                        output.pop_back();
                    }
                    throw std::runtime_error(
                        "command failed with exit code " + std::to_string(result) + ": " + command + "\n" + output
                    );
                }

                throw std::runtime_error("command failed with exit code " + std::to_string(result) + ": " + command);
            }
        }

        std::filesystem::file_time_type compute_module_build_stamp(std::filesystem::path const& dir)
        {
            std::filesystem::file_time_type latest {};
            bool saw_file = false;

            std::filesystem::path local_cmake = normalize_path(dir / "CMakeLists.txt");
            for (auto const& entry : std::filesystem::recursive_directory_iterator(dir)) {
                if (!entry.is_regular_file()) {
                    continue;
                }
                if (normalize_path(entry.path()) == local_cmake) {
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
    }

    class ModuleLoader::Impl {
        struct BuildWorkspace {
            std::filesystem::path root;
            std::filesystem::path source_dir;
            std::filesystem::path build_dir;
            std::filesystem::path output_dir;
            std::filesystem::path generations_dir;
            std::filesystem::path configure_signature_file;
            std::filesystem::path build_signature_file;
            std::filesystem::path generator_file;
        };

        struct BuildSession {
            Impl* impl = nullptr;
            std::vector<ModuleRef> module_refs;
            std::vector<ModuleDependency> dependencies;
            std::unordered_map<std::string, std::shared_ptr<LoadedBinary>> binaries_by_id;
            std::unordered_map<std::string, ResolvedModule> registry;
            std::vector<std::filesystem::path> search_roots;
            std::unordered_set<std::string> seen_dependencies;
            size_t sink_count = 0;

            TypeErasedModule load_module(std::string_view id)
            {
                auto it = registry.find(std::string(id));
                if (it == registry.end()) {
                    std::vector<std::string> known_ids;
                    known_ids.reserve(registry.size());
                    for (auto const& [known_id, _] : registry) {
                        known_ids.push_back(known_id);
                    }
                    std::sort(known_ids.begin(), known_ids.end());

                    std::ostringstream error;
                    error << "unknown module id '" << id << "'";
                    if (!search_roots.empty()) {
                        error << " in search roots:";
                        for (auto const& root : search_roots) {
                            error << "\n  " << root.string();
                        }
                    }
                    if (!known_ids.empty()) {
                        error << "\nknown module ids:";
                        size_t count = 0;
                        for (auto const& known_id : known_ids) {
                            error << "\n  " << known_id;
                            if (++count == 8) {
                                if (known_ids.size() > count) {
                                    error << "\n  ...";
                                }
                                break;
                            }
                        }
                    }
                    throw std::runtime_error(error.str());
                }

                ResolvedModule const& resolved = it->second;
                if (!seen_dependencies.contains(resolved.id)) {
                    dependencies.push_back(ModuleDependency {
                        .id = resolved.id,
                        .module_dir = resolved.module_dir,
                        .entry_file = resolved.entry_file,
                        .source_stamp = resolved.source_stamp,
                    });
                    seen_dependencies.insert(resolved.id);
                }

                if (auto found = binaries_by_id.find(resolved.id); found != binaries_by_id.end()) {
                    auto binary = found->second;
                    return TypeErasedModule([binary](ModuleContext const& context) {
                        if (char const* error = binary->descriptor->build(context)) {
                            throw std::runtime_error(error);
                        }
                    });
                }

                auto binary = impl->build_and_load_binary(resolved);
                binaries_by_id.emplace(resolved.id, binary);
                module_refs.push_back(binary);
                return TypeErasedModule([binary](ModuleContext const& context) {
                    if (char const* error = binary->descriptor->build(context)) {
                        throw std::runtime_error(error);
                    }
                });
            }

            void ensure_loaded_binary_dependencies()
            {
                for (auto const& [id, _] : binaries_by_id) {
                    if (seen_dependencies.contains(id)) {
                        continue;
                    }

                    auto resolved = registry.find(id);
                    if (resolved == registry.end()) {
                        continue;
                    }

                    dependencies.push_back(ModuleDependency {
                        .id = resolved->second.id,
                        .module_dir = resolved->second.module_dir,
                        .entry_file = resolved->second.entry_file,
                        .source_stamp = resolved->second.source_stamp,
                    });
                    seen_dependencies.insert(id);
                }
            }
        };

    public:
        std::filesystem::path repo_root;
        std::filesystem::path core_include_dir;
        std::filesystem::path third_party_include_dir;
        std::filesystem::path cache_root;
        std::filesystem::path default_template_path;
        std::filesystem::path default_pch_path;
        std::vector<std::filesystem::path> extra_search_roots;
        Timeline& timeline;
        ToolchainConfig toolchain;
        LogSink log_sink;
        mutable std::mutex build_mutex;

        explicit Impl(
            Timeline& timeline_,
            std::filesystem::path discovery_start,
            std::vector<std::filesystem::path> extra_roots,
            ToolchainConfig toolchain_,
            LogSink log_sink_
        ) :
            repo_root(discover_repo_root(std::move(discovery_start))),
            core_include_dir(repo_root / "intravenous"),
            third_party_include_dir(repo_root / "intravenous" / "third_party"),
            cache_root(repo_root / "build" / "iv_runtime_modules"),
            default_template_path(repo_root / "intravenous" / "module" / "template" / "CMakeLists.txt"),
            default_pch_path(repo_root / "intravenous" / "module" / "template" / "module_pch.h"),
            timeline(timeline_),
            toolchain(std::move(toolchain_)),
            log_sink(std::move(log_sink_))
        {
            std::filesystem::create_directories(cache_root);
            for (auto& root : extra_roots) {
                extra_search_roots.push_back(normalize_path(root));
            }
        }

        static TypeErasedModule load_from_context(void* session_ptr, std::string_view id)
        {
            return static_cast<BuildSession*>(session_ptr)->load_module(id);
        }

        static NodeRef sink_from_context(void* session_ptr, GraphBuilder& builder, size_t channel, size_t device_id)
        {
            auto& session = *static_cast<BuildSession*>(session_ptr);
            ++session.sink_count;
            return builder.node<AudioDeviceSink>(AudioDeviceSink{
                .device_id = device_id,
                .channel = channel,
            });
        }

        static NodeRef file_from_context(void*, GraphBuilder& builder, size_t channel, std::filesystem::path const& path)
        {
            return builder.node<FileSink>(FileSink{
                .path = path,
                .channel = channel,
            });
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

                auto id = parse_exported_module_id(entry);
                if (!id.has_value()) {
                    throw std::runtime_error(
                        "module '" + entry.string() + "' does not declare IV_EXPORT_MODULE(\"id\", ...)"
                    );
                }

                std::filesystem::path cmake = normalized / "CMakeLists.txt";
                return ResolvedModule {
                    .id = *id,
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

            auto id = parse_exported_module_id(normalized);
            if (!id.has_value()) {
                throw std::runtime_error(
                    "module '" + normalized.string() + "' does not declare IV_EXPORT_MODULE(\"id\", ...)"
                );
            }

            std::filesystem::path module_dir = normalized.parent_path();
            std::filesystem::path cmake = module_dir / "CMakeLists.txt";
            return ResolvedModule {
                .id = *id,
                .request_path = normalized,
                .module_dir = module_dir,
                .entry_file = normalized,
                .cmake_dir = module_dir,
                .has_custom_cmake = std::filesystem::exists(cmake),
                .source_stamp = compute_stamp_for_directory(module_dir),
            };
        }

        std::unordered_map<std::string, ResolvedModule> discover_registry(std::vector<std::filesystem::path> const& search_roots) const
        {
            std::unordered_map<std::string, ResolvedModule> registry;

            for (auto const& search_root : search_roots) {
                if (!std::filesystem::exists(search_root)) {
                    continue;
                }

                for (auto const& entry : std::filesystem::recursive_directory_iterator(search_root)) {
                    if (!entry.is_regular_file() || entry.path().filename() != "module.cpp") {
                        continue;
                    }

                    if (!parse_exported_module_id(entry.path()).has_value()) {
                        continue;
                    }

                    ResolvedModule resolved = resolve_module_path(entry.path().parent_path());
                    if (auto it = registry.find(resolved.id); it != registry.end()) {
                        if (it->second.entry_file != resolved.entry_file) {
                            throw std::runtime_error(
                                "duplicate module id '" + resolved.id + "' in '" +
                                it->second.entry_file.string() + "' and '" + resolved.entry_file.string() + "'"
                            );
                        }
                    } else {
                        registry.emplace(resolved.id, std::move(resolved));
                    }
                }
            }

            return registry;
        }

        std::filesystem::path module_workspace(std::string_view id, std::filesystem::path const& module_dir) const
        {
            return cache_root / (sanitize_identifier(id) + "_" + stable_path_hash(module_dir)) / active_build_config();
        }

        BuildWorkspace build_workspace_for(ResolvedModule const& resolved) const
        {
            std::filesystem::path root = module_workspace(resolved.id, resolved.module_dir);
            return BuildWorkspace {
                .root = root,
                .source_dir = root / "src",
                .build_dir = root / "build",
                .output_dir = root / "out",
                .generations_dir = root / "generations",
                .configure_signature_file = root / "configure.signature",
                .build_signature_file = root / "build.signature",
                .generator_file = root / "generator.txt",
            };
        }

        std::string configure_signature(ResolvedModule const& resolved, std::filesystem::path const& output_dir, std::string const& output_name) const
        {
            auto const [c_compiler, cxx_compiler] = preferred_module_compilers();
            auto const cmake_program = preferred_cmake_program();
            auto const cmake_generator = preferred_cmake_generator();
            auto const make_program = preferred_make_program();
            auto const juce_dir = preferred_juce_dir();

            std::ostringstream sig;
            sig
                << "id=" << resolved.id << '\n'
                << "entry=" << resolved.entry_file.generic_string() << '\n'
                << "module_dir=" << resolved.module_dir.generic_string() << '\n'
                << "custom=" << resolved.has_custom_cmake << '\n'
                << "template_stamp=" << compute_stamp_for_file(default_template_path).time_since_epoch().count() << '\n'
                << "module_support_stamp=" << compute_stamp_for_file(repo_root / "intravenous" / "module" / "template" / "ModuleSupport.cmake").time_since_epoch().count() << '\n'
                << "juce_support_stamp=" << compute_stamp_for_file(repo_root / "intravenous" / "module" / "template" / "JuceSupport.cmake").time_since_epoch().count() << '\n'
                << "source_span_rewrite_stamp=" << compute_stamp_for_file(repo_root / "intravenous" / "module" / "template" / "SourceSpanRewrite.cmake").time_since_epoch().count() << '\n'
                << "custom_stamp=" << (resolved.has_custom_cmake ? compute_stamp_for_file(resolved.cmake_dir / "CMakeLists.txt").time_since_epoch().count() : 0) << '\n'
                << "core_runtime_library=" << IV_CONFIGURED_CORE_RUNTIME_LIBRARY << '\n'
                << "core_enable_juce_vst=" << IV_CONFIGURED_ENABLE_JUCE_VST << '\n'
                << "juce_web_browser=" << IV_CONFIGURED_JUCE_WEB_BROWSER << '\n'
                << "juce_use_curl=" << IV_CONFIGURED_JUCE_USE_CURL << '\n'
                << "c_compiler=" << c_compiler.generic_string() << '\n'
                << "cxx_compiler=" << cxx_compiler.generic_string() << '\n'
                << "cmake_program=" << cmake_program.generic_string() << '\n'
                << "cmake_generator=" << cmake_generator << '\n'
                << "make_program=" << make_program.generic_string() << '\n'
                << "source_span_rewriter=" << IV_CONFIGURED_CLANG_SOURCE_SPAN_REWRITER << '\n'
                << "juce_dir=" << juce_dir.generic_string() << '\n'
                << "juce_modules_dir=" << IV_CONFIGURED_JUCE_MODULES_DIR << '\n'
                << "output_name=" << output_name << '\n'
                << "output_dir=" << output_dir.generic_string() << '\n';
            return sig.str();
        }

        std::string build_signature(ResolvedModule const& resolved) const
        {
            std::ostringstream sig;
            sig
                << "id=" << resolved.id << '\n'
                << "build_stamp=" << compute_module_build_stamp(resolved.module_dir).time_since_epoch().count() << '\n'
                << "core_stamp=" << compute_stamp_for_directory(core_include_dir).time_since_epoch().count() << '\n';
            return sig.str();
        }

        std::filesystem::path ensure_default_template_workspace(std::filesystem::path const& source_dir) const
        {
            std::filesystem::create_directories(source_dir);
            std::filesystem::path const template_dir = repo_root / "intravenous" / "module" / "template";
            copy_file_if_different(default_template_path, source_dir / "CMakeLists.txt");
            copy_file_if_different(template_dir / "ModuleSupport.cmake", source_dir / "ModuleSupport.cmake");
            copy_file_if_different(template_dir / "JuceSupport.cmake", source_dir / "JuceSupport.cmake");
            copy_file_if_different(template_dir / "SourceSpanRewrite.cmake", source_dir / "SourceSpanRewrite.cmake");
            return source_dir;
        }

        std::optional<std::string> configured_generator(BuildWorkspace const& workspace) const
        {
            if (std::filesystem::exists(workspace.generator_file)) {
                return read_text(workspace.generator_file);
            }

            std::filesystem::path cache_path = workspace.build_dir / "CMakeCache.txt";
            if (!std::filesystem::exists(cache_path)) {
                return std::nullopt;
            }

            std::ifstream in(cache_path);
            std::string line;
            while (std::getline(in, line)) {
                static std::string const prefix = "CMAKE_GENERATOR:INTERNAL=";
                if (line.starts_with(prefix)) {
                    return line.substr(prefix.size());
                }
            }

            return std::nullopt;
        }

        std::string preferred_cmake_generator() const
        {
            if (toolchain.cmake_generator.has_value()) {
                return *toolchain.cmake_generator;
            }

            if (std::string_view(IV_CONFIGURED_CMAKE_GENERATOR).size() != 0) {
                return IV_CONFIGURED_CMAKE_GENERATOR;
            }

            return {};
        }

        std::filesystem::path existing_program_or_empty(std::optional<std::filesystem::path> const& configured) const
        {
            if (!configured.has_value()) {
                return {};
            }
            if (!std::filesystem::exists(*configured)) {
                throw std::runtime_error("configured program does not exist: " + configured->string());
            }
            return *configured;
        }

        std::filesystem::path preferred_cmake_program() const
        {
            if (auto configured = existing_program_or_empty(toolchain.cmake_program); !configured.empty()) {
                return configured;
            }

            if (std::string_view(IV_CONFIGURED_CMAKE_COMMAND).size() != 0) {
                return std::filesystem::path(IV_CONFIGURED_CMAKE_COMMAND);
            }

            if (auto found = find_program_on_path("cmake"); found.has_value()) {
                return *found;
            }

            throw std::runtime_error("runtime module configure requires cmake, but it was not found");
        }

        std::filesystem::path preferred_make_program() const
        {
            if (auto configured = existing_program_or_empty(toolchain.make_program); !configured.empty()) {
                return configured;
            }

            if (std::string_view(IV_CONFIGURED_MAKE_PROGRAM).size() != 0) {
                std::filesystem::path configured = IV_CONFIGURED_MAKE_PROGRAM;
                if (!configured.empty() && std::filesystem::exists(configured)) {
                    return configured;
                }
            }

            return {};
        }

        std::filesystem::path preferred_juce_dir() const
        {
            if (toolchain.juce_dir.has_value()) {
                return *toolchain.juce_dir;
            }

            if (std::string_view(IV_CONFIGURED_JUCE_DIR).size() != 0) {
                return std::filesystem::path(IV_CONFIGURED_JUCE_DIR);
            }

            return {};
        }

        std::pair<std::filesystem::path, std::filesystem::path> preferred_module_compilers() const
        {
            if (toolchain.c_compiler.has_value() || toolchain.cxx_compiler.has_value()) {
                if (!toolchain.c_compiler.has_value() || !toolchain.cxx_compiler.has_value()) {
                    throw std::runtime_error("runtime module toolchain override requires both cCompiler and cxxCompiler");
                }
                if (!std::filesystem::exists(*toolchain.c_compiler)) {
                    throw std::runtime_error("configured C compiler does not exist: " + toolchain.c_compiler->string());
                }
                if (!std::filesystem::exists(*toolchain.cxx_compiler)) {
                    throw std::runtime_error("configured C++ compiler does not exist: " + toolchain.cxx_compiler->string());
                }
                return {*toolchain.c_compiler, *toolchain.cxx_compiler};
            }

            if (
                std::string_view(IV_CONFIGURED_C_COMPILER).size() != 0 &&
                std::string_view(IV_CONFIGURED_CXX_COMPILER).size() != 0 &&
                std::filesystem::exists(std::filesystem::path(IV_CONFIGURED_C_COMPILER)) &&
                std::filesystem::exists(std::filesystem::path(IV_CONFIGURED_CXX_COMPILER))
            ) {
                return {
                    std::filesystem::path(IV_CONFIGURED_C_COMPILER),
                    std::filesystem::path(IV_CONFIGURED_CXX_COMPILER),
                };
            }

            auto clangxx = find_program_on_path("clang++");
            if (!clangxx.has_value()) {
                throw std::runtime_error(
                    "runtime module configure requires clang++, but it was not found on PATH"
                );
            }

            auto clang = find_program_on_path("clang");
            if (!clang.has_value()) {
                throw std::runtime_error(
                    "runtime module configure requires clang, but it was not found on PATH"
                );
            }

            return {*clang, *clangxx};
        }

        std::string choose_generator(BuildWorkspace const& workspace, bool should_configure) const
        {
            if (should_configure) {
                return preferred_cmake_generator();
            }

            if (auto configured = configured_generator(workspace); configured.has_value() && !configured->empty()) {
                return *configured;
            }

            return {};
        }

        void configure_workspace(
            BuildWorkspace const& workspace,
            ResolvedModule const& resolved,
            std::filesystem::path const& configure_source,
            std::string const& output_name,
            std::string const& configure_signature_text,
            std::string const& generator
        ) const
        {
            auto const [c_compiler, cxx_compiler] = preferred_module_compilers();

            if (!generator.empty()) {
                if (
                    auto existing = configured_generator(workspace);
                    existing.has_value() &&
                    *existing != generator &&
                    std::filesystem::exists(workspace.build_dir / "CMakeCache.txt")
                ) {
                    std::filesystem::remove_all(workspace.build_dir);
                }
            }

            std::ostringstream configure;
            configure
                << quote(preferred_cmake_program()) << " -S " << quote(configure_source)
                << " -B " << quote(workspace.build_dir);
            if (!generator.empty()) {
                configure << " -G " << quote(std::filesystem::path(generator));
                auto const make_program = preferred_make_program();
                if (!make_program.empty()) {
                    configure << " -DCMAKE_MAKE_PROGRAM=" << quote(make_program);
                }
            }
            configure
                << " -DCMAKE_BUILD_TYPE=" << active_build_config()
                << " -DCMAKE_C_COMPILER=" << quote(c_compiler)
                << " -DCMAKE_CXX_COMPILER=" << quote(cxx_compiler)
                << " -DIV_MODULE_ENTRY_FILE=" << quote(resolved.entry_file)
                << " -DIV_MODULE_SOURCE_DIR=" << quote(resolved.module_dir)
                << " -DIV_CORE_INCLUDE_DIR=" << quote(core_include_dir)
                << " -DIV_THIRD_PARTY_INCLUDE_DIR=" << quote(third_party_include_dir)
                << " -DIV_CORE_ENABLE_JUCE_VST=" << IV_CONFIGURED_ENABLE_JUCE_VST
                << " -DJUCE_WEB_BROWSER=" << IV_CONFIGURED_JUCE_WEB_BROWSER
                << " -DJUCE_USE_CURL=" << IV_CONFIGURED_JUCE_USE_CURL
                << " -DIV_MODULE_OUTPUT_DIR=" << quote(workspace.output_dir)
                << " -DIV_MODULE_OUTPUT_NAME=" << quote(std::filesystem::path(output_name))
                << " -DIV_MODULE_PCH_HEADER=" << quote(default_pch_path);
            if (std::string_view(IV_CONFIGURED_CORE_RUNTIME_LIBRARY).size() != 0) {
                configure << " -DIV_CORE_RUNTIME_LIBRARY=" << quote(std::filesystem::path(IV_CONFIGURED_CORE_RUNTIME_LIBRARY));
            }
            if (std::string_view(IV_CONFIGURED_CLANG_SOURCE_SPAN_REWRITER).size() != 0) {
                configure << " -DIV_SOURCE_SPAN_REWRITER=" << quote(std::filesystem::path(IV_CONFIGURED_CLANG_SOURCE_SPAN_REWRITER));
            }
            if (std::string_view(IV_CONFIGURED_JUCE_MODULES_DIR).size() != 0) {
                configure << " -DIV_JUCE_MODULES_DIR=" << quote(std::filesystem::path(IV_CONFIGURED_JUCE_MODULES_DIR));
            }
            if (auto juce_dir = preferred_juce_dir(); !juce_dir.empty()) {
                configure << " -DJUCE_DIR=" << quote(juce_dir);
            }
            run_command(configure.str(), log_sink);

            write_text_if_different(workspace.configure_signature_file, configure_signature_text);
            write_text_if_different(workspace.generator_file, generator);
        }

        void build_workspace(BuildWorkspace const& workspace, std::string const&) const
        {
            std::ostringstream build;
            build
                << quote(preferred_cmake_program()) << " --build " << quote(workspace.build_dir)
                << " --config " << active_build_config()
                << " --parallel";
            run_command(build.str(), log_sink);
        }

        std::filesystem::path build_artifact(ResolvedModule const& resolved) const
        {
            std::lock_guard lock(build_mutex);

            std::string output_name = sanitize_identifier(resolved.id);
            BuildWorkspace workspace = build_workspace_for(resolved);
            std::filesystem::create_directories(workspace.output_dir);
            std::filesystem::create_directories(workspace.generations_dir);

            std::filesystem::path configure_source =
                resolved.has_custom_cmake
                    ? resolved.cmake_dir
                    : ensure_default_template_workspace(workspace.source_dir);

            std::string configure_signature_text = configure_signature(resolved, workspace.output_dir, output_name);
            bool should_configure = !std::filesystem::exists(workspace.build_dir / "CMakeCache.txt");
            if (!should_configure) {
                should_configure =
                    !std::filesystem::exists(workspace.configure_signature_file) ||
                    read_text(workspace.configure_signature_file) != configure_signature_text;
            }

            std::string generator = choose_generator(workspace, should_configure);
            if (should_configure) {
                configure_workspace(
                    workspace,
                    resolved,
                    configure_source,
                    output_name,
                    configure_signature_text,
                    generator
                );
            }

            std::filesystem::path stable_artifact = workspace.output_dir / shared_library_filename(output_name);
            std::filesystem::path config_artifact =
                workspace.output_dir / active_build_config() / shared_library_filename(output_name);
            if (!std::filesystem::exists(stable_artifact) && std::filesystem::exists(config_artifact)) {
                stable_artifact = config_artifact;
            }

            std::string build_signature_text = build_signature(resolved);
            bool should_build = should_configure || !std::filesystem::exists(stable_artifact);
            if (!should_build) {
                should_build =
                    !std::filesystem::exists(workspace.build_signature_file) ||
                    read_text(workspace.build_signature_file) != build_signature_text;
            }

            if (should_build) {
                build_workspace(workspace, generator);
                write_text_if_different(workspace.build_signature_file, build_signature_text);
                stable_artifact = workspace.output_dir / shared_library_filename(output_name);
                config_artifact = workspace.output_dir / active_build_config() / shared_library_filename(output_name);
                if (!std::filesystem::exists(stable_artifact) && std::filesystem::exists(config_artifact)) {
                    stable_artifact = config_artifact;
                }
            }

            if (!std::filesystem::exists(stable_artifact)) {
                throw std::runtime_error(
                    "module build did not produce expected artifact '" + stable_artifact.string() + "'"
                );
            }

            std::string generation = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
            std::filesystem::path generation_dir = workspace.generations_dir / generation;
            std::filesystem::create_directories(generation_dir);
            std::filesystem::path generation_artifact = generation_dir / stable_artifact.filename();
            std::filesystem::copy_file(stable_artifact, generation_artifact, std::filesystem::copy_options::overwrite_existing);
            return generation_artifact;
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
            if (!descriptor->id || std::strcmp(descriptor->id, resolved.id.c_str()) != 0) {
                throw std::runtime_error("module '" + artifact.string() + "' exported unexpected id");
            }
            if (!descriptor->build) {
                throw std::runtime_error("module '" + artifact.string() + "' has no build function");
            }

            return std::make_shared<LoadedBinary>(LoadedBinary {
                .id = resolved.id,
                .module_dir = resolved.module_dir,
                .artifact_path = artifact,
                .library = std::move(library),
                .descriptor = descriptor,
            });
        }

        LoadedGraph load_root(
            std::filesystem::path const& module_path,
            ModuleRenderConfig render_config,
            Sample* sample_period
        ) const
        {
            ResolvedModule root = resolve_module_path(module_path);
            std::vector<std::filesystem::path> search_roots { root.module_dir };
            for (auto const& extra : extra_search_roots) {
                if (std::find(search_roots.begin(), search_roots.end(), extra) == search_roots.end()) {
                    search_roots.push_back(extra);
                }
            }

            BuildSession session;
            session.impl = const_cast<Impl*>(this);
            session.registry = discover_registry(search_roots);
            session.search_roots = search_roots;

            GraphBuilder builder;
            ModuleTargetFactory target_factory(
                &session,
                &Impl::sink_from_context,
                &Impl::file_from_context
            );
            ModuleContext context(
                builder,
                target_factory,
                render_config,
                sample_period,
                &Impl::load_from_context,
                &session
            );
            TypeErasedModule root_module = session.load_module(root.id);
            GraphBuilder::BuildResult built_root = [&]() -> GraphBuilder::BuildResult {
                try {
                    GraphBuilder root_builder = root_module.builder(context);
                    root_builder.augment(timeline);
                    return root_builder.build_with_metadata();
                } catch (std::exception const& e) {
                    throw std::runtime_error(wrap_exception(
                        "failed to build root module '" + root.id + "' from '" + root.request_path.string() + "'",
                        e
                    ));
                } catch (...) {
                    throw std::runtime_error(
                        "failed to build root module '" + root.id + "' from '" + root.request_path.string() + "'"
                    );
                }
            }();
            session.ensure_loaded_binary_dependencies();

            return LoadedGraph(
                std::move(built_root.graph),
                std::move(session.module_refs),
                std::move(built_root.introspection),
                root.request_path,
                root.id,
                std::move(session.dependencies),
                session.sink_count
            );
        }
    };

    ModuleLoader::LoadedGraph::LoadedGraph(
        TypeErasedNode root_,
        std::vector<ModuleRef> module_refs_,
        GraphIntrospectionMetadata introspection_,
        std::filesystem::path module_path_,
        std::string module_id_,
        std::vector<ModuleDependency> dependencies_,
        size_t sink_count_
    ) :
        module_refs(std::move(module_refs_)),
        root(std::move(root_)),
        introspection(std::move(introspection_)),
        module_path(std::move(module_path_)),
        module_id(std::move(module_id_)),
        dependencies(std::move(dependencies_)),
        sink_count(sink_count_)
    {}

    ModuleLoader::ModuleLoader(
        Timeline& timeline,
        std::filesystem::path discovery_start,
        std::vector<std::filesystem::path> extra_search_roots,
        ToolchainConfig toolchain,
        LogSink log_sink
    ) :
        _impl(std::make_unique<Impl>(
            timeline,
            std::move(discovery_start),
            std::move(extra_search_roots),
            std::move(toolchain),
            std::move(log_sink)
        ))
    {}

    ModuleLoader::~ModuleLoader() = default;
    ModuleLoader::ModuleLoader(ModuleLoader&&) noexcept = default;
    ModuleLoader& ModuleLoader::operator=(ModuleLoader&&) noexcept = default;

    ModuleLoader::LoadedGraph ModuleLoader::load_root(
        std::filesystem::path const& module_path,
        ModuleRenderConfig render_config,
        Sample* sample_period
    ) const
    {
        return _impl->load_root(module_path, render_config, sample_period);
    }

    std::vector<std::filesystem::path> const& ModuleLoader::extra_search_roots() const
    {
        return _impl->extra_search_roots;
    }
}
