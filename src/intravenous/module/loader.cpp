#include <intravenous/module/loader.h>
#include <intravenous/module/abi.h>
#include <intravenous/module/configured_graph_wire.h>
#include <intravenous/module/builder_session.h>
#include <intravenous/module/package_manifest.h>
#include <intravenous/module/package_registration.h>
#include <intravenous/compat.h>
#include <intravenous/graph/builder/lowering.hpp>
#include <intravenous/graph/compiler.h>
#include <intravenous/graph/node.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <mutex>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <type_traits>
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
    std::filesystem::path entry;
};

std::optional<std::filesystem::path> find_source_manifest(
    std::filesystem::path const& directory)
{
    auto const source_manifest = directory / IV_PACKAGE_MANIFEST_FILE;
    return std::filesystem::exists(source_manifest)
        ? std::optional<std::filesystem::path>{source_manifest}
        : std::nullopt;
}

struct ResolvedPackage {
    Manifest manifest;
    std::string package_key;
    std::filesystem::path module_dir;
    std::filesystem::path manifest_file;
    std::filesystem::path entry_file;
    bool global = false;
    std::filesystem::file_time_type package_stamp {};
};

std::string read_text(std::filesystem::path const& path);

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
    std::string package_root;
    std::filesystem::path binary_path;
    std::shared_ptr<DynamicLibrary> library;
    std::vector<details::PackageRegistration> registrations{};
    std::vector<NodeConfigPointerFieldData> config_pointer_fields{};
    std::vector<RetainedGlobalData> retained_globals{};
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
        manifest.entry = json.at("entry").get<std::string>();
    } catch (nlohmann::json::exception const &e) {
        throw std::runtime_error("invalid IV package manifest '" + file.string() + "': " + e.what());
    }

    if (manifest.schema != 2) {
        throw std::runtime_error(
            "IV package manifest '" + file.string() + "' uses unsupported schema " +
            std::to_string(manifest.schema));
    }
    if (manifest.entry.empty() || manifest.entry.is_absolute()) {
        throw std::runtime_error(
            "IV package manifest '" + file.string() + "' entry must be a relative path");
    }
    return manifest;
}

std::string package_key_for(std::filesystem::path const& directory)
{
    auto const normalized = normalize(directory);
    auto modules = normalized.end();
    for (auto it = normalized.begin(); it != normalized.end(); ++it) {
        if (*it == "modules") modules = it;
    }
    if (modules != normalized.end()) {
        auto relative = std::filesystem::path{};
        for (auto it = std::next(modules); it != normalized.end(); ++it) {
            relative /= *it;
        }
        if (!relative.empty()) return relative.generic_string();
    }
    auto const name = normalized.filename().generic_string();
    if (name.empty() || name == "." || name == "..") {
        throw std::runtime_error(
            "cannot derive IV package package key from '" + normalized.string() + "'");
    }
    return name;
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
            } else if (it->path() != dir
                       && std::filesystem::exists(
                           it->path() / std::string(IV_PACKAGE_MANIFEST_FILE))) {
                // A nested IV package package owns a distinct artifact and
                // source stamp. Its edits must not cause this package's C++
                // compilation signature to change.
                it.disable_recursion_pending();
            }
            continue;
        }
        if (!it->is_regular_file() || !is_module_dependency_package_path(it->path())) {
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

std::string stable_text_hash(std::string_view value)
{
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : value) {
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

std::string_view compile_stage_name(ModuleCompileStage stage)
{
    switch (stage) {
    case ModuleCompileStage::full: return "full";
    case ModuleCompileStage::configuration: return "configuration";
    case ModuleCompileStage::lowering_topology: return "lowering-topology";
    case ModuleCompileStage::lowering_materialization:
        return "lowering-materialization";
    case ModuleCompileStage::lowering_normalization:
        return "lowering-normalization";
    case ModuleCompileStage::lowering: return "lowering";
    case ModuleCompileStage::compilation: return "compilation";
    case ModuleCompileStage::static_metadata: return "static-metadata";
    }
    throw std::logic_error("invalid module compile stage");
}

std::string_view module_optimization_name(ModuleOptimization optimization)
{
    switch (optimization) {
    case ModuleOptimization::O0: return "O0";
    case ModuleOptimization::O3: return "O3";
    }
    throw std::logic_error("invalid module optimization");
}

std::string_view module_optimization_flags(ModuleOptimization optimization)
{
    switch (optimization) {
    case ModuleOptimization::O0: return "-O0 -DNDEBUG";
    case ModuleOptimization::O3: return "-O3 -DNDEBUG";
    }
    throw std::logic_error("invalid module optimization");
}

std::string quote_string(std::string_view value)
{
    std::string out = "\"";
    for (char c : value) {
        if (c == '"') out += '\\';
        out += c;
    }
    return out + "\"";
}

std::string quote(std::filesystem::path const &path)
{
    return quote_string(path.generic_string());
}

std::string cxx_string_literal(std::string_view value)
{
    std::string result = "\"";
    for (char character : value) {
        switch (character) {
        case '\\': result += "\\\\"; break;
        case '\"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default: result += character; break;
        }
    }
    return result + "\"";
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
    std::string_view phase,
    std::filesystem::path const* output_log = nullptr)
{
    if (sink) sink("[" + std::string(phase) + "] " + command);
    auto invocation = command;
    if (output_log) {
        invocation += " >> " + quote(*output_log) + " 2>&1";
    }
    auto const started_at = std::chrono::steady_clock::now();
    int rc = std::system(invocation.c_str());
    if (rc != 0) {
        throw std::runtime_error(
            "command failed with exit code " + std::to_string(rc) + ": " + command);
    }
    if (sink) {
        sink(
            "[" + std::string(phase) + "] elapsed_us=" +
            std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started_at).count()));
    }
}
} // namespace

class ModuleLoader::Impl {
    std::filesystem::path repo_root_;
    std::filesystem::path global_cache_root_;
    ModuleLoaderToolchainConfig toolchain_;
    LogSink log_sink_;
    mutable std::mutex mutex_;

    struct CompiledPackage {
        ResolvedPackage root;
        // All independently discovered source packages participate in the
        // current graph configuration. Stable IDs are discovered from their
        // compiler registrations after loading, never by parsing C++ text.
        std::vector<ResolvedPackage> configuration_packages;
        std::filesystem::path artifact;
    };

    mutable std::unordered_map<std::string, std::weak_ptr<LoadedBinary>>
        loaded_binaries_by_artifact;

    static std::string key(ResolvedPackage const &module)
    {
        return std::string(module.global ? "global:" : "project:")
            + normalize(module.module_dir).generic_string();
    }

    ResolvedPackage resolve_dir(std::filesystem::path dir, bool global) const
    {
        dir = normalize(dir);
        auto manifest_file = find_source_manifest(dir);
        if (!manifest_file) {
            throw std::runtime_error(
                "IV package directory '" + dir.string() + "' does not contain " +
                std::string(IV_PACKAGE_MANIFEST_FILE));
        }

        auto manifest = parse_manifest(*manifest_file);
        auto entry = normalize(dir / manifest.entry);
        if (!std::filesystem::exists(entry) || !std::filesystem::is_regular_file(entry)) {
            throw std::runtime_error(
                "IV package manifest '" + manifest_file->string() + "' entry does not exist: " +
                manifest.entry.string());
        }
        if (!is_within(entry, dir)) {
            throw std::runtime_error(
                "IV package manifest entry escapes source directory: " + manifest.entry.string());
        }

        return {
            .manifest = std::move(manifest),
            .package_key = package_key_for(dir),
            .module_dir = dir,
            .manifest_file = normalize(*manifest_file),
            .entry_file = entry,
            .global = global,
            .package_stamp = directory_stamp(dir),
        };
    }

    void scan_root(
        std::filesystem::path const &root,
        bool global,
        std::vector<ResolvedPackage>& out) const
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
            if (!it->is_regular_file()
                || !is_iv_package_manifest_file(it->path().filename().string())) {
                continue;
            }

            auto const source_dir = it->path().parent_path();
            auto manifest = find_source_manifest(source_dir);
            if (!manifest || normalize(it->path()) != normalize(*manifest)) {
                continue;
            }
            out.push_back(resolve_dir(source_dir, global));
            it.disable_recursion_pending();
        }
    }

    std::vector<ResolvedPackage> packages_for_graph_configuration(
        ResolvedPackage const &root,
        std::filesystem::path const &project_root) const
    {
        std::vector<ResolvedPackage> discovered;
        for (auto const &path : extra_search_roots) {
            scan_root(path, true, discovered);
        }
        if (!root.global) {
            scan_root(project_root, false, discovered);
        }
        // A project package shadows a global package at the same physical
        // path, but stable-ID collisions between distinct sources are left to
        // complete candidate-registry validation.
        std::unordered_map<std::string, ResolvedPackage> by_manifest;
        for (auto& source : discovered) {
            auto const manifest = source.manifest_file.generic_string();
            auto existing = by_manifest.find(manifest);
            if (existing == by_manifest.end() || !source.global) {
                by_manifest.insert_or_assign(manifest, std::move(source));
            }
        }
        by_manifest.insert_or_assign(root.manifest_file.generic_string(), root);
        std::vector<ResolvedPackage> result;
        result.reserve(by_manifest.size());
        for (auto& [_, source] : by_manifest) result.push_back(std::move(source));
        std::ranges::sort(result, {}, [](ResolvedPackage const& source) {
            return source.manifest_file.generic_string();
        });
        return result;
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

    std::filesystem::path build(
        ResolvedPackage const &root,
        std::filesystem::path const &project_root) const
    {
        auto const project_iv_root = project_root / "build/iv";
        auto const owner_root = root.global ? global_cache_root_ : project_iv_root;
        auto const build_key = sanitize(root.package_key) + "_" + stable_hash(root.module_dir);
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

        std::ostringstream export_tu;
        export_tu << "#include <intravenous/module/abi.h>\n"
                  << "extern \"C\" IV_MODULE_EXPORT std::uint32_t "
                     "iv_module_abi_version() {\n"
                  << "  return iv::IV_MODULE_ABI_VERSION;\n"
                  << "}\n";
        write_text_if_different(export_file, export_tu.str());

        if (!std::filesystem::exists(custom_cmake)) {
            std::filesystem::create_directories(default_source_dir);
            write_text_if_different(
                default_source_dir / "CMakeLists.txt",
                "cmake_minimum_required(VERSION 3.21)\n"
                "project(iv_runtime_module LANGUAGES CXX)\n"
                "set(CMAKE_EXPORT_COMPILE_COMMANDS ON)\n"
                "include(${IV_SOURCE_DIR}/module/template/ModuleSupport.cmake)\n"
                "iv_add_runtime_module(iv_runtime_module)\n");
        }

        auto const [cc, cxx] = compilers();
        auto const source_introspection_plugin =
            std::filesystem::path(IV_CONFIGURED_CLANG_SOURCE_INTROSPECTION_PLUGIN);
        auto const module_finalizer =
            std::filesystem::path(IV_CONFIGURED_IV_MODULE_FINALIZER);
        if (source_introspection_plugin.empty()
            || !std::filesystem::exists(source_introspection_plugin)) {
            throw std::runtime_error(
                "configured Clang source-introspection plugin does not exist: '" +
                source_introspection_plugin.string() + "'");
        }
        if (module_finalizer.empty() || !std::filesystem::exists(module_finalizer)) {
            throw std::runtime_error(
                "configured IV module finalizer does not exist: '" +
                module_finalizer.string() + "'");
        }
        std::string generator = toolchain_.cmake_generator.value_or(
            std::string(IV_CONFIGURED_CMAKE_GENERATOR));

        std::ostringstream signature;
        signature << "iv-module-abi=" << IV_MODULE_ABI_VERSION << '\n'
                  << "config=" << config_name() << '\n'
                  << "cmake=" << cmake_program().generic_string() << '\n'
                  << "cc=" << cc.generic_string() << '\n'
                  << "cxx=" << cxx.generic_string() << '\n'
                  << "generator=" << generator << '\n'
                  << "compile-stage="
                  << compile_stage_name(toolchain_.compile_stage) << '\n'
                  << "optimization="
                  << module_optimization_name(toolchain_.optimization) << '\n'
                  << "source-introspection="
                  << toolchain_.source_introspection << '\n'
                  << "precompiled-header="
                  << toolchain_.precompiled_header << '\n'
                  << "clang-time-trace="
                  << toolchain_.clang_time_trace << '\n'
                  << "generated-export=" << export_tu.str() << '\n'
                  << "core-source-stamp="
                  << directory_stamp(repo_root_ / "src/intravenous")
                         .time_since_epoch().count() << '\n'
                  << read_text(repo_root_ / "src/intravenous/module/abi.h") << '\n'
                  << read_text(repo_root_ / "src/intravenous/module/builder_session.h") << '\n'
                  << read_text(repo_root_ / "src/intravenous/module/template/ModuleSupport.cmake") << '\n';
        signature
            << "module-finalizer=" << module_finalizer.generic_string() << '\n'
            << "module-finalizer-stamp="
            << std::filesystem::last_write_time(module_finalizer)
                   .time_since_epoch().count() << '\n';
        signature
            << "source-introspection-plugin="
            << source_introspection_plugin.generic_string() << '\n'
            << "source-introspection-plugin-stamp="
            << std::filesystem::last_write_time(source_introspection_plugin)
                   .time_since_epoch().count() << '\n';
        // A source artifact owns only its own implementation files. Registered
        // IDs resolve through graph configuration, so provider edits do
        // not enter a consumer's C++ compilation signature.
        signature << key(root) << '\n'
                  << read_text(root.manifest_file) << '\n'
                  << root.package_stamp.time_since_epoch().count() << '\n';
        if (std::filesystem::exists(custom_cmake)) {
            signature << read_text(custom_cmake) << '\n';
        }
        auto const signature_file = workspace / "build.signature";
        auto const artifact_stem = "iv_source_" + sanitize(root.package_key)
            + "_" + stable_text_hash(signature.str());
        auto const artifact_name = library_name(artifact_stem);
        auto artifact = output_dir / artifact_name;
        bool const needs_build =
            !std::filesystem::exists(artifact) ||
            !std::filesystem::exists(signature_file) ||
            read_text(signature_file) != signature.str();

        std::vector<std::filesystem::path> include_dirs{root.module_dir};
        std::ostringstream include_list;
        for (size_t i = 0; i < include_dirs.size(); ++i) {
            if (i) include_list << ';';
            include_list << include_dirs[i].generic_string();
        }
        std::ostringstream source_list;
        source_list << root.entry_file.generic_string();

        std::ostringstream configure;
        configure << quote(cmake_program())
                  << " -S " << quote(source_dir)
                  << " -B " << quote(build_dir)
                  << " -DCMAKE_BUILD_TYPE=" << config_name()
                  << " -DCMAKE_CXX_FLAGS_RELEASE="
                  << quote_string(module_optimization_flags(toolchain_.optimization));
        if (!generator.empty()) configure << " -G " << quote(generator);
        if (!cc.empty()) configure << " -DCMAKE_C_COMPILER=" << quote(cc);
        if (!cxx.empty()) configure << " -DCMAKE_CXX_COMPILER=" << quote(cxx);
        configure << " -DIV_INCLUDE_DIR=" << quote(repo_root_ / "src")
                  << " -DIV_SOURCE_DIR=" << quote(repo_root_ / "src/intravenous")
                  << " -DIV_THIRD_PARTY_INCLUDE_DIR=" << quote(repo_root_ / "src/intravenous/third_party")
                  << " -DIV_MODULE_SOURCE_DIR=" << quote(root.module_dir)
                  << " -DIV_MODULE_ENTRY_FILE=" << quote(root.entry_file)
                  << " -DIV_MODULE_EXPORT_FILE=" << quote(export_file)
                  << " -DIV_MODULE_INCLUDE_DIRS=\"" << include_list.str() << "\""
                  << " -DIV_MODULE_SOURCE_FILES=\"" << source_list.str() << "\""
                  << " -DIV_MODULE_OUTPUT_DIR=" << quote(output_dir)
                  << " -DIV_MODULE_OUTPUT_NAME=" << artifact_stem
                  << " -DIV_CLANG_SOURCE_INTROSPECTION_PLUGIN="
                  << quote(source_introspection_plugin)
                  << " -DIV_MODULE_FINALIZER=" << quote(module_finalizer)
                  << " -DIV_MODULE_FINALIZER_OPTIMIZATION="
                  << module_optimization_name(toolchain_.optimization);
        if (!toolchain_.source_introspection) {
            configure << " -DIV_MODULE_SOURCE_INTROSPECTION=OFF";
        }
        if (!toolchain_.precompiled_header) {
            configure << " -DIV_MODULE_PCH_HEADER=";
        }
        if (toolchain_.clang_time_trace) {
            configure << " -DIV_MODULE_CLANG_TIME_TRACE=ON";
        }
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
            try {
                write_text_if_different(
                    root.module_dir / "compile_commands.json",
                    database_text
                );
            } catch (std::exception const& error) {
                if (log_sink_) {
                    log_sink_(
                        "warning: could not publish source compile database to '" +
                        root.module_dir.string() + "': " + error.what()
                    );
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

        // The signature is part of the filename. A changed source produces a
        // new DSO path while graphs using the previous source generation keep
        // their old artifact alive through ModuleRef ownership.
        return artifact;
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

    CompiledPackage compile_package_unlocked(
        std::filesystem::path const& path) const
    {
        auto module_path = normalize(path);
        if (std::filesystem::is_regular_file(module_path)) {
            if (is_iv_package_manifest_file(module_path.filename().string())) {
                module_path = module_path.parent_path();
            } else {
                throw std::runtime_error(
                    "root source path must be an IV package directory, " +
                    std::string(IV_PACKAGE_MANIFEST_FILE));
            }
        }

        bool const global_root = std::ranges::any_of(
            extra_search_roots,
            [&](auto const& root) { return is_within(module_path, root); });
        auto root = resolve_dir(module_path, global_root);
        auto const project_root = root.global
            ? global_cache_root_
            : discover_project_root(root.module_dir);
        auto configuration_packages = packages_for_graph_configuration(root, project_root);
        auto artifact = build(root, project_root);
        return {
            .root = std::move(root),
            .configuration_packages = std::move(configuration_packages),
            .artifact = std::move(artifact),
        };
    }

    std::filesystem::path compile_package(
        std::filesystem::path const& path) const
    {
        std::lock_guard lock(mutex_);
        return compile_package_unlocked(path).artifact;
    }

    ModuleLoader::LoadedPackage load_compiled_package(
        CompiledPackage const& compiled) const
    {
        auto const& root = compiled.root;
        auto const& artifact = compiled.artifact;

        auto const artifact_key = normalize(artifact).generic_string();
        std::shared_ptr<LoadedBinary> binary;
        if (auto existing = loaded_binaries_by_artifact.find(artifact_key);
            existing != loaded_binaries_by_artifact.end()) {
            binary = existing->second.lock();
        }
        if (!binary) {
            // A changed IV package uses a signature-addressed DSO path. Older
            // binaries remain alive only through graphs that still reference
            // them; no process-global registration state is replaced here.
            auto const dynamic_library_started_at = std::chrono::steady_clock::now();
            auto library = std::make_shared<DynamicLibrary>(artifact);
            if (log_sink_) {
                log_sink_(
                    "[dynamic-library-load] elapsed_us=" +
                    std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - dynamic_library_started_at).count()));
            }
            binary = std::make_shared<LoadedBinary>(LoadedBinary{
                .id = root.package_key,
                .package_root = root.module_dir.generic_string(),
                .binary_path = artifact,
                .library = std::move(library),
            });
            loaded_binaries_by_artifact.insert_or_assign(artifact_key, binary);
        }
        auto const& library = binary->library;

        auto const source_deserialization_started_at = std::chrono::steady_clock::now();
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
        auto const registrations_fn = reinterpret_cast<iv_package_registrations_fn>(
            library->symbol("iv_package_registrations"));
        auto const pointer_fields_fn =
            reinterpret_cast<iv_package_node_config_pointer_fields_fn>(
                library->symbol("iv_package_node_config_pointer_fields"));
        auto const retained_globals_fn = reinterpret_cast<iv_package_retained_globals_fn>(
            library->symbol("iv_package_retained_globals"));
        if (!registrations_fn || !pointer_fields_fn || !retained_globals_fn) {
            throw std::runtime_error(
                "IV package binary '" + artifact.string()
                + "' does not export its graph-configuration tables");
        }

        auto copy_table = [&](auto view, auto* type_tag, std::string_view name) {
            using T = std::remove_pointer_t<decltype(type_tag)>;
            if (!view.data && view.size != 0) {
                throw std::runtime_error(
                    "IV package " + std::string(name) + " table has null data");
            }
            if (view.size % sizeof(T) != 0) {
                throw std::runtime_error(
                    "IV package " + std::string(name) + " table has invalid size");
            }
            auto values = std::span(
                static_cast<T const*>(view.data), view.size / sizeof(T));
            return std::vector<T>(values.begin(), values.end());
        };
        binary->registrations = copy_table(
            registrations_fn(),
            static_cast<details::PackageRegistration*>(nullptr),
            "registration");
        binary->config_pointer_fields = copy_table(
            pointer_fields_fn(),
            static_cast<NodeConfigPointerFieldData*>(nullptr),
            "node-config pointer-field");
        binary->retained_globals = copy_table(
            retained_globals_fn(),
            static_cast<RetainedGlobalData*>(nullptr),
            "retained LLVM global");

        for (auto const& registration : binary->registrations) {
            if (!registration.package_root || registration.package_root_size == 0) {
                throw std::runtime_error("IV package registration has no source root");
            }
            auto const registration_root = normalize(std::filesystem::path(std::string(
                registration.package_root, registration.package_root_size)));
            if (registration_root != normalize(root.module_dir)) {
                throw std::runtime_error(
                    "IV package registration belongs to a different source root");
            }
        }
        auto node_types = reinterpret_cast<iv_module_node_types_fn>(
            library->symbol("iv_module_node_types"));
        auto source_node_types = reinterpret_cast<iv_package_node_types_fn>(
            library->symbol("iv_package_node_types"));
        if (!node_types || !source_node_types) {
            throw std::runtime_error(
                "IV package binary '" + artifact.string()
                + "' does not export its node type definition tables");
        }
        auto const type_view = node_types();
        if (!type_view.data && type_view.size != 0) {
            throw std::runtime_error("IV package node type view has null data");
        }
        if (type_view.size % sizeof(details::NodeCompilerRecord) != 0) {
            throw std::runtime_error("IV package node type table has invalid size");
        }
        auto const types = std::span(
            static_cast<details::NodeCompilerRecord const*>(type_view.data),
            type_view.size / sizeof(details::NodeCompilerRecord));
        auto const source_node_type_view = source_node_types();
        if (!source_node_type_view.data && source_node_type_view.size != 0) {
            throw std::runtime_error("IV package node type definition view has null data");
        }
        if (source_node_type_view.size % sizeof(SourceNodeTypeData) != 0) {
            throw std::runtime_error("IV package node type definition table has invalid size");
        }
        auto const source_node_type_data = std::span(
            static_cast<SourceNodeTypeData const*>(source_node_type_view.data),
            source_node_type_view.size / sizeof(SourceNodeTypeData));

        std::vector<LoadedNodeType> loaded_node_types;
        loaded_node_types.reserve(source_node_type_data.size());
        std::unordered_set<std::string> node_type_ids;
        for (auto const& node_type : source_node_type_data) {
            if (!node_type.id.data || node_type.id.size == 0) {
                throw std::runtime_error("IV package node type ID view is empty");
            }
            auto node_type_id = std::string(
                static_cast<char const*>(node_type.id.data), node_type.id.size);
            if (!node_type_ids.insert(node_type_id).second) {
                throw std::runtime_error(
                    "IV package artifact contains duplicate node type ID '"
                    + node_type_id + "'");
            }
            auto const compiler_record = std::ranges::find(
                types, node_type.code_key, &details::NodeCompilerRecord::code_key);
            if (compiler_record == types.end()) {
                throw std::runtime_error(
                    "IV package node type '" + node_type_id
                    + "' references an unknown NodeCodeKey");
            }
            if (!node_type.configured_graph.data && node_type.configured_graph.size != 0) {
                throw std::runtime_error(
                    "IV package node type '" + node_type_id
                    + "' has a null configured graph view");
            }
            if (!node_type.node_configs.data && node_type.node_configs.size != 0) {
                throw std::runtime_error(
                    "IV package node type '" + node_type_id
                    + "' has a null node config view");
            }
            if (node_type.node_configs.size % sizeof(ModuleNodeConfigRecord) != 0) {
                throw std::runtime_error(
                    "IV package node type '" + node_type_id
                    + "' has an invalid node config table");
            }
            auto const node_graph_archive = std::span(
                static_cast<std::byte const*>(node_type.configured_graph.data),
                node_type.configured_graph.size);
            auto const node_configs = std::span(
                static_cast<ModuleNodeConfigRecord const*>(node_type.node_configs.data),
                node_type.node_configs.size / sizeof(ModuleNodeConfigRecord));
            loaded_node_types.push_back({
                .node_type_id = std::move(node_type_id),
                .compiler_record = *compiler_record,
                .package_path = root.module_dir,
                .module_refs = {binary},
                .configured_graph = std::make_shared<ConfiguredGraph const>(
                    deserialize_configured_graph(node_graph_archive, types, node_configs)),
            });
        }

        std::vector<ModuleDependency> dependencies;
        // A loaded source watches only its implementation package. Imported
        // source implementation edits are resolved through the registry and
        // do not invalidate this source's cached ConfiguredGraph.
        dependencies.push_back({
            root.package_key,
            root.module_dir,
            root.entry_file,
            root.package_stamp,
        });
        std::sort(
            dependencies.begin(),
            dependencies.end(),
            [](auto const &a, auto const &b) {
                if (a.id != b.id) return a.id < b.id;
                return a.module_dir < b.module_dir;
            });

        std::vector<LoadedDefinition> definitions;
        if (log_sink_) {
            log_sink_(
                "[source-graph-deserialization] elapsed_us=" +
                std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - source_deserialization_started_at).count()));
        }
        return {
            .definitions = std::move(definitions),
            .node_types = std::move(loaded_node_types),
            .dependencies = std::move(dependencies),
            .package_code = binary,
        };
    }

    ModuleLoader::LoadedPackage configure_iv_modules(
        CompiledPackage const& compiled,
        ModuleLoader::LoadedPackage source,
        std::vector<std::shared_ptr<LoadedBinary>> const& loaded_binaries,
        std::vector<ModuleDependency> configuration_dependencies) const
    {
        auto const root_binary = std::static_pointer_cast<LoadedBinary>(
            source.package_code);
        if (!root_binary || !root_binary->library) {
            throw std::logic_error("loaded IV package has no binary");
        }

        std::vector<details::BuilderPackageView> source_views;
        source_views.reserve(loaded_binaries.size());
        for (auto const& binary : loaded_binaries) {
            if (!binary || !binary->library || binary->registrations.empty()) continue;
            source_views.push_back({
                .package_root = binary->package_root,
                .registrations = binary->registrations,
                .config_pointer_fields = binary->config_pointer_fields,
                .retained_globals = binary->retained_globals,
            });
        }

        std::sort(
            configuration_dependencies.begin(),
            configuration_dependencies.end(),
            [](auto const& lhs, auto const& rhs) {
                if (lhs.id != rhs.id) return lhs.id < rhs.id;
                return lhs.module_dir < rhs.module_dir;
            });
        configuration_dependencies.erase(
            std::unique(
                configuration_dependencies.begin(),
                configuration_dependencies.end(),
                [](auto const& lhs, auto const& rhs) {
                    return lhs.id == rhs.id && lhs.module_dir == rhs.module_dir;
                }),
            configuration_dependencies.end());

        auto const started_at = std::chrono::steady_clock::now();
        std::vector<LoadedDefinition> definitions;
        std::unordered_set<std::string> module_ids;
        for (auto const& registration : root_binary->registrations) {
            if (registration.kind != details::PackageRegistrationKind::module) continue;
            if (!registration.id || registration.id_size == 0 || !registration.module_build) {
                throw std::runtime_error("IV module registration is incomplete");
            }
            auto module_id = std::string(registration.id, registration.id_size);
            if (!module_ids.insert(module_id).second) {
                throw std::runtime_error(
                    "IV package contains duplicate iv module ID '" + module_id + "'");
            }

            auto session = std::unique_ptr<details::BuilderSession,
                decltype(&details::iv_builder_session_destroy)>(
                    details::iv_builder_session_create(),
                    details::iv_builder_session_destroy);
            if (!session) {
                throw std::runtime_error("could not create graph configuration session");
            }
            details::set_builder_packages(session.get(), source_views);
            auto const root_source_index = details::builder_package_index(
                session.get(), root_binary->package_root);
            details::select_builder_package(session.get(), root_source_index);
            details::begin_builder_module(session.get(), module_id);
            struct ModuleCallScope {
                details::BuilderSession* session = nullptr;
                ~ModuleCallScope() { details::end_builder_module(session); }
            } const module_call{session.get()};

            GraphBuilder builder(session.get());
            registration.module_build(builder);
            auto configured = std::make_shared<ConfiguredGraph const>(
                details::take_built_graph(session.get()));
            auto plan = GraphCompiler::compile(
                GraphLowerer::lower(*configured, {.execution_root = true}));
            auto runtime_root = std::make_shared<RuntimeGraphRoot>(std::move(plan.graph));

            std::vector<ModuleRef> refs;
            refs.reserve(loaded_binaries.size() + 1);
            for (auto const& binary : loaded_binaries) refs.push_back(binary);
            refs.push_back(runtime_root);
            definitions.emplace_back(
                std::move(refs),
                WeakTypeErasedNode(*runtime_root),
                std::move(plan.introspection),
                compiled.root.module_dir,
                std::move(module_id),
                configuration_dependencies,
                std::move(configured));
        }
        if (log_sink_) {
            log_sink_(
                "[graph-configuration] elapsed_us="
                + std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - started_at).count()));
        }
        source.definitions = std::move(definitions);
        source.dependencies = std::move(configuration_dependencies);
        return source;
    }

    ModuleLoader::LoadedPackage load_package(
        std::filesystem::path const& path) const
    {
        std::lock_guard lock(mutex_);
        if (toolchain_.compile_stage != ModuleCompileStage::full) {
            throw std::logic_error(
                "only the full module compile stage can be loaded");
        }

        auto root_compiled = compile_package_unlocked(path);
        std::vector<CompiledPackage> compiled_sources;
        compiled_sources.reserve(root_compiled.configuration_packages.size());
        for (auto const& candidate : root_compiled.configuration_packages) {
            if (normalize(candidate.module_dir) == normalize(root_compiled.root.module_dir)) {
                compiled_sources.push_back(root_compiled);
                continue;
            }
            try {
                compiled_sources.push_back(compile_package_unlocked(candidate.module_dir));
            } catch (std::exception const& exception) {
                // Broken unrelated IV packages are omitted. If graph
                // configuration actually requests one of their registered IDs,
                // BuilderSession reports that provider as unavailable.
                if (log_sink_) {
                    log_sink_(
                        "[graph-configuration-source-skipped] root="
                        + candidate.module_dir.generic_string()
                        + " error=" + exception.what());
                }
            }
        }

        std::optional<ModuleLoader::LoadedPackage> root_source;
        std::vector<std::shared_ptr<LoadedBinary>> loaded_binaries;
        std::vector<ModuleDependency> configuration_dependencies;
        loaded_binaries.reserve(compiled_sources.size());
        for (auto const& compiled : compiled_sources) {
            auto const is_root = normalize(compiled.root.module_dir)
                == normalize(root_compiled.root.module_dir);
            try {
                auto loaded = load_compiled_package(compiled);
                if (loaded.package_code) {
                    loaded_binaries.push_back(
                        std::static_pointer_cast<LoadedBinary>(loaded.package_code));
                }
                configuration_dependencies.insert(
                    configuration_dependencies.end(),
                    loaded.dependencies.begin(), loaded.dependencies.end());
                if (is_root) root_source = std::move(loaded);
            } catch (std::exception const& exception) {
                if (is_root) throw;
                if (log_sink_) {
                    log_sink_(
                        "[graph-configuration-source-load-skipped] root="
                        + compiled.root.module_dir.generic_string()
                        + " error=" + exception.what());
                }
            }
        }
        if (!root_source) {
            throw std::logic_error("loaded IV packages omitted the requested source");
        }

        return configure_iv_modules(
            root_compiled,
            std::move(*root_source),
            loaded_binaries,
            std::move(configuration_dependencies));
    }

};

ModuleLoader::LoadedDefinition::LoadedDefinition(
    std::vector<ModuleRef> refs,
    WeakTypeErasedNode root_,
    GraphIntrospectionMetadata introspection_,
    std::filesystem::path path,
    std::string id,
    std::vector<ModuleDependency> deps,
    std::shared_ptr<ConfiguredGraph const> configured_graph_)
    : module_refs(std::move(refs)),
      root(root_),
      introspection(std::move(introspection_)),
      package_path(std::move(path)),
      module_id(std::move(id)),
      dependencies(std::move(deps)),
      configured_graph(std::move(configured_graph_))
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

ModuleLoader::LoadedPackage ModuleLoader::load_package(
    std::filesystem::path const &path) const
{
    return _impl->load_package(path);
}

std::vector<ModuleLoader::LoadedDefinition> ModuleLoader::load_package_definitions(
    std::filesystem::path const &path) const
{
    auto source = load_package(path);
    return std::move(source.definitions);
}

std::filesystem::path ModuleLoader::compile_package(
    std::filesystem::path const& path) const
{
    return _impl->compile_package(path);
}

std::vector<std::filesystem::path> const &ModuleLoader::extra_search_roots() const
{
    return _impl->extra_search_roots;
}
} // namespace iv
