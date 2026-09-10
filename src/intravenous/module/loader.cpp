#include <intravenous/module/loader.h>
#include <intravenous/module/abi.h>
#include <intravenous/module/authored_graph_wire.h>
#include <intravenous/module/source_manifest.h>
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
#include <regex>
#include <ranges>
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
    std::filesystem::path entry;
};

std::optional<std::filesystem::path> find_source_manifest(
    std::filesystem::path const& directory)
{
    auto const source_manifest = directory / IV_SOURCE_MANIFEST_FILE;
    return std::filesystem::exists(source_manifest)
        ? std::optional<std::filesystem::path>{source_manifest}
        : std::nullopt;
}

struct ResolvedModule {
    Manifest manifest;
    std::string source_key;
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

std::string read_text(std::filesystem::path const& path);

enum class SourceDefinitionKind {
    node,
    module,
};

struct SourceDefinitionInterface {
    std::string id;
    SourceDefinitionKind kind{};
    std::string function;
};

std::vector<SourceDefinitionInterface> scan_source_definitions(
    std::filesystem::path const& entry)
{
    // IV_MODULE is intentionally a simple declaration spelling.  The source
    // introspection plugin remains authoritative for rich metadata, while
    // this small loader scan creates the generated C++ import declaration
    // before that source has been built.
    static std::regex const module_registration(
        R"iv(IV_MODULE\s*\(\s*"([^"]+)"\s*,\s*([A-Za-z_][A-Za-z0-9_]*(?:::[A-Za-z_][A-Za-z0-9_]*)*)\s*\))iv");
    static std::regex const node_registration(
        R"iv(IV_NODE\s*\(\s*"([^"]+)"\s*,\s*([A-Za-z_][A-Za-z0-9_:]*)\s*\))iv");
    std::vector<SourceDefinitionInterface> result;
    auto const source = read_text(entry);
    auto append = [&](SourceDefinitionInterface interface) {
        if (interface.id.empty()) {
            throw std::runtime_error(
                "IV source registration has an empty stable ID in '" + entry.string() + "'");
        }
        if (interface.id.contains('/') || interface.id.contains('\\')) {
            throw std::runtime_error(
                "IV source registration ID must not contain a path separator in '"
                + entry.string() + "'");
        }
        auto const duplicate = std::find_if(
            result.begin(), result.end(), [&](SourceDefinitionInterface const& existing) {
                return existing.id == interface.id;
            });
        if (duplicate != result.end()) {
            throw std::runtime_error(
                "duplicate stable IV source registration '" + interface.id + "' in '"
                + entry.string() + "'");
        }
        result.push_back(std::move(interface));
    };
    for (std::sregex_iterator it(
             source.begin(), source.end(), module_registration), end;
         it != end; ++it) {
        append({
            .id = (*it)[1].str(),
            .kind = SourceDefinitionKind::module,
            .function = (*it)[2].str(),
        });
    }
    for (std::sregex_iterator it(source.begin(), source.end(), node_registration), end;
         it != end; ++it) {
        append({
            .id = (*it)[1].str(),
            .kind = SourceDefinitionKind::node,
            .function = {},
        });
    }
    return result;
}

void emit_module_function_declaration(
    std::ostream& output,
    std::string_view qualified_function)
{
    std::vector<std::string_view> components;
    for (std::size_t begin = 0; begin < qualified_function.size();) {
        auto const end = qualified_function.find("::", begin);
        components.push_back(qualified_function.substr(
            begin,
            end == std::string_view::npos ? std::string_view::npos : end - begin));
        if (end == std::string_view::npos) break;
        begin = end + 2;
    }
    for (std::size_t index = 0; index + 1 < components.size(); ++index) {
        output << "namespace " << components[index] << " {\n";
    }
    output << "void " << components.back() << "(iv::GraphBuilder&);\n";
    for (std::size_t index = components.size(); index-- > 1;) {
        output << "} // namespace " << components[index - 1] << "\n";
    }
}

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
        manifest.entry = json.at("entry").get<std::string>();
    } catch (nlohmann::json::exception const &e) {
        throw std::runtime_error("invalid IV source manifest '" + file.string() + "': " + e.what());
    }

    if (manifest.schema != 2) {
        throw std::runtime_error(
            "IV source manifest '" + file.string() + "' uses unsupported schema " +
            std::to_string(manifest.schema));
    }
    if (manifest.entry.empty() || manifest.entry.is_absolute()) {
        throw std::runtime_error(
            "IV source manifest '" + file.string() + "' entry must be a relative path");
    }
    return manifest;
}

std::string source_key_for(std::filesystem::path const& directory)
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
            "cannot derive IV source package key from '" + normalized.string() + "'");
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
        R"(^\s*#\s*include\s*<iv/(nodes-global|nodes)/([^>]+)>\s*$)");

    std::vector<ModuleImport> imports;
    std::istringstream stream(read_text(entry));
    for (std::string line; std::getline(stream, line);) {
        std::smatch match;
        if (!std::regex_match(line, match, pattern)) {
            continue;
        }
        ModuleImport import{
            .id = match[2].str(),
            .global_only = match[1].str() == "nodes-global",
        };
        if (import.id.empty() || import.id.contains('/') || import.id.contains('\\')) {
            throw std::runtime_error(
                "invalid IV node interface import <iv/" + match[1].str() + "/" + import.id +
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

std::string_view compile_stage_name(ModuleCompileStage stage)
{
    switch (stage) {
    case ModuleCompileStage::full: return "full";
    case ModuleCompileStage::authoring: return "authoring";
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

    struct Registry {
        std::unordered_map<std::string, ResolvedModule> project;
        std::unordered_map<std::string, ResolvedModule> global;
        std::unordered_map<std::string, ResolvedModule> effective;
    };

    struct Closure {
        std::vector<ResolvedModule> modules;
        std::unordered_map<std::string, std::vector<std::string>> dependency_keys;
    };

    struct CompiledRoot {
        ResolvedModule root;
        Closure closure;
        std::filesystem::path artifact;
    };

    static std::string key(ResolvedModule const &module)
    {
        return std::string(module.global ? "global:" : "project:")
            + normalize(module.module_dir).generic_string();
    }

    static void register_source_definitions(
        ResolvedModule const& source,
        std::unordered_map<std::string, ResolvedModule>& registry)
    {
        for (auto const& definition : scan_source_definitions(source.entry_file)) {
            auto [position, inserted] = registry.emplace(definition.id, source);
            if (!inserted && position->second.manifest_file != source.manifest_file) {
                throw std::runtime_error(
                    "duplicate stable IV definition ID '" + definition.id + "' in '"
                    + position->second.manifest_file.string() + "' and '"
                    + source.manifest_file.string() + "'");
            }
        }
    }

    ResolvedModule resolve_dir(std::filesystem::path dir, bool global) const
    {
        dir = normalize(dir);
        auto manifest_file = find_source_manifest(dir);
        if (!manifest_file) {
            throw std::runtime_error(
                "IV source directory '" + dir.string() + "' does not contain " +
                std::string(IV_SOURCE_MANIFEST_FILE));
        }

        auto manifest = parse_manifest(*manifest_file);
        auto entry = normalize(dir / manifest.entry);
        if (!std::filesystem::exists(entry) || !std::filesystem::is_regular_file(entry)) {
            throw std::runtime_error(
                "IV source manifest '" + manifest_file->string() + "' entry does not exist: " +
                manifest.entry.string());
        }
        if (!is_within(entry, dir)) {
            throw std::runtime_error(
                "IV source manifest entry escapes source directory: " + manifest.entry.string());
        }

        return {
            .manifest = std::move(manifest),
            .source_key = source_key_for(dir),
            .module_dir = dir,
            .manifest_file = normalize(*manifest_file),
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
            if (!it->is_regular_file()
                || !is_iv_source_manifest_file(it->path().filename().string())) {
                continue;
            }

            auto const source_dir = it->path().parent_path();
            auto manifest = find_source_manifest(source_dir);
            if (!manifest || normalize(it->path()) != normalize(*manifest)) {
                continue;
            }
            auto resolved = resolve_dir(source_dir, global);
            register_source_definitions(resolved, out);
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
                            "warning: project IV source '" + id + "' at '" +
                            module.manifest_file.string() + "' shadows global IV source at '" +
                            global->second.manifest_file.string() + "'");
                    }
                }
                registry.effective[id] = module;
            }
            register_source_definitions(root, registry.effective);
        } else {
            register_source_definitions(root, registry.global);
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
                    "IV source '" + from.source_key + "' imports missing " +
                std::string((from.global || import.global_only) ? "global" : "project/global") +
                " IV source '" + import.id + "'");
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
                    "cyclic IV source import involving '" + module.source_key + "'");
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

    std::filesystem::path build(
        ResolvedModule const &root,
        Closure const &closure,
        Registry const& registry,
        std::filesystem::path const &project_root) const
    {
        auto const global_import_root = global_cache_root_ / "imports";
        auto const project_iv_root = project_root / "build/iv";
        auto const project_import_root = root.global
            ? global_import_root
            : project_iv_root / "imports";
        auto const owner_root = root.global ? global_cache_root_ : project_iv_root;
        auto const build_key = sanitize(root.source_key) + "_" + stable_hash(root.module_dir);
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
        std::filesystem::create_directories(project_import_root / "iv/nodes");
        std::filesystem::create_directories(global_import_root / "iv/nodes");
        std::filesystem::create_directories(global_import_root / "iv/nodes-global");

        // Interface headers are generated for every discovered definition,
        // not merely this source's import closure.  That makes the generated
        // tree a project/application registry view while CMake still compiles
        // only the closure needed by this source artifact.
        std::vector<ResolvedModule> header_sources;
        std::unordered_set<std::string> seen_header_sources;
        auto collect_header_sources = [&](auto const& providers) {
            for (auto const& [_, provider] : providers) {
                if (seen_header_sources.insert(key(provider)).second) {
                    header_sources.push_back(provider);
                }
            }
        };
        collect_header_sources(registry.effective);
        // A project provider may shadow a global ID in <iv/nodes/...>, but
        // <iv/nodes-global/...> is an explicit escape hatch and still needs
        // its global definition header generated.
        collect_header_sources(registry.global);
        std::ranges::sort(header_sources, {}, [](ResolvedModule const& source) {
            return source.manifest_file.generic_string();
        });
        for (auto const &module : header_sources) {
            for (auto const& registration : scan_source_definitions(module.entry_file)) {
                auto const definition_import = (module.global
                    ? global_import_root / "iv/nodes-global"
                    : project_import_root / "iv/nodes") / registration.id;
                std::ostringstream interface;
                interface << "#pragma once\n"
                          << "#include <intravenous/dsl.h>\n";
                if (registration.kind == SourceDefinitionKind::module) {
                    emit_module_function_declaration(interface, registration.function);
                    interface << "IV_MODULE_INTERFACE(\"" << registration.id << "\", "
                              << registration.function << ");\n";
                } else {
                    interface << "IV_NODE_INTERFACE(\"" << registration.id << "\");\n";
                }
                write_text_if_different(definition_import, interface.str());
                if (module.global) {
                    auto const forwarder = global_import_root / "iv/nodes" / registration.id;
                    write_text_if_different(
                        forwarder,
                        "#pragma once\n#include <iv/nodes-global/" + registration.id + ">\n");
                }
            }
        }

        std::ostringstream export_tu;
        // The generated root owns the module ABI symbols, so it must include
        // their declaration directly rather than rely on the DSL's transitive
        // includes.
        export_tu << "#include <intravenous/module/abi.h>\n"
                  << "#include <intravenous/dsl.h>\n"
                  << "#include <cstddef>\n";
        export_tu
            << "extern \"C\" IV_MODULE_EXPORT std::uint32_t "
               "iv_module_abi_version() {\n"
            << "  return iv::IV_MODULE_ABI_VERSION;\n"
            << "}\n"
            << "extern \"C\" std::size_t "
               "iv_source_registered_module_count() {\n"
            << "  return iv::details::source_module_count();\n"
            << "}\n"
            << "extern \"C\" iv::ModuleDataView "
               "iv_source_registered_module_id(std::size_t index) {\n"
            << "  auto const registration = iv::details::source_module_at(index);\n"
            << "  return {registration.id, registration.id_size};\n"
            << "}\n"
            << "extern \"C\" void "
               "iv_source_build_registered_module(std::size_t index, "
               "iv::details::BuilderSession* session) {\n"
            << "  iv::GraphBuilder builder{session};\n"
            << "  iv::details::source_module_at(index).module_build(builder);\n"
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
        for (auto const &module : closure.modules) {
            signature << key(module) << '\n'
                      << read_text(module.manifest_file) << '\n'
                      << module.source_stamp.time_since_epoch().count() << '\n';
        }
        if (std::filesystem::exists(custom_cmake)) {
            signature << read_text(custom_cmake) << '\n';
        }
        auto const signature_file = workspace / "build.signature";
        auto const artifact_name = library_name("iv_source_" + sanitize(root.source_key));
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
        std::ostringstream source_list;
        for (std::size_t i = 0; i < closure.modules.size(); ++i) {
            if (i) source_list << ';';
            source_list << closure.modules[i].entry_file.generic_string();
        }

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
                  << " -DIV_MODULE_GENERATED_INCLUDE_DIR=" << quote(project_import_root)
                  << " -DIV_GLOBAL_MODULE_GENERATED_INCLUDE_DIR=" << quote(global_import_root)
                  << " -DIV_MODULE_INCLUDE_DIRS=\"" << include_list.str() << "\""
                  << " -DIV_MODULE_SOURCE_FILES=\"" << source_list.str() << "\""
                  << " -DIV_MODULE_OUTPUT_DIR=" << quote(output_dir)
                  << " -DIV_MODULE_OUTPUT_NAME=iv_source_" << sanitize(root.source_key)
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
        auto const copy_started_at = std::chrono::steady_clock::now();
        std::filesystem::copy_file(
            artifact,
            generation_artifact,
            std::filesystem::copy_options::overwrite_existing);
        if (log_sink_) {
            log_sink_(
                "[generation-copy] elapsed_us=" +
                std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - copy_started_at).count()));
        }
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

    CompiledRoot compile_source_unlocked(
        std::filesystem::path const& path) const
    {
        auto module_path = normalize(path);
        if (std::filesystem::is_regular_file(module_path)) {
            if (is_iv_source_manifest_file(module_path.filename().string())) {
                module_path = module_path.parent_path();
            } else {
                throw std::runtime_error(
                    "root source path must be an IV source directory, " +
                    std::string(IV_SOURCE_MANIFEST_FILE));
            }
        }

        bool const global_root = std::ranges::any_of(
            extra_search_roots,
            [&](auto const& root) { return is_within(module_path, root); });
        auto root = resolve_dir(module_path, global_root);
        auto const project_root = root.global
            ? global_cache_root_
            : discover_project_root(root.module_dir);
        auto registry = registry_for(root, project_root);
        auto closure = reachable_modules(root, registry);
        auto artifact = build(root, closure, registry, project_root);
        return {
            .root = std::move(root),
            .closure = std::move(closure),
            .artifact = std::move(artifact),
        };
    }

    std::filesystem::path compile_source(
        std::filesystem::path const& path) const
    {
        std::lock_guard lock(mutex_);
        return compile_source_unlocked(path).artifact;
    }

    ModuleLoader::LoadedSource load_source(
        std::filesystem::path const &path) const
    {
        std::lock_guard lock(mutex_);
        if (toolchain_.compile_stage != ModuleCompileStage::full) {
            throw std::logic_error(
                "only the full module compile stage can be loaded");
        }
        auto compiled = compile_source_unlocked(path);
        auto& root = compiled.root;
        auto& closure = compiled.closure;
        auto& artifact = compiled.artifact;

        auto const dynamic_library_started_at = std::chrono::steady_clock::now();
        auto library = std::make_shared<DynamicLibrary>(artifact);
        if (log_sink_) {
            log_sink_(
                "[dynamic-library-load] elapsed_us=" +
                std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - dynamic_library_started_at).count()));
        }

        auto const graph_materialization_started_at = std::chrono::steady_clock::now();
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
        auto source_module_count = reinterpret_cast<iv_source_module_count_fn>(
            library->symbol("iv_source_module_count"));
        auto source_module_id = reinterpret_cast<iv_source_module_id_fn>(
            library->symbol("iv_source_module_id"));
        auto authored_graph = reinterpret_cast<iv_source_module_authored_graph_fn>(
            library->symbol("iv_source_module_authored_graph"));
        auto node_configs = reinterpret_cast<iv_source_module_node_configs_fn>(
            library->symbol("iv_source_module_node_configs"));
        auto node_types = reinterpret_cast<iv_module_node_types_fn>(
            library->symbol("iv_module_node_types"));
        if (!source_module_count || !source_module_id || !authored_graph
            || !node_configs || !node_types) {
            throw std::runtime_error(
                "IV source artifact '" + artifact.string()
                + "' does not export the finalized source definition tables");
        }
        auto const type_view = node_types();
        if (!type_view.data && type_view.size != 0) {
            throw std::runtime_error("IV source node type view has null data");
        }
        if (type_view.size % sizeof(details::NodeCompilerRecord) != 0) {
            throw std::runtime_error("IV source node type table has invalid size");
        }
        auto const types = std::span(
            static_cast<details::NodeCompilerRecord const*>(type_view.data),
            type_view.size / sizeof(details::NodeCompilerRecord));

        auto binary = std::make_shared<LoadedBinary>(LoadedBinary{
            root.source_key,
            artifact,
            library,
        });

        std::vector<ModuleDependency> dependencies;
        dependencies.reserve(closure.modules.size());
        for (auto const &module : closure.modules) {
            dependencies.push_back({
                module.source_key,
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

        auto const module_count = source_module_count();
        std::vector<LoadedDefinition> definitions;
        definitions.reserve(module_count);
        std::unordered_set<std::string> module_ids;
        for (std::size_t index = 0; index < module_count; ++index) {
            auto const id_view = source_module_id(index);
            auto const graph_view = authored_graph(index);
            auto const config_view = node_configs(index);
            if (!id_view.data || id_view.size == 0) {
                throw std::runtime_error("IV source module ID view is empty");
            }
            if (!graph_view.data && graph_view.size != 0) {
                throw std::runtime_error("IV source authored graph view has null data");
            }
            if (!config_view.data && config_view.size != 0) {
                throw std::runtime_error("IV source node config view has null data");
            }
            if (config_view.size % sizeof(ModuleNodeConfigRecord) != 0) {
                throw std::runtime_error("IV source node config table has invalid size");
            }
            auto module_id = std::string(
                static_cast<char const*>(id_view.data), id_view.size);
            if (!module_ids.insert(module_id).second) {
                throw std::runtime_error(
                    "IV source artifact contains duplicate module ID '" + module_id + "'");
            }
            auto const graph_archive = std::span(
                static_cast<std::byte const*>(graph_view.data), graph_view.size);
            auto const configs = std::span(
                static_cast<ModuleNodeConfigRecord const*>(config_view.data),
                config_view.size / sizeof(ModuleNodeConfigRecord));
            auto authored = deserialize_authored_graph(graph_archive, types, configs);
            auto plan = GraphCompiler::compile(
                GraphLowerer::lower(authored, {.execution_root = true}));
            auto runtime_root = std::make_shared<RuntimeGraphRoot>(
                std::move(plan.graph));
            std::vector<ModuleRef> refs;
            refs.push_back(binary);
            refs.push_back(runtime_root);
            definitions.emplace_back(
                std::move(refs),
                WeakTypeErasedNode(*runtime_root),
                std::move(plan.introspection),
                root.module_dir,
                std::move(module_id),
                dependencies);
        }
        if (log_sink_) {
            log_sink_(
                "[runtime-graph-materialization] elapsed_us=" +
                std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - graph_materialization_started_at).count()));
        }
        return {
            .definitions = std::move(definitions),
            .dependencies = std::move(dependencies),
        };
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
      source_path(std::move(path)),
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

ModuleLoader::LoadedSource ModuleLoader::load_source(
    std::filesystem::path const &path) const
{
    return _impl->load_source(path);
}

std::vector<ModuleLoader::LoadedDefinition> ModuleLoader::load_source_definitions(
    std::filesystem::path const &path) const
{
    auto source = load_source(path);
    return std::move(source.definitions);
}

std::filesystem::path ModuleLoader::compile_source(
    std::filesystem::path const& path) const
{
    return _impl->compile_source(path);
}

std::vector<std::filesystem::path> const &ModuleLoader::extra_search_roots() const
{
    return _impl->extra_search_roots;
}
} // namespace iv
