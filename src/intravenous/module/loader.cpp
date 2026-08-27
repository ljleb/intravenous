#include <intravenous/module/loader.h>
#include <intravenous/module/abi.h>
#include <intravenous/compat.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <mutex>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace iv {
namespace {
struct Manifest {
    int schema = 0;
    std::string id;
    std::filesystem::path entry;
    std::string main;
};

struct ResolvedModule {
    Manifest manifest;
    std::filesystem::path module_dir;
    std::filesystem::path manifest_file;
    std::filesystem::path entry_file;
    bool global = false;
    std::filesystem::file_time_type source_stamp {};
};

struct ModuleImport {
    std::string id;
    bool global_only = false;
};

struct DynamicLibrary {
#if defined(_WIN32)
    HMODULE handle = nullptr;
#else
    void *handle = nullptr;
#endif

    explicit DynamicLibrary(std::filesystem::path const &path)
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
        if (handle) FreeLibrary(handle);
#else
        if (handle) dlclose(handle);
#endif
    }

    void *symbol(char const *name) const
    {
#if defined(_WIN32)
        return reinterpret_cast<void *>(GetProcAddress(handle, name));
#else
        return dlsym(handle, name);
#endif
    }
};

struct LoadedBinary {
    std::string id;
    std::filesystem::path artifact_path;
    std::shared_ptr<DynamicLibrary> library;
};

class ScopedModuleBuildLock {
#if defined(_WIN32)
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    OVERLAPPED overlapped_ {};
#else
    int fd_ = -1;
#endif

public:
    explicit ScopedModuleBuildLock(std::filesystem::path const& path)
    {
#if defined(_WIN32)
        handle_ = CreateFileW(
            path.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE ||
            !LockFileEx(
                handle_, LOCKFILE_EXCLUSIVE_LOCK, 0,
                MAXDWORD, MAXDWORD, &overlapped_)) {
            if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
            throw std::runtime_error(
                "failed to lock module build workspace '" + path.string() + "'");
        }
#else
        fd_ = ::open(path.c_str(), O_CREAT | O_RDWR, 0666);
        if (fd_ < 0 || ::flock(fd_, LOCK_EX) != 0) {
            if (fd_ >= 0) ::close(fd_);
            throw std::runtime_error(
                "failed to lock module build workspace '" + path.string() + "'");
        }
#endif
    }

    ~ScopedModuleBuildLock()
    {
#if defined(_WIN32)
        if (handle_ != INVALID_HANDLE_VALUE) {
            UnlockFileEx(handle_, 0, MAXDWORD, MAXDWORD, &overlapped_);
            CloseHandle(handle_);
        }
#else
        if (fd_ >= 0) {
            ::flock(fd_, LOCK_UN);
            ::close(fd_);
        }
#endif
    }

    ScopedModuleBuildLock(ScopedModuleBuildLock const&) = delete;
    ScopedModuleBuildLock& operator=(ScopedModuleBuildLock const&) = delete;
};

std::filesystem::path normalize(std::filesystem::path const &path)
{
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(path, ec);
    return ec ? std::filesystem::absolute(path).lexically_normal() : canonical;
}

bool is_within(std::filesystem::path const &path, std::filesystem::path const &root)
{
    auto p = normalize(path);
    auto r = normalize(root);
    auto pit = p.begin();
    auto rit = r.begin();
    for (; rit != r.end(); ++rit, ++pit) {
        if (pit == p.end() || *pit != *rit) {
            return false;
        }
    }
    return true;
}

std::string read_text(std::filesystem::path const &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("failed to open '" + path.string() + "'");
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write_text_if_different(std::filesystem::path const &path, std::string const &text)
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

Manifest parse_manifest(std::filesystem::path const &file)
{
    std::ifstream in(file);
    if (!in) {
        throw std::runtime_error("failed to open '" + file.string() + "'");
    }

    Manifest manifest;
    try {
        auto const json = nlohmann::json::parse(in);
        manifest.schema = json.at("schema").get<int>();
        manifest.id = json.at("id").get<std::string>();
        manifest.entry = json.at("entry").get<std::string>();
        manifest.main = json.at("main").get<std::string>();
    } catch (nlohmann::json::exception const &e) {
        throw std::runtime_error("invalid module manifest '" + file.string() + "': " + e.what());
    }

    if (manifest.schema != 1) {
        throw std::runtime_error(
            "manifest '" + file.string() + "' uses unsupported schema " +
            std::to_string(manifest.schema));
    }
    if (manifest.id.empty() || manifest.id == "." || manifest.id == ".." ||
        manifest.id.contains('/') || manifest.id.contains('\\')) {
        throw std::runtime_error(
            "manifest '" + file.string() + "' id must be a non-empty single path component");
    }
    if (manifest.entry.empty() || manifest.entry.is_absolute()) {
        throw std::runtime_error(
            "manifest '" + file.string() + "' entry must be a relative path");
    }
    if (manifest.main.empty()) {
        throw std::runtime_error("manifest '" + file.string() + "' has empty main");
    }
    return manifest;
}

std::filesystem::file_time_type directory_stamp(std::filesystem::path const &dir)
{
    std::filesystem::file_time_type latest {};
    bool any = false;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(
             dir,
             std::filesystem::directory_options::skip_permission_denied,
             ec),
         end;
         it != end;
         it.increment(ec)) {
        if (ec) break;
        if (it->is_directory()) {
            auto const name = it->path().filename();
            if (name == ".git" || name == "build") {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (!it->is_regular_file() || !is_module_dependency_source_path(it->path())) {
            continue;
        }
        auto stamp = std::filesystem::last_write_time(it->path(), ec);
        if (!ec) {
            latest = any ? std::max(latest, stamp) : stamp;
            any = true;
        }
    }
    return any ? latest : std::filesystem::file_time_type {};
}

std::vector<ModuleImport> scan_imports(std::filesystem::path const &entry)
{
    static std::regex const pattern(
        R"(^\s*#\s*include\s*<iv/(modules-global|modules)/([^>]+)>\s*$)");

    std::vector<ModuleImport> imports;
    std::istringstream stream(read_text(entry));
    for (std::string line; std::getline(stream, line);) {
        std::smatch match;
        if (!std::regex_match(line, match, pattern)) {
            continue;
        }
        ModuleImport import{
            .id = match[2].str(),
            .global_only = match[1].str() == "modules-global",
        };
        if (import.id.empty() || import.id.contains('/') || import.id.contains('\\')) {
            throw std::runtime_error(
                "invalid IV module import <iv/" + match[1].str() + "/" + import.id +
                "> in '" + entry.string() + "'");
        }
        imports.push_back(std::move(import));
    }
    return imports;
}

std::string sanitize(std::string_view value)
{
    std::string out(value);
    for (char &c : out) {
        if (!std::isalnum(static_cast<unsigned char>(c))) {
            c = '_';
        }
    }
    return out.empty() ? "module" : out;
}

std::string stable_hash(std::filesystem::path const &path)
{
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : normalize(path).generic_string()) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    std::ostringstream out;
    out << std::hex << hash;
    return out.str();
}

std::string library_name(std::string_view base)
{
#if defined(_WIN32)
    return std::string(base) + ".dll";
#elif defined(__APPLE__)
    return "lib" + std::string(base) + ".dylib";
#else
    return "lib" + std::string(base) + ".so";
#endif
}

char const *config_name()
{
#if defined(NDEBUG)
    return "Release";
#else
    return "Debug";
#endif
}

std::string quote(std::filesystem::path const &path)
{
    std::string value = path.generic_string();
    std::string out = "\"";
    for (char c : value) {
        if (c == '"') out += '\\';
        out += c;
    }
    return out + "\"";
}

std::filesystem::path discover_repo(std::filesystem::path start)
{
    start = normalize(start);
    if (std::filesystem::is_regular_file(start)) {
        start = start.parent_path();
    }
    for (auto path = start; !path.empty(); path = path.parent_path()) {
        if (std::filesystem::exists(path / "src/intravenous/dsl.h")) {
            return path;
        }
        if (path == path.root_path()) break;
    }
    throw std::runtime_error(
        "failed to discover repo root from '" + start.string() + "'");
}

std::filesystem::path discover_project_root(std::filesystem::path module_dir)
{
    module_dir = normalize(module_dir);
    for (auto path = module_dir; !path.empty(); path = path.parent_path()) {
        if (std::filesystem::exists(path / "iv_project.json") ||
            std::filesystem::exists(path / "iv_project.jsonl")) {
            return path;
        }
        if (path == path.root_path()) break;
    }
    if (module_dir.parent_path().filename() == "modules") {
        return module_dir.parent_path().parent_path();
    }
    return module_dir;
}

std::filesystem::path global_cache_root()
{
    if (char const *explicit_cache = std::getenv("IV_GLOBAL_MODULE_CACHE")) {
        if (*explicit_cache) return normalize(explicit_cache);
    }
#if defined(_WIN32)
    if (char const *local_app_data = std::getenv("LOCALAPPDATA")) {
        if (*local_app_data) return normalize(std::filesystem::path(local_app_data) / "Intravenous");
    }
#else
    if (char const *xdg = std::getenv("XDG_CACHE_HOME")) {
        if (*xdg) return normalize(std::filesystem::path(xdg) / "intravenous");
    }
    if (char const *home = std::getenv("HOME")) {
        if (*home) return normalize(std::filesystem::path(home) / ".cache/intravenous");
    }
#endif
    return normalize(std::filesystem::temp_directory_path() / "intravenous-global-cache");
}

void run(
    std::string const &command,
    ModuleLoader::LogSink const &sink,
    std::string_view phase)
{
    if (sink) sink("[" + std::string(phase) + "] " + command);
    int rc = std::system(command.c_str());
    if (rc != 0) {
        throw std::runtime_error(
            "command failed with exit code " + std::to_string(rc) + ": " + command);
    }
}
} // namespace

class ModuleLoader::Impl {
    std::filesystem::path repo_root_;
    std::filesystem::path global_cache_root_;
    ModuleLoaderToolchainConfig toolchain_;
    LogSink log_sink_;
    mutable std::mutex mutex_;

    struct Registry {
        std::unordered_map<std::string, ResolvedModule> project;
        std::unordered_map<std::string, ResolvedModule> global;
        std::unordered_map<std::string, ResolvedModule> effective;
    };

    struct Closure {
        std::vector<ResolvedModule> modules;
        std::unordered_map<std::string, std::vector<std::string>> dependency_keys;
    };

    static std::string key(ResolvedModule const &module)
    {
        return std::string(module.global ? "global:" : "project:") + module.manifest.id;
    }

    ResolvedModule resolve_dir(std::filesystem::path dir, bool global) const
    {
        dir = normalize(dir);
        auto manifest_file = dir / "iv_module.json";
        if (!std::filesystem::exists(manifest_file)) {
            throw std::runtime_error(
                "module directory '" + dir.string() + "' does not contain iv_module.json");
        }

        auto manifest = parse_manifest(manifest_file);
        auto entry = normalize(dir / manifest.entry);
        if (!std::filesystem::exists(entry) || !std::filesystem::is_regular_file(entry)) {
            throw std::runtime_error(
                "manifest '" + manifest_file.string() + "' entry does not exist: " +
                manifest.entry.string());
        }
        if (!is_within(entry, dir)) {
            throw std::runtime_error(
                "manifest entry escapes module directory: " + manifest.entry.string());
        }

        return {
            .manifest = std::move(manifest),
            .module_dir = dir,
            .manifest_file = normalize(manifest_file),
            .entry_file = entry,
            .global = global,
            .source_stamp = directory_stamp(dir),
        };
    }

    void scan_root(
        std::filesystem::path const &root,
        bool global,
        std::unordered_map<std::string, ResolvedModule> &out) const
    {
        if (!std::filesystem::exists(root)) return;

        std::error_code ec;
        for (std::filesystem::recursive_directory_iterator it(
                 root,
                 std::filesystem::directory_options::skip_permission_denied,
                 ec),
             end;
             it != end;
             it.increment(ec)) {
            if (ec) break;
            if (it->is_directory()) {
                auto const name = it->path().filename();
                if (name == ".git" || name == "build") {
                    it.disable_recursion_pending();
                }
                continue;
            }
            if (!it->is_regular_file() || it->path().filename() != "iv_module.json") {
                continue;
            }

            auto resolved = resolve_dir(it->path().parent_path(), global);
            auto [position, inserted] = out.emplace(resolved.manifest.id, resolved);
            if (!inserted && position->second.manifest_file != resolved.manifest_file) {
                throw std::runtime_error(
                    "duplicate module id '" + resolved.manifest.id + "' in '" +
                    position->second.manifest_file.string() + "' and '" +
                    resolved.manifest_file.string() + "'");
            }
            it.disable_recursion_pending();
        }
    }

    Registry registry_for(
        ResolvedModule const &root,
        std::filesystem::path const &project_root) const
    {
        Registry registry;
        for (auto const &path : extra_search_roots) {
            scan_root(path, true, registry.global);
        }

        if (!root.global) {
            scan_root(project_root, false, registry.project);
            registry.effective = registry.global;
            for (auto const &[id, module] : registry.project) {
                if (auto global = registry.global.find(id); global != registry.global.end()) {
                    if (log_sink_) {
                        log_sink_(
                            "warning: project module '" + id + "' at '" +
                            module.manifest_file.string() + "' shadows global module at '" +
                            global->second.manifest_file.string() + "'");
                    }
                }
                registry.effective[id] = module;
            }
            registry.effective[root.manifest.id] = root;
        } else {
            registry.global[root.manifest.id] = root;
            registry.effective = registry.global;
        }
        return registry;
    }

    ResolvedModule const &resolve_import(
        ResolvedModule const &from,
        ModuleImport const &import,
        Registry const &registry) const
    {
        auto const &scope = (from.global || import.global_only)
            ? registry.global
            : registry.effective;
        auto found = scope.find(import.id);
        if (found == scope.end()) {
            throw std::runtime_error(
                "module '" + from.manifest.id + "' imports missing " +
                std::string((from.global || import.global_only) ? "global" : "project/global") +
                " module '" + import.id + "'");
        }
        return found->second;
    }

    Closure reachable_modules(
        ResolvedModule const &root,
        Registry const &registry) const
    {
        Closure closure;
        std::unordered_set<std::string> visited;
        std::unordered_set<std::string> visiting;

        auto visit = [&](auto &&self, ResolvedModule const &module) -> void {
            auto const module_key = key(module);
            if (visited.contains(module_key)) return;
            if (!visiting.insert(module_key).second) {
                throw std::runtime_error(
                    "cyclic IV module import involving '" + module.manifest.id + "'");
            }

            auto &deps = closure.dependency_keys[module_key];
            for (auto const &import : scan_imports(module.entry_file)) {
                auto const &dependency = resolve_import(module, import, registry);
                self(self, dependency);
                deps.push_back(key(dependency));
            }
            std::sort(deps.begin(), deps.end());
            deps.erase(std::unique(deps.begin(), deps.end()), deps.end());

            visiting.erase(module_key);
            visited.insert(module_key);
            closure.modules.push_back(module);
        };

        visit(visit, root);
        return closure;
    }

    std::filesystem::path cmake_program() const
    {
        if (toolchain_.cmake_program) return *toolchain_.cmake_program;
        if (std::string_view(IV_CONFIGURED_CMAKE_COMMAND).size()) {
            return IV_CONFIGURED_CMAKE_COMMAND;
        }
        return "cmake";
    }

    std::pair<std::filesystem::path, std::filesystem::path> compilers() const
    {
        if (toolchain_.c_compiler && toolchain_.cxx_compiler) {
            return {*toolchain_.c_compiler, *toolchain_.cxx_compiler};
        }
        return {IV_CONFIGURED_C_COMPILER, IV_CONFIGURED_CXX_COMPILER};
    }

    static std::string generated_import_path(ResolvedModule const &module)
    {
        return std::string("iv/") +
            (module.global ? "modules-global/" : "modules/") +
            module.manifest.id;
    }

    static std::filesystem::path import_for(
        ResolvedModule const &module,
        std::filesystem::path const &project_import_root,
        std::filesystem::path const &global_import_root)
    {
        auto const &root = module.global ? global_import_root : project_import_root;
        return root / generated_import_path(module);
    }

    std::filesystem::path build(
        ResolvedModule const &root,
        Closure const &closure,
        std::filesystem::path const &project_root) const
    {
        auto const global_import_root = global_cache_root_ / "imports";
        auto const project_iv_root = project_root / "build/iv";
        auto const project_import_root = root.global
            ? global_import_root
            : project_iv_root / "imports";
        auto const owner_root = root.global ? global_cache_root_ : project_iv_root;
        auto const build_key = sanitize(root.manifest.id) + "_" + stable_hash(root.module_dir);
        auto const workspace = owner_root / "build" / build_key / config_name();
        auto const build_dir = workspace / "cmake-build";
        auto const output_dir = workspace / "out";
        auto const generated_dir = workspace / "generated";
        auto const export_file = generated_dir / "root_export.cpp";
        auto const default_source_dir = generated_dir / "default-project";
        auto const custom_cmake = root.module_dir / "CMakeLists.txt";
        auto const source_dir = std::filesystem::exists(custom_cmake)
            ? root.module_dir
            : default_source_dir;

        std::filesystem::create_directories(workspace);
        ScopedModuleBuildLock const build_lock(workspace / "build.lock");
        std::filesystem::create_directories(output_dir);
        std::filesystem::create_directories(generated_dir);
        std::filesystem::create_directories(project_import_root / "iv/modules");
        std::filesystem::create_directories(global_import_root / "iv/modules");
        std::filesystem::create_directories(global_import_root / "iv/modules-global");

        for (auto const &module : closure.modules) {
            auto const generated_import = import_for(
                module, project_import_root, global_import_root);
            write_text_if_different(
                generated_import,
                "#pragma once\n#include " + quote(module.entry_file) + "\n");
            if (module.global) {
                auto const forwarder = global_import_root / "iv/modules" / module.manifest.id;
                write_text_if_different(
                    forwarder,
                    "#pragma once\n#include <iv/modules-global/" + module.manifest.id + ">\n");
            }
        }

        auto const root_include = root.global
            ? std::string("iv/modules-global/") + root.manifest.id
            : std::string("iv/modules/") + root.manifest.id;
        std::ostringstream export_tu;
        export_tu << "#include <intravenous/module/authoring.h>\n"
                  << "namespace iv::details::source_introspection_plugin_bridge {\n"
                  << "template<class Ref> constexpr void "
                     "_annotate_source_info_after_statement(\n"
                  << "    Ref* ref, char const* declaration_identity, "
                     "char const* file_path,\n"
                  << "    std::uint32_t begin, std::uint32_t end) {\n"
                  << "  iv::_annotate_source_info_after_statement(\n"
                  << "      ref, declaration_identity, file_path, begin, end);\n"
                  << "}\n"
                  << "constexpr void _annotate_public_output_after_statement(\n"
                  << "    iv::GraphBuilder* builder, bool event, std::size_t ordinal,\n"
                  << "    char const* file_path, std::uint32_t begin, "
                     "std::uint32_t end) {\n"
                  << "  iv::_annotate_public_output_after_statement(\n"
                  << "      builder, event, ordinal, file_path, begin, end);\n"
                  << "}\n"
                  << "}\n"
                  << "#include <" << root_include << ">\n"
                  << "namespace {\n"
                  << "consteval iv::Graph iv_generated_module_graph_value() {\n"
                  << "  iv::GraphBuilder builder;\n"
                  << "  " << root.manifest.main << "(builder);\n"
                  << "  return builder.build_execution_root_node().graph;\n"
                  << "}\n"
                  << "consteval iv::StaticGraphIntrospectionMetadata "
                     "iv_generated_module_metadata_value() {\n"
                  << "  iv::GraphBuilder builder;\n"
                  << "  " << root.manifest.main << "(builder);\n"
                  << "  return iv::details::define_static_metadata(builder.build_metadata());\n"
                  << "}\n"
                  << "}\n"
                  << "extern \"C\" IV_MODULE_EXPORT std::uint32_t iv_module_abi_version() {\n"
                  << "  return iv::IV_MODULE_ABI_VERSION;\n"
                  << "}\n"
                  << "extern \"C\" IV_MODULE_EXPORT iv::WeakTypeErasedNode iv_module_graph() {\n"
                  << "  static constexpr iv::StaticGraphRoot<"
                     "iv_generated_module_graph_value()> graph {};\n"
                  << "  return iv::WeakTypeErasedNode(graph);\n"
                  << "}\n"
                  << "extern \"C\" IV_MODULE_EXPORT iv::StaticGraphIntrospectionMetadata iv_module_metadata() {\n"
                  << "  static constexpr auto metadata = "
                     "iv_generated_module_metadata_value();\n"
                  << "  return metadata;\n"
                  << "}\n";
        write_text_if_different(export_file, export_tu.str());

        if (!std::filesystem::exists(custom_cmake)) {
            std::filesystem::create_directories(default_source_dir);
            write_text_if_different(
                default_source_dir / "CMakeLists.txt",
                "cmake_minimum_required(VERSION 3.20)\n"
                "project(iv_runtime_module LANGUAGES CXX)\n"
                "set(CMAKE_EXPORT_COMPILE_COMMANDS ON)\n"
                "include(${IV_SOURCE_DIR}/module/template/ModuleSupport.cmake)\n"
                "iv_add_runtime_module(iv_runtime_module)\n");
        }

        auto const [cc, cxx] = compilers();
        auto const source_introspection_plugin =
            std::filesystem::path(IV_CONFIGURED_GCC_SOURCE_INTROSPECTION_PLUGIN);
        if (source_introspection_plugin.empty()
            || !std::filesystem::exists(source_introspection_plugin)) {
            throw std::runtime_error(
                "configured GCC source-introspection plugin does not exist: '" +
                source_introspection_plugin.string() + "'");
        }
        std::string generator = toolchain_.cmake_generator.value_or(
            std::string(IV_CONFIGURED_CMAKE_GENERATOR));

        std::ostringstream signature;
        signature << "iv-module-abi=" << IV_MODULE_ABI_VERSION << '\n'
                  << "config=" << config_name() << '\n'
                  << "cmake=" << cmake_program().generic_string() << '\n'
                  << "cc=" << cc.generic_string() << '\n'
                  << "cxx=" << cxx.generic_string() << '\n'
                  << "source-introspection-plugin="
                  << source_introspection_plugin.generic_string() << '\n'
                  << "source-introspection-plugin-stamp="
                  << std::filesystem::last_write_time(source_introspection_plugin)
                         .time_since_epoch().count() << '\n'
                  << "generator=" << generator << '\n'
                  << "generated-export=" << export_tu.str() << '\n'
                  << "core-source-stamp="
                  << directory_stamp(repo_root_ / "src/intravenous")
                         .time_since_epoch().count() << '\n'
                  << read_text(repo_root_ / "src/intravenous/module/abi.h") << '\n'
                  << read_text(repo_root_ / "src/intravenous/module/authoring.h") << '\n'
                  << read_text(repo_root_ / "src/intravenous/graph/static_metadata.hpp") << '\n'
                  << read_text(repo_root_ / "src/intravenous/module/template/ModuleSupport.cmake") << '\n';
        for (auto const &module : closure.modules) {
            signature << key(module) << '\n'
                      << read_text(module.manifest_file) << '\n'
                      << module.source_stamp.time_since_epoch().count() << '\n';
        }
        if (std::filesystem::exists(custom_cmake)) {
            signature << read_text(custom_cmake) << '\n';
        }
        auto const signature_file = workspace / "build.signature";
        auto const artifact_name = library_name("iv_module_" + sanitize(root.manifest.id));
        auto artifact = output_dir / artifact_name;
        bool const needs_build =
            !std::filesystem::exists(artifact) ||
            !std::filesystem::exists(signature_file) ||
            read_text(signature_file) != signature.str();

        std::vector<std::filesystem::path> include_dirs;
        include_dirs.reserve(closure.modules.size());
        for (auto const &module : closure.modules) {
            include_dirs.push_back(module.module_dir);
        }
        std::sort(include_dirs.begin(), include_dirs.end());
        include_dirs.erase(std::unique(include_dirs.begin(), include_dirs.end()), include_dirs.end());
        std::ostringstream include_list;
        for (size_t i = 0; i < include_dirs.size(); ++i) {
            if (i) include_list << ';';
            include_list << include_dirs[i].generic_string();
        }

        std::ostringstream configure;
        configure << quote(cmake_program())
                  << " -S " << quote(source_dir)
                  << " -B " << quote(build_dir)
                  << " -DCMAKE_BUILD_TYPE=" << config_name();
        if (!generator.empty()) configure << " -G " << quote(generator);
        if (!cc.empty()) configure << " -DCMAKE_C_COMPILER=" << quote(cc);
        if (!cxx.empty()) configure << " -DCMAKE_CXX_COMPILER=" << quote(cxx);
        configure << " -DIV_INCLUDE_DIR=" << quote(repo_root_ / "src")
                  << " -DIV_SOURCE_DIR=" << quote(repo_root_ / "src/intravenous")
                  << " -DIV_THIRD_PARTY_INCLUDE_DIR=" << quote(repo_root_ / "src/intravenous/third_party")
                  << " -DIV_MODULE_SOURCE_DIR=" << quote(root.module_dir)
                  << " -DIV_MODULE_ENTRY_FILE=" << quote(root.entry_file)
                  << " -DIV_MODULE_EXPORT_FILE=" << quote(export_file)
                  << " -DIV_MODULE_GENERATED_INCLUDE_DIR=" << quote(project_import_root)
                  << " -DIV_GLOBAL_MODULE_GENERATED_INCLUDE_DIR=" << quote(global_import_root)
                  << " -DIV_MODULE_INCLUDE_DIRS=\"" << include_list.str() << "\""
                  << " -DIV_MODULE_OUTPUT_DIR=" << quote(output_dir)
                  << " -DIV_MODULE_OUTPUT_NAME=iv_module_" << sanitize(root.manifest.id)
                  << " -DIV_GCC_SOURCE_INTROSPECTION_PLUGIN="
                  << quote(source_introspection_plugin);
        if (std::string_view(IV_CONFIGURED_IV_MODULE_SHARED_LIBRARY).size()) {
            configure << " -DIV_MODULE_SHARED_LIBRARY=" << quote(IV_CONFIGURED_IV_MODULE_SHARED_LIBRARY);
        }
        if (needs_build || !std::filesystem::exists(build_dir / "CMakeCache.txt")) {
            run(configure.str(), log_sink_, "configure");
            run(
                quote(cmake_program()) + " --build " + quote(build_dir) +
                    " --config " + config_name() + " --parallel 16",
                log_sink_,
                "build");
            write_text_if_different(signature_file, signature.str());
        }

        auto const compile_database = build_dir / "compile_commands.json";
        if (std::filesystem::exists(compile_database)) {
            auto const database_text = read_text(compile_database);
            for (auto const& module : closure.modules) {
                try {
                    write_text_if_different(
                        module.module_dir / "compile_commands.json",
                        database_text
                    );
                } catch (std::exception const& error) {
                    if (log_sink_) {
                        log_sink_(
                            "warning: could not publish module compile database to '" +
                            module.module_dir.string() + "': " + error.what()
                        );
                    }
                }
            }
        }

        if (!std::filesystem::exists(artifact)) {
            auto configured_artifact = output_dir / config_name() / artifact_name;
            if (std::filesystem::exists(configured_artifact)) {
                artifact = configured_artifact;
            }
        }
        if (!std::filesystem::exists(artifact)) {
            throw std::runtime_error(
                "module build did not produce expected artifact '" + artifact.string() + "'");
        }

        auto const generation = std::to_string(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        auto const generation_dir = owner_root / "generations" / build_key / generation;
        std::filesystem::create_directories(generation_dir);
        auto const generation_artifact = generation_dir / artifact.filename();
        std::filesystem::copy_file(
            artifact,
            generation_artifact,
            std::filesystem::copy_options::overwrite_existing);
        return generation_artifact;
    }

public:
    std::vector<std::filesystem::path> extra_search_roots;

    Impl(
        std::filesystem::path discovery_start,
        std::vector<std::filesystem::path> roots,
        ModuleLoaderToolchainConfig toolchain,
        LogSink sink)
        : repo_root_(discover_repo(std::move(discovery_start))),
          global_cache_root_(global_cache_root()),
          toolchain_(std::move(toolchain)),
          log_sink_(std::move(sink))
    {
        std::filesystem::create_directories(global_cache_root_);
        for (auto const &root : roots) {
            extra_search_roots.push_back(normalize(root));
        }
    }

    LoadedDefinition load_root_definition(std::filesystem::path const &path) const
    {
        std::lock_guard lock(mutex_);
        auto module_path = normalize(path);
        if (std::filesystem::is_regular_file(module_path)) {
            if (module_path.filename() == "iv_module.json") {
                module_path = module_path.parent_path();
            } else {
                throw std::runtime_error(
                    "root module path must be a module directory or iv_module.json");
            }
        }

        bool const global_root = std::ranges::any_of(
            extra_search_roots,
            [&](auto const &root) { return is_within(module_path, root); });
        auto root = resolve_dir(module_path, global_root);
        auto const project_root = root.global
            ? global_cache_root_
            : discover_project_root(root.module_dir);
        auto registry = registry_for(root, project_root);
        auto closure = reachable_modules(root, registry);
        auto artifact = build(root, closure, project_root);

        auto library = std::make_shared<DynamicLibrary>(artifact);
        auto abi_version = reinterpret_cast<iv_module_abi_version_fn>(
            library->symbol("iv_module_abi_version"));
        if (!abi_version) {
            throw std::runtime_error(
                "module '" + artifact.string() +
                "' does not export iv_module_abi_version");
        }
        auto const loaded_abi_version = abi_version();
        if (loaded_abi_version != IV_MODULE_ABI_VERSION) {
            throw std::runtime_error(
                "module '" + artifact.string() +
                "' has incompatible ABI version " +
                std::to_string(loaded_abi_version) + " (expected " +
                std::to_string(IV_MODULE_ABI_VERSION) + ")");
        }
        auto graph = reinterpret_cast<iv_module_graph_fn>(
            library->symbol("iv_module_graph"));
        if (!graph) {
            throw std::runtime_error(
                "module '" + artifact.string() + "' does not export iv_module_graph");
        }
        auto metadata = reinterpret_cast<iv_module_metadata_fn>(
            library->symbol("iv_module_metadata"));
        if (!metadata) {
            throw std::runtime_error(
                "module '" + artifact.string() + "' does not export iv_module_metadata");
        }
        auto root_node = graph();
        if (!root_node) {
            throw std::runtime_error(
                "module '" + artifact.string() + "' exported an invalid graph root");
        }
        auto introspection = metadata().metadata();

        auto binary = std::make_shared<LoadedBinary>(LoadedBinary{
            root.manifest.id,
            artifact,
            library,
        });

        std::vector<ModuleDependency> dependencies;
        dependencies.reserve(closure.modules.size());
        for (auto const &module : closure.modules) {
            dependencies.push_back({
                module.manifest.id,
                module.module_dir,
                module.entry_file,
                module.source_stamp,
            });
        }
        std::sort(
            dependencies.begin(),
            dependencies.end(),
            [](auto const &a, auto const &b) {
                if (a.id != b.id) return a.id < b.id;
                return a.module_dir < b.module_dir;
            });

        std::vector<ModuleRef> refs{binary};
        return LoadedDefinition(
            std::move(refs),
            root_node,
            std::move(introspection),
            root.module_dir,
            root.manifest.id,
            std::move(dependencies));
    }
};

ModuleLoader::LoadedDefinition::LoadedDefinition(
    std::vector<ModuleRef> refs,
    WeakTypeErasedNode root_,
    GraphIntrospectionMetadata introspection_,
    std::filesystem::path path,
    std::string id,
    std::vector<ModuleDependency> deps)
    : module_refs(std::move(refs)),
      root(root_),
      introspection(std::move(introspection_)),
      module_path(std::move(path)),
      module_id(std::move(id)),
      dependencies(std::move(deps))
{}

ModuleLoader::ModuleLoader(
    std::filesystem::path start,
    std::vector<std::filesystem::path> roots,
    ModuleLoaderToolchainConfig toolchain,
    LogSink sink)
    : _impl(std::make_unique<Impl>(
          std::move(start),
          std::move(roots),
          std::move(toolchain),
          std::move(sink)))
{}

ModuleLoader::~ModuleLoader() = default;
ModuleLoader::ModuleLoader(ModuleLoader &&) noexcept = default;
ModuleLoader &ModuleLoader::operator=(ModuleLoader &&) noexcept = default;

ModuleLoader::LoadedDefinition ModuleLoader::load_root_definition(
    std::filesystem::path const &path) const
{
    return _impl->load_root_definition(path);
}

std::vector<std::filesystem::path> const &ModuleLoader::extra_search_roots() const
{
    return _impl->extra_search_roots;
}
} // namespace iv
