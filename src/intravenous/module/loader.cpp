#include <intravenous/module/loader.h>
#include <intravenous/module/abi.h>
#include <intravenous/module/authored_graph_wire.h>
#include <intravenous/module/source_manifest.h>
#include <intravenous/compat.h>
#include <intravenous/graph/builder/lowering.hpp>
#include <intravenous/graph/builder/embedder.hpp>
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

std::vector<std::filesystem::path> source_discovery_files(
    std::filesystem::path const& entry)
{
    // This remains only the bootstrap discovery pass needed to make generated
    // ID headers available before Clang has built a fresh provider.  It scans
    // the complete source package (rather than just the manifest entry) so
    // registrations in headers and custom-CMake translation units participate
    // in discovery.  The compiler plugin's registration metadata remains the
    // planned authoritative replacement for this textual bridge.
    auto const root = entry.parent_path();
    std::vector<std::filesystem::path> files;
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator it(
             root,
             std::filesystem::directory_options::skip_permission_denied,
             error),
         end;
         it != end;
         it.increment(error)) {
        if (error) break;
        if (it->is_directory()) {
            auto const name = it->path().filename();
            if (name == ".git" || name == "build" || name == ".cache") {
                it.disable_recursion_pending();
            } else if (it->path() != root
                       && std::filesystem::exists(
                           it->path() / std::string(IV_SOURCE_MANIFEST_FILE))) {
                // A nested source package owns its own registrations and
                // generated headers. It is not a translation-unit subtree of
                // this source merely because the directories are nested.
                it.disable_recursion_pending();
            }
            continue;
        }
        if (!it->is_regular_file()) continue;
        auto const extension = it->path().extension().string();
        if (extension == ".c" || extension == ".cc" || extension == ".cpp"
            || extension == ".cxx" || extension == ".h" || extension == ".hh"
            || extension == ".hpp" || extension == ".hxx") {
            files.push_back(it->path());
        }
    }
    if (std::ranges::find(files, entry) == files.end()) files.push_back(entry);
    std::ranges::sort(files, {}, [](auto const& path) { return path.generic_string(); });
    files.erase(std::unique(files.begin(), files.end()), files.end());
    return files;
}

std::string strip_cpp_comments(std::string_view source)
{
    // Bootstrap discovery is deliberately small, but it must not advertise a
    // registration or import merely mentioned in a comment. Preserve line
    // breaks so diagnostic locations and line-oriented include recognition
    // remain meaningful; leave literals untouched because registration IDs
    // themselves are string literals.
    std::string result(source);
    enum class State { code, string_literal, character_literal, line_comment, block_comment };
    State state = State::code;
    bool escaped = false;
    for (std::size_t index = 0; index < result.size(); ++index) {
        auto& character = result[index];
        auto const next = index + 1 < result.size() ? result[index + 1] : '\0';
        switch (state) {
        case State::code:
            if (character == '/' && next == '/') {
                character = ' ';
                result[++index] = ' ';
                state = State::line_comment;
            } else if (character == '/' && next == '*') {
                character = ' ';
                result[++index] = ' ';
                state = State::block_comment;
            } else if (character == '"') {
                state = State::string_literal;
                escaped = false;
            } else if (character == '\'') {
                state = State::character_literal;
                escaped = false;
            }
            break;
        case State::string_literal:
            if (character == '"' && !escaped) state = State::code;
            escaped = character == '\\' && !escaped;
            if (character != '\\') escaped = false;
            break;
        case State::character_literal:
            if (character == '\'' && !escaped) state = State::code;
            escaped = character == '\\' && !escaped;
            if (character != '\\') escaped = false;
            break;
        case State::line_comment:
            if (character == '\n') state = State::code;
            else character = ' ';
            break;
        case State::block_comment:
            if (character == '*' && next == '/') {
                character = ' ';
                result[++index] = ' ';
                state = State::code;
            } else if (character != '\n') {
                character = ' ';
            }
            break;
        }
    }
    return result;
}

enum class SourceDefinitionKind {
    node,
    module,
};

struct SourceDefinitionInterface {
    std::string id;
    SourceDefinitionKind kind{};
};

std::vector<SourceDefinitionInterface> scan_source_definitions(
    std::filesystem::path const& entry)
{
    // The source introspection plugin remains authoritative for registration
    // metadata. This deliberately shallow scan recognizes only the macro and
    // stable string ID, which is enough to create a generated C++ interface
    // before a fresh provider has been built; it does not attempt to parse a
    // function or C++ node-type expression.
    static std::regex const module_registration(
        R"iv(IV_MODULE\s*\(\s*"([^"]+)"\s*,)iv");
    static std::regex const node_registration(
        R"iv(IV_NODE\s*\(\s*"([^"]+)"\s*,)iv");
    std::vector<SourceDefinitionInterface> result;
    auto append = [&](SourceDefinitionInterface interface,
                      std::filesystem::path const& source_file) {
        if (interface.id.empty()) {
            throw std::runtime_error(
                "IV source registration has an empty stable ID in '"
                + source_file.string() + "'");
        }
        if (interface.id.contains('/') || interface.id.contains('\\')) {
            throw std::runtime_error(
                "IV source registration ID must not contain a path separator in '"
                + source_file.string() + "'");
        }
        auto const duplicate = std::find_if(
            result.begin(), result.end(), [&](SourceDefinitionInterface const& existing) {
                return existing.id == interface.id;
            });
        if (duplicate != result.end()) {
            // This scanner reads both headers and translation units. A normal
            // registration macro in a header can therefore be seen once at
            // its spelling and again through its including source file. The
            // compiler-backed registration table is authoritative for
            // duplicate diagnostics; bootstrap discovery needs only one
            // generated interface for this ID.
            return;
        }
        result.push_back(std::move(interface));
    };
    for (auto const& source_file : source_discovery_files(entry)) {
        auto const source = strip_cpp_comments(read_text(source_file));
        for (std::sregex_iterator it(
                 source.begin(), source.end(), module_registration), end;
             it != end; ++it) {
            append({
                .id = (*it)[1].str(),
                .kind = SourceDefinitionKind::module,
            }, source_file);
        }
        for (std::sregex_iterator it(source.begin(), source.end(), node_registration), end;
             it != end; ++it) {
            append({
                .id = (*it)[1].str(),
                .kind = SourceDefinitionKind::node,
            }, source_file);
        }
    }
    return result;
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
    for (auto const& source_file : source_discovery_files(entry)) {
        std::istringstream stream(strip_cpp_comments(read_text(source_file)));
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
                    "> in '" + source_file.string() + "'");
            }
            imports.push_back(std::move(import));
        }
    }
    std::ranges::sort(imports, {}, [](ModuleImport const& imported) {
        return std::pair{imported.id, imported.global_only};
    });
    imports.erase(std::unique(imports.begin(), imports.end(),
        [](ModuleImport const& left, ModuleImport const& right) {
            return left.id == right.id && left.global_only == right.global_only;
        }), imports.end());
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

inline constexpr std::string_view registered_subgraph_prefix = "registered:";

std::vector<std::string> registered_definition_references(AuthoredGraph const& graph)
{
    std::vector<std::string> references;
    for (std::size_t handle = 0; handle < graph.node_bundles.size(); ++handle) {
        auto const& bundle = graph.node_bundles.bundle(handle);
        if (!bundle.is_subgraph()) continue;
        auto const kind = bundle.subgraph_kind();
        if (!kind.starts_with(registered_subgraph_prefix)) continue;
        references.emplace_back(kind.substr(registered_subgraph_prefix.size()));
    }
    std::ranges::sort(references);
    references.erase(std::unique(references.begin(), references.end()), references.end());
    return references;
}

void validate_registered_module_cycles(
    std::unordered_map<std::string, std::shared_ptr<AuthoredGraph const>> const& providers,
    std::unordered_set<std::string> const& module_ids)
{
    // Includes make C++ interfaces available. Only an authored registered-ID
    // edge between two iv modules can recurse. Primitive node types terminate
    // expansion, so they are deliberately absent from this graph.
    std::unordered_set<std::string> visited;
    std::unordered_set<std::string> visiting;
    std::vector<std::string> path;
    auto visit = [&](auto&& self, std::string const& module_id) -> void {
        if (visited.contains(module_id)) return;
        if (!visiting.insert(module_id).second) {
            auto const begin = std::ranges::find(path, module_id);
            std::string message = "cyclic registered IV module dependency: ";
            for (auto it = begin; it != path.end(); ++it) {
                if (it != begin) message += " -> ";
                message += *it;
            }
            throw std::runtime_error(message + " -> " + module_id);
        }
        path.push_back(module_id);
        auto const provider = providers.find(module_id);
        if (provider == providers.end() || !provider->second) {
            throw std::runtime_error(
                "registered IV module '" + module_id + "' has no loaded provider");
        }
        for (auto const& reference : registered_definition_references(*provider->second)) {
            if (module_ids.contains(reference)) self(self, reference);
        }
        path.pop_back();
        visiting.erase(module_id);
        visited.insert(module_id);
    };
    for (auto const& module_id : module_ids) visit(visit, module_id);
}

void resolve_registered_graph_references(
    AuthoredGraph& graph,
    std::unordered_map<std::string, std::shared_ptr<AuthoredGraph const>> const& providers,
    std::vector<std::string>& expansion_stack)
{
    // Imported children are appended while walking, so inspect only the
    // bundles that were originally owned by this graph.  Each provider is
    // recursively resolved before it is copied, which makes the final graph
    // self-contained for the existing GraphLowerer/GraphCompiler path.
    auto const original_bundle_count = graph.node_bundles.size();
    for (std::size_t handle = 0; handle < original_bundle_count; ++handle) {
        auto const& bundle = graph.node_bundles.bundle(handle);
        if (!bundle.is_subgraph()) continue;
        auto const kind = std::string(bundle.subgraph_kind());
        if (!kind.starts_with(registered_subgraph_prefix)) continue;
        auto const id = kind.substr(registered_subgraph_prefix.size());
        auto provider = providers.find(id);
        if (provider == providers.end()) {
            throw std::runtime_error(
                "registered IV definition '" + id + "' has no loaded provider");
        }
        if (std::ranges::find(expansion_stack, id) != expansion_stack.end()) {
            std::string cycle = "cyclic registered IV module dependency: ";
            for (auto const& item : expansion_stack) cycle += item + " -> ";
            throw std::runtime_error(cycle + id);
        }

        expansion_stack.push_back(id);
        auto child = *provider->second;
        resolve_registered_graph_references(child, providers, expansion_stack);
        expansion_stack.pop_back();

        auto const child_begin = graph.node_bundles.size();
        auto const offset = GraphBuilderChildEmbedder::embed(
            graph.node_bundles,
            graph.connections,
            graph.detach,
            graph.virtual_nodes,
            child.public_ports,
            child.node_bundles,
            child.connections,
            child.detach,
            child.virtual_nodes);
        if (offset != child_begin) {
            throw std::logic_error("registered graph provider bundle offset changed unexpectedly");
        }
        graph.node_bundles.replace_registered_subgraph(
            handle,
            offset + child.public_ports.boundary_handle(),
            offset,
            child.node_bundles.size(),
            id);
    }
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
        // Discovery is deliberately non-authoritative.  Multiple sources may
        // temporarily advertise an ID while a definition is moved; retaining
        // all providers lets the runtime registry resolve that transaction
        // without requiring the destination source to be saved twice.
        using Providers = std::vector<ResolvedModule>;
        std::unordered_map<std::string, Providers> project;
        std::unordered_map<std::string, Providers> global;
        std::unordered_map<std::string, Providers> effective;
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
        std::unordered_map<std::string, Registry::Providers>& registry)
    {
        for (auto const& definition : scan_source_definitions(source.entry_file)) {
            auto& providers = registry[definition.id];
            auto const already_present = std::ranges::any_of(
                providers, [&](ResolvedModule const& existing) {
                    return existing.manifest_file == source.manifest_file;
                });
            if (!already_present) {
                providers.push_back(source);
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
        std::unordered_map<std::string, Registry::Providers> &out) const
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
            for (auto const &[id, providers] : registry.project) {
                auto& effective = registry.effective[id];
                for (auto const& provider : providers) {
                    auto const already_present = std::ranges::any_of(
                        effective, [&](ResolvedModule const& existing) {
                            return existing.manifest_file == provider.manifest_file;
                        });
                    if (!already_present) effective.push_back(provider);
                }
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
        auto const& providers = found->second;
        if (providers.empty()) {
            throw std::logic_error("IV definition registry contains an empty provider set");
        }
        if (providers.size() != 1) {
            // Generated headers are identical across candidates, but the
            // current compatibility RuntimeGraphRoot still needs one concrete
            // provider artifact to materialize. Choosing one by source scope
            // or path would silently violate the stable-ID namespace; leave
            // the last valid candidate generation live until the conflict is
            // resolved instead.
            throw std::runtime_error(
                "registered IV definition '" + import.id
                + "' has multiple source candidates");
        }
        return providers.front();
    }

    Closure reachable_modules(
        ResolvedModule const &root,
        Registry const &registry) const
    {
        Closure closure;
        std::unordered_set<std::string> visited;

        auto visit = [&](auto &&self, ResolvedModule const &module) -> void {
            auto const module_key = key(module);
            if (visited.contains(module_key)) return;
            // C++ interface availability is not a graph-recursion relation.
            // Cycles between sources are valid when their referenced IDs end
            // in primitive nodes; module-reference cycles are checked below.
            visited.insert(module_key);

            auto &deps = closure.dependency_keys[module_key];
            for (auto const &import : scan_imports(module.entry_file)) {
                auto const &dependency = resolve_import(module, import, registry);
                self(self, dependency);
                deps.push_back(key(dependency));
            }
            std::sort(deps.begin(), deps.end());
            deps.erase(std::unique(deps.begin(), deps.end()), deps.end());

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
        // not merely this source's import closure.  They are ID-only
        // declarations, so no header encodes whether its provider is a node
        // type or an iv module and duplicate discovery candidates can share
        // one identical generated header.
        std::vector<ResolvedModule> header_sources;
        std::unordered_set<std::string> seen_header_sources;
        auto collect_header_sources = [&](auto const& providers) {
            for (auto const& [_, candidates] : providers) {
                for (auto const& provider : candidates) {
                    if (seen_header_sources.insert(key(provider)).second) {
                        header_sources.push_back(provider);
                    }
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
                          << "#include <intravenous/dsl.h>\n"
                          << "IV_REGISTERED_INTERFACE(\"" << registration.id << "\");\n";
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
                  << "#include <cstddef>\n"
                  << "#include <span>\n"
                  << "#include <string>\n"
                  << "#include <vector>\n";
        export_tu
            << "extern \"C\" IV_MODULE_EXPORT std::uint32_t "
               "iv_module_abi_version() {\n"
            << "  return iv::IV_MODULE_ABI_VERSION;\n"
            << "}\n"
            << "extern \"C\" std::size_t "
               "iv_source_registered_module_count() {\n"
            << "  return iv::details::source_module_count("
            << cxx_string_literal(root.module_dir.generic_string()) << ");\n"
            << "}\n"
            << "extern \"C\" iv::ModuleDataView "
               "iv_source_registered_module_id(std::size_t index) {\n"
            << "  auto const registration = iv::details::source_module_at("
            << cxx_string_literal(root.module_dir.generic_string()) << ", index);\n"
            << "  return {registration.id, registration.id_size};\n"
            << "}\n"
            << "extern \"C\" void "
               "iv_source_build_registered_module(std::size_t index, "
               "iv::details::BuilderSession* session) {\n"
            << "  iv::GraphBuilder builder{session};\n"
            << "  iv::details::source_module_at("
            << cxx_string_literal(root.module_dir.generic_string())
            << ", index).module_build(builder);\n"
            << "}\n"
            << "extern \"C\" std::size_t "
               "iv_source_registered_node_type_count() {\n"
            << "  return iv::details::source_node_count("
            << cxx_string_literal(root.module_dir.generic_string()) << ");\n"
            << "}\n"
            << "extern \"C\" iv::ModuleDataView "
               "iv_source_registered_node_type_id(std::size_t index) {\n"
            << "  auto const registration = iv::details::source_node_at("
            << cxx_string_literal(root.module_dir.generic_string()) << ", index);\n"
            << "  return {registration.id, registration.id_size};\n"
            << "}\n"
            << "extern \"C\" iv::NodeCodeKey "
               "iv_source_registered_node_type_code_key(std::size_t index) {\n"
            << "  return iv::details::source_node_code_key("
            << cxx_string_literal(root.module_dir.generic_string()) << ", index);\n"
            << "}\n"
            << "extern \"C\" void "
               "iv_source_build_registered_node_type(std::size_t index, "
               "iv::details::BuilderSession* session) {\n"
            << "  iv::GraphBuilder builder{session};\n"
            << "  auto node = iv::details::source_node_at("
            << cxx_string_literal(root.module_dir.generic_string())
            << ", index).node_build(builder);\n"
            << "  for (std::size_t input = 0; input < node.sample_input_count(); ++input) {\n"
            << "    auto const config = builder.sample_input_config(\n"
               "        node.node_bundle_handle(), input);\n"
            << "    auto const name = config.name.empty()\n"
               "        ? std::string(\"input\") + std::to_string(input)\n"
               "        : config.name;\n"
            << "    node.connect_input(input, builder.input_named(\n"
               "        name, config.channel_layout, config.default_value, config.min, config.max));\n"
            << "  }\n"
            << "  for (std::size_t input = 0; input < node.event_input_count(); ++input) {\n"
            << "    auto const config = builder.event_input_config(\n"
               "        node.node_bundle_handle(), input);\n"
            << "    auto const name = config.name.empty()\n"
               "        ? std::string(\"eventInput\") + std::to_string(input)\n"
               "        : config.name;\n"
            << "    node.connect_event_input(\n"
               "        input, builder.event_input_named(name, config.type));\n"
            << "  }\n"
            << "  std::vector<iv::SampleOutputRequest> sample_outputs;\n"
            << "  std::vector<std::string> sample_output_names;\n"
            << "  sample_outputs.reserve(node.sample_output_count());\n"
            << "  sample_output_names.reserve(node.sample_output_count());\n"
            << "  for (std::size_t output = 0; output < node.sample_output_count(); ++output) {\n"
            << "    auto port = node[output];\n"
            << "    sample_output_names.push_back(\n"
               "        std::string(\"output\") + std::to_string(output));\n"
            << "    auto const& name = sample_output_names.back();\n"
            << "    sample_outputs.push_back({\n"
            << "      .ref = port,\n"
            << "      .name = name,\n"
            << "      .channel_layout = {.channel_type = port.channel_type, "
               ".sample_layout = iv::SampleStreamLayout::planar},\n"
            << "      .family_name = name,\n"
            << "      .family_channel_type = port.channel_type,\n"
            << "    });\n"
            << "  }\n"
            << "  if (!sample_outputs.empty()) builder.outputs(\n"
               "      std::span<iv::SampleOutputRequest const>(sample_outputs));\n"
            << "  std::vector<iv::EventOutputRequest> event_outputs;\n"
            << "  std::vector<std::string> event_output_names;\n"
            << "  event_outputs.reserve(node.event_output_count());\n"
            << "  event_output_names.reserve(node.event_output_count());\n"
            << "  for (std::size_t output = 0; output < node.event_output_count(); ++output) {\n"
            << "    event_output_names.push_back(\n"
               "        std::string(\"eventOutput\") + std::to_string(output));\n"
            << "    event_outputs.push_back({\n"
            << "      .ref = node.event_port(output),\n"
            << "      .name = event_output_names.back(),\n"
            << "    });\n"
            << "  }\n"
            << "  if (!event_outputs.empty()) builder.event_outputs(\n"
               "      std::span<iv::EventOutputRequest const>(event_outputs));\n"
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
        // A source artifact owns only its own implementation files.  Imported
        // IDs are compile-time interfaces, not implementation timestamps, so
        // a provider's internal edit cannot rebuild every consumer.
        signature << key(root) << '\n'
                  << read_text(root.manifest_file) << '\n'
                  << root.source_stamp.time_since_epoch().count() << '\n';
        for (auto const& imported : scan_imports(root.entry_file)) {
            signature << "import-interface=" << imported.id << ':'
                      << imported.global_only << '\n';
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
        auto artifact = build(root, registry, project_root);
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

    ModuleLoader::LoadedSource load_compiled_source(
        CompiledRoot const& compiled) const
    {
        auto const& root = compiled.root;
        auto const& artifact = compiled.artifact;

        auto const dynamic_library_started_at = std::chrono::steady_clock::now();
        auto library = std::make_shared<DynamicLibrary>(artifact);
        if (log_sink_) {
            log_sink_(
                "[dynamic-library-load] elapsed_us=" +
                std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - dynamic_library_started_at).count()));
        }

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
        auto source_node_types = reinterpret_cast<iv_source_node_types_fn>(
            library->symbol("iv_source_node_types"));
        if (!source_module_count || !source_module_id || !authored_graph
            || !node_configs || !node_types || !source_node_types) {
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
        auto const source_node_type_view = source_node_types();
        if (!source_node_type_view.data && source_node_type_view.size != 0) {
            throw std::runtime_error("IV source node type definition view has null data");
        }
        if (source_node_type_view.size % sizeof(SourceNodeTypeData) != 0) {
            throw std::runtime_error("IV source node type definition table has invalid size");
        }
        auto const source_node_type_data = std::span(
            static_cast<SourceNodeTypeData const*>(source_node_type_view.data),
            source_node_type_view.size / sizeof(SourceNodeTypeData));

        auto binary = std::make_shared<LoadedBinary>(LoadedBinary{
            root.source_key,
            artifact,
            library,
        });

        std::vector<LoadedNodeType> loaded_node_types;
        loaded_node_types.reserve(source_node_type_data.size());
        std::unordered_set<std::string> node_type_ids;
        for (auto const& node_type : source_node_type_data) {
            if (!node_type.id.data || node_type.id.size == 0) {
                throw std::runtime_error("IV source node type ID view is empty");
            }
            auto node_type_id = std::string(
                static_cast<char const*>(node_type.id.data), node_type.id.size);
            if (!node_type_ids.insert(node_type_id).second) {
                throw std::runtime_error(
                    "IV source artifact contains duplicate node type ID '"
                    + node_type_id + "'");
            }
            auto const compiler_record = std::ranges::find(
                types, node_type.code_key, &details::NodeCompilerRecord::code_key);
            if (compiler_record == types.end()) {
                throw std::runtime_error(
                    "IV source node type '" + node_type_id
                    + "' references an unknown NodeCodeKey");
            }
            if (!node_type.authored_graph.data && node_type.authored_graph.size != 0) {
                throw std::runtime_error(
                    "IV source node type '" + node_type_id
                    + "' has a null authored graph view");
            }
            if (!node_type.node_configs.data && node_type.node_configs.size != 0) {
                throw std::runtime_error(
                    "IV source node type '" + node_type_id
                    + "' has a null node config view");
            }
            if (node_type.node_configs.size % sizeof(ModuleNodeConfigRecord) != 0) {
                throw std::runtime_error(
                    "IV source node type '" + node_type_id
                    + "' has an invalid node config table");
            }
            auto const node_graph_archive = std::span(
                static_cast<std::byte const*>(node_type.authored_graph.data),
                node_type.authored_graph.size);
            auto const node_configs = std::span(
                static_cast<ModuleNodeConfigRecord const*>(node_type.node_configs.data),
                node_type.node_configs.size / sizeof(ModuleNodeConfigRecord));
            loaded_node_types.push_back({
                .node_type_id = std::move(node_type_id),
                .compiler_record = *compiler_record,
                .source_path = root.module_dir,
                .module_refs = {binary},
                .authored_graph = std::make_shared<AuthoredGraph const>(
                    deserialize_authored_graph(node_graph_archive, types, node_configs)),
            });
        }

        std::vector<ModuleDependency> dependencies;
        // A loaded source watches only its implementation package. Imported
        // source implementation edits are resolved through the registry and
        // do not invalidate this source's cached AuthoredGraph.
        dependencies.push_back({
            root.source_key,
            root.module_dir,
            root.entry_file,
            root.source_stamp,
        });
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
            if (node_type_ids.contains(module_id)) {
                throw std::runtime_error(
                    "IV source artifact uses registered ID '" + module_id
                    + "' for both a node type and an IV module");
            }
            auto const graph_archive = std::span(
                static_cast<std::byte const*>(graph_view.data), graph_view.size);
            auto const configs = std::span(
                static_cast<ModuleNodeConfigRecord const*>(config_view.data),
                config_view.size / sizeof(ModuleNodeConfigRecord));
            auto authored = std::make_shared<AuthoredGraph const>(
                deserialize_authored_graph(graph_archive, types, configs));
            std::vector<ModuleRef> refs;
            refs.push_back(binary);
            definitions.emplace_back(
                std::move(refs),
                WeakTypeErasedNode{},
                GraphIntrospectionMetadata{},
                root.module_dir,
                std::move(module_id),
                dependencies,
                std::move(authored));
        }
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
        };
    }

    ModuleLoader::LoadedSource load_source(
        std::filesystem::path const& path) const
    {
        std::lock_guard lock(mutex_);
        if (toolchain_.compile_stage != ModuleCompileStage::full) {
            throw std::logic_error(
                "only the full module compile stage can be loaded");
        }

        auto root_compiled = compile_source_unlocked(path);
        auto root_source = load_compiled_source(root_compiled);
        std::vector<ModuleLoader::LoadedSource> provider_sources;
        provider_sources.reserve(root_compiled.closure.modules.size());

        // Each source in the import closure is built and loaded as its own
        // artifact.  The closure remains discovery/watch information only; it
        // is no longer a list of C++ files merged into the caller's target.
        for (auto const& source : root_compiled.closure.modules) {
            if (key(source) == key(root_compiled.root)) continue;
            provider_sources.push_back(load_compiled_source(
                compile_source_unlocked(source.module_dir)));
        }

        std::unordered_map<std::string, std::shared_ptr<AuthoredGraph const>> providers;
        std::unordered_set<std::string> module_ids;
        std::vector<ModuleRef> provider_refs;
        auto register_provider = [&](std::string const& id,
                                     std::shared_ptr<AuthoredGraph const> graph,
                                     std::vector<ModuleRef> const& refs) {
            if (!graph) {
                throw std::logic_error(
                    "loaded registered IV definition has no AuthoredGraph");
            }
            auto [_, inserted] = providers.emplace(id, std::move(graph));
            if (!inserted) {
                throw std::runtime_error(
                    "multiple loaded providers publish registered IV definition '"
                    + id + "'");
            }
            provider_refs.insert(provider_refs.end(), refs.begin(), refs.end());
        };
        auto register_source_providers = [&](ModuleLoader::LoadedSource const& source) {
            for (auto const& definition : source.definitions) {
                module_ids.insert(definition.module_id);
                register_provider(
                    definition.module_id, definition.authored_graph, definition.module_refs);
            }
            for (auto const& node_type : source.node_types) {
                register_provider(
                    node_type.node_type_id, node_type.authored_graph, node_type.module_refs);
            }
        };
        register_source_providers(root_source);
        for (auto const& source : provider_sources) register_source_providers(source);
        validate_registered_module_cycles(providers, module_ids);

        auto const graph_materialization_started_at = std::chrono::steady_clock::now();
        for (auto& definition : root_source.definitions) {
            auto resolved = std::make_shared<AuthoredGraph>(*definition.authored_graph);
            std::vector<std::string> expansion_stack{definition.module_id};
            resolve_registered_graph_references(*resolved, providers, expansion_stack);
            auto plan = GraphCompiler::compile(
                GraphLowerer::lower(*resolved, {.execution_root = true}));
            auto runtime_root = std::make_shared<RuntimeGraphRoot>(std::move(plan.graph));
            definition.module_refs.insert(
                definition.module_refs.end(), provider_refs.begin(), provider_refs.end());
            definition.module_refs.push_back(runtime_root);
            definition.root = WeakTypeErasedNode(*runtime_root);
            definition.introspection = std::move(plan.introspection);
            // Preserve the source-authored graph with its stable registered
            // IDs for the registry/whole-project cache. The flattened copy is
            // only the current RuntimeGraphRoot compatibility materialization
            // and must never replace that cache object.
        }
        if (log_sink_) {
            log_sink_(
                "[runtime-graph-materialization] elapsed_us=" +
                std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now()
                    - graph_materialization_started_at).count()));
        }
        return root_source;
    }
};

ModuleLoader::LoadedDefinition::LoadedDefinition(
    std::vector<ModuleRef> refs,
    WeakTypeErasedNode root_,
    GraphIntrospectionMetadata introspection_,
    std::filesystem::path path,
    std::string id,
    std::vector<ModuleDependency> deps,
    std::shared_ptr<AuthoredGraph const> authored_graph_)
    : module_refs(std::move(refs)),
      root(root_),
      introspection(std::move(introspection_)),
      source_path(std::move(path)),
      module_id(std::move(id)),
      dependencies(std::move(deps)),
      authored_graph(std::move(authored_graph_))
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
