#define IV_INTERNAL_TRANSLATION_UNIT

#include <intravenous/module/loader.h>
#include <intravenous/compat.h>
#include <intravenous/graph/builder.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
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

    struct DynamicLibrary {
#if defined(_WIN32)
        HMODULE handle = nullptr;
#else
        void* handle = nullptr;
#endif
        explicit DynamicLibrary(std::filesystem::path const& path) {
#if defined(_WIN32)
            handle = LoadLibraryW(path.c_str());
            if (!handle) throw std::runtime_error("LoadLibraryW failed for '" + path.string() + "'");
#else
            handle = dlopen(path.c_str(), RTLD_NOW);
            if (!handle) throw std::runtime_error("dlopen failed for '" + path.string() + "': " + dlerror());
#endif
        }
        ~DynamicLibrary() {
#if defined(_WIN32)
            if (handle) FreeLibrary(handle);
#else
            if (handle) dlclose(handle);
#endif
        }
        void* symbol(char const* name) const {
#if defined(_WIN32)
            return reinterpret_cast<void*>(GetProcAddress(handle, name));
#else
            return dlsym(handle, name);
#endif
        }
    };

    struct LoadedBinary {
        std::string id;
        std::filesystem::path artifact_path;
        std::shared_ptr<DynamicLibrary> library;
        iv_module_descriptor_v1 const* descriptor = nullptr;
    };

    std::filesystem::path normalize(std::filesystem::path const& path) {
        std::error_code ec;
        auto p = std::filesystem::weakly_canonical(path, ec);
        return ec ? std::filesystem::absolute(path).lexically_normal() : p;
    }

    std::string read_text(std::filesystem::path const& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) throw std::runtime_error("failed to open '" + path.string() + "'");
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

    void write_text_if_different(std::filesystem::path const& path, std::string const& text) {
        if (std::filesystem::exists(path) && read_text(path) == text) return;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("failed to write '" + path.string() + "'");
        out << text;
    }

    Manifest parse_manifest(std::filesystem::path const& file) {
        std::ifstream in(file);
        if (!in) throw std::runtime_error("failed to open '" + file.string() + "'");

        Manifest m;
        try {
            auto const json = nlohmann::json::parse(in);
            m.schema = json.at("schema").get<int>();
            m.id = json.at("id").get<std::string>();
            m.entry = json.at("entry").get<std::string>();
            m.main = json.at("main").get<std::string>();
        } catch (nlohmann::json::exception const& e) {
            throw std::runtime_error("invalid module manifest '" + file.string() + "': " + e.what());
        }

        if (m.schema != 1) throw std::runtime_error("manifest '" + file.string() + "' uses unsupported schema " + std::to_string(m.schema));
        if (m.id.empty()) throw std::runtime_error("manifest '" + file.string() + "' has empty id");
        if (m.entry.empty() || m.entry.is_absolute()) throw std::runtime_error("manifest '" + file.string() + "' entry must be a relative path");
        if (m.main.empty()) throw std::runtime_error("manifest '" + file.string() + "' has empty main");
        return m;
    }

    std::filesystem::file_time_type directory_stamp(std::filesystem::path const& dir) {
        std::filesystem::file_time_type latest{};
        bool any = false;
        std::error_code ec;
        for (std::filesystem::recursive_directory_iterator it(dir, std::filesystem::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file() || !is_module_dependency_source_path(it->path())) continue;
            auto stamp = std::filesystem::last_write_time(it->path(), ec);
            if (!ec) { latest = any ? std::max(latest, stamp) : stamp; any = true; }
        }
        return any ? latest : std::filesystem::file_time_type{};
    }

    std::string sanitize(std::string_view s) {
        std::string out(s);
        for (char& c : out) if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
        return out.empty() ? "module" : out;
    }

    std::string stable_hash(std::filesystem::path const& path) {
        uint64_t h = 1469598103934665603ull;
        for (unsigned char c : normalize(path).generic_string()) { h ^= c; h *= 1099511628211ull; }
        std::ostringstream out; out << std::hex << h; return out.str();
    }

    std::string lib_name(std::string_view base) {
#if defined(_WIN32)
        return std::string(base) + ".dll";
#elif defined(__APPLE__)
        return "lib" + std::string(base) + ".dylib";
#else
        return "lib" + std::string(base) + ".so";
#endif
    }

    char const* config_name() {
#if defined(NDEBUG)
        return "Release";
#else
        return "Debug";
#endif
    }

    std::string q(std::filesystem::path const& p) {
        std::string s = p.generic_string(), out = "\"";
        for (char c : s) { if (c == '"') out += '\\'; out += c; }
        return out + "\"";
    }

    std::filesystem::path discover_repo(std::filesystem::path start) {
        start = normalize(start);
        if (std::filesystem::is_regular_file(start)) start = start.parent_path();
        for (auto p = start; !p.empty(); p = p.parent_path()) {
            if (std::filesystem::exists(p / "src/intravenous/dsl.h")) return p;
            if (p == p.root_path()) break;
        }
        throw std::runtime_error("failed to discover repo root from '" + start.string() + "'");
    }

    void run(std::string const& command, ModuleLoader::LogSink const& sink, std::string_view phase) {
        if (sink) sink("[" + std::string(phase) + "] " + command);
        int rc = std::system(command.c_str());
        if (rc != 0) throw std::runtime_error("command failed with exit code " + std::to_string(rc) + ": " + command);
    }
}

class ModuleLoader::Impl {
    std::filesystem::path repo_root;
    std::filesystem::path cache_root;
    ModuleLoaderToolchainConfig toolchain;
    LogSink log_sink;
    mutable std::mutex mutex;

    struct Registry {
        std::unordered_map<std::string, ResolvedModule> project;
        std::unordered_map<std::string, ResolvedModule> global;
        std::unordered_map<std::string, ResolvedModule> effective;
    };

    ResolvedModule resolve_dir(std::filesystem::path dir, bool global) const {
        dir = normalize(dir);
        auto manifest_file = dir / "iv_module.json";
        if (!std::filesystem::exists(manifest_file)) throw std::runtime_error("module directory '" + dir.string() + "' does not contain iv_module.json");
        auto manifest = parse_manifest(manifest_file);
        auto entry = normalize(dir / manifest.entry);
        if (!std::filesystem::exists(entry) || !std::filesystem::is_regular_file(entry)) throw std::runtime_error("manifest '" + manifest_file.string() + "' entry does not exist: " + manifest.entry.string());
        auto rel = entry.lexically_relative(dir);
        if (rel.empty() || rel.native().starts_with("..")) throw std::runtime_error("manifest entry escapes module directory: " + manifest.entry.string());
        return {manifest, dir, normalize(manifest_file), entry, global, directory_stamp(dir)};
    }

    void scan_root(std::filesystem::path const& root, bool global, std::unordered_map<std::string, ResolvedModule>& out) const {
        if (!std::filesystem::exists(root)) return;
        std::error_code ec;
        for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file() || it->path().filename() != "iv_module.json") continue;
            auto resolved = resolve_dir(it->path().parent_path(), global);
            auto [pos, inserted] = out.emplace(resolved.manifest.id, resolved);
            if (!inserted && pos->second.manifest_file != resolved.manifest_file) {
                throw std::runtime_error("duplicate module id '" + resolved.manifest.id + "' in '" + pos->second.manifest_file.string() + "' and '" + resolved.manifest_file.string() + "'");
            }
            it.disable_recursion_pending();
        }
    }

    Registry registry_for(ResolvedModule const& root) const {
        Registry r;
        // A root module belongs to the project containing its siblings. Extra
        // search roots are the shared/global registry.
        scan_root(root.module_dir.parent_path(), false, r.project);
        for (auto const& path : extra_search_roots) scan_root(path, true, r.global);
        r.effective = r.global;
        for (auto const& [id, module] : r.project) {
            if (r.global.contains(id) && log_sink) log_sink("warning: project module '" + id + "' shadows global module '" + r.global.at(id).manifest_file.string() + "'");
            r.effective[id] = module;
        }
        r.effective[root.manifest.id] = root;
        return r;
    }

    std::filesystem::path cmake_program() const {
        if (toolchain.cmake_program) return *toolchain.cmake_program;
        if (std::string_view(IV_CONFIGURED_CMAKE_COMMAND).size()) return IV_CONFIGURED_CMAKE_COMMAND;
        return "cmake";
    }

    std::pair<std::filesystem::path, std::filesystem::path> compilers() const {
        if (toolchain.c_compiler && toolchain.cxx_compiler) return {*toolchain.c_compiler, *toolchain.cxx_compiler};
        return {IV_CONFIGURED_C_COMPILER, IV_CONFIGURED_CXX_COMPILER};
    }

    std::filesystem::path workspace(ResolvedModule const& root) const {
        return cache_root / (sanitize(root.manifest.id) + "_" + stable_hash(root.module_dir)) / config_name();
    }

    static std::string generated_import_path(ResolvedModule const& m) {
        return std::string("iv/") + (m.global ? "modules-global/" : "modules/") + m.manifest.id;
    }

    std::filesystem::path build(ResolvedModule const& root, Registry const& registry) const {
        auto ws = workspace(root);
        auto source = ws / "src";
        auto build_dir = ws / "build";
        auto out_dir = ws / "out";
        auto gen_include = ws / "generated/include";
        auto export_file = ws / "generated/root_export.cpp";
        std::filesystem::create_directories(source);
        std::filesystem::create_directories(out_dir);

        std::vector<ResolvedModule> modules;
        modules.reserve(registry.effective.size());
        for (auto const& [_, m] : registry.effective) modules.push_back(m);
        std::sort(modules.begin(), modules.end(), [](auto const& a, auto const& b) { return a.manifest.id < b.manifest.id; });

        std::ostringstream cmake;
        cmake << "cmake_minimum_required(VERSION 3.20)\nproject(iv_runtime_module LANGUAGES CXX)\nset(CMAKE_EXPORT_COMPILE_COMMANDS ON)\n";
        cmake << "include(" << q(repo_root / "src/intravenous/module/template/ModuleSupport.cmake") << ")\n";
        cmake << "add_library(iv_phase2_settings INTERFACE)\ntarget_compile_features(iv_phase2_settings INTERFACE cxx_std_23)\n";
        cmake << "target_include_directories(iv_phase2_settings INTERFACE " << q(repo_root / "src") << " " << q(gen_include) << ")\n";
        cmake << "target_include_directories(iv_phase2_settings SYSTEM INTERFACE " << q(repo_root / "src/intravenous/third_party") << ")\n";
        cmake << "set(_iv_defs)\n";
        for (auto const& m : modules) {
            auto generated = gen_include / generated_import_path(m);
            auto target = "iv_rewrite_" + sanitize(m.manifest.id);
            cmake << "iv_rewrite_module_entry(TARGET " << target << " SOURCE " << q(m.entry_file) << " OUTPUT " << q(generated)
                  << " COMPILE_SETTINGS_TARGET iv_phase2_settings" << (m.global ? " GLOBAL_MODULE" : "") << ")\n";
            cmake << "list(APPEND _iv_defs " << q(generated) << ")\n";
        }
        cmake << "add_custom_target(iv_phase2_definitions DEPENDS ${_iv_defs})\n";
        cmake << "add_library(iv_runtime_module SHARED " << q(export_file) << ")\nadd_dependencies(iv_runtime_module iv_phase2_definitions)\n";
        cmake << "target_link_libraries(iv_runtime_module PRIVATE iv_phase2_settings)\n";
        cmake << "set_target_properties(iv_runtime_module PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES OUTPUT_NAME iv_module_" << sanitize(root.manifest.id)
              << " RUNTIME_OUTPUT_DIRECTORY " << q(out_dir) << " LIBRARY_OUTPUT_DIRECTORY " << q(out_dir) << ")\n";
        if (std::string_view(IV_CONFIGURED_IV_MODULE_SHARED_LIBRARY).size()) {
            cmake << "add_library(iv_module_shared SHARED IMPORTED GLOBAL)\nset_target_properties(iv_module_shared PROPERTIES IMPORTED_LOCATION " << q(IV_CONFIGURED_IV_MODULE_SHARED_LIBRARY)
                  << " INTERFACE_INCLUDE_DIRECTORIES " << q(repo_root / "src") << ")\ntarget_link_libraries(iv_runtime_module PRIVATE iv_module_shared)\n";
        }
        write_text_if_different(source / "CMakeLists.txt", cmake.str());

        auto root_generated = gen_include / generated_import_path(root);
        std::ostringstream export_tu;
        export_tu << "#include <intravenous/module/module.h>\n#include <" << generated_import_path(root) << ">\n";
        export_tu << "extern \"C\" IV_MODULE_EXPORT iv_module_descriptor_v1 const* iv_get_module_descriptor_v1() {\n";
        export_tu << "  static iv_module_descriptor_v1 const d{IV_MODULE_ABI_VERSION_V1, \"" << root.manifest.id << "\", &iv::details::generated_module_build_v1<&" << root.manifest.main << ">};\n  return &d;\n}\n";
        write_text_if_different(export_file, export_tu.str());

        std::ostringstream signature;
        signature << read_text(root.manifest_file) << '\n';
        for (auto const& m : modules) signature << m.manifest.id << '=' << m.source_stamp.time_since_epoch().count() << '\n';
        auto sig_file = ws / "build.signature";
        auto artifact = out_dir / lib_name("iv_module_" + sanitize(root.manifest.id));
        bool needs = !std::filesystem::exists(artifact) || !std::filesystem::exists(sig_file) || read_text(sig_file) != signature.str();

        auto [cc, cxx] = compilers();
        std::string generator = toolchain.cmake_generator.value_or(std::string(IV_CONFIGURED_CMAKE_GENERATOR));
        std::ostringstream configure;
        configure << q(cmake_program()) << " -S " << q(source) << " -B " << q(build_dir) << " -DCMAKE_BUILD_TYPE=" << config_name();
        if (!generator.empty()) configure << " -G " << q(generator);
        if (!cc.empty()) configure << " -DCMAKE_C_COMPILER=" << q(cc);
        if (!cxx.empty()) configure << " -DCMAKE_CXX_COMPILER=" << q(cxx);
        configure << " -DIV_INCLUDE_DIR=" << q(repo_root / "src") << " -DIV_SOURCE_DIR=" << q(repo_root / "src/intravenous")
                  << " -DIV_THIRD_PARTY_INCLUDE_DIR=" << q(repo_root / "src/intravenous/third_party");
        if (std::string_view(IV_CONFIGURED_CLANG_SOURCE_SPAN_REWRITER).size()) configure << " -DIV_SOURCE_SPAN_REWRITER=" << q(IV_CONFIGURED_CLANG_SOURCE_SPAN_REWRITER);

        if (needs || !std::filesystem::exists(build_dir / "CMakeCache.txt")) {
            run(configure.str(), log_sink, "configure");
            run(q(cmake_program()) + " --build " + q(build_dir) + " --config " + config_name(), log_sink, "build");
            write_text_if_different(sig_file, signature.str());
        }
        if (!std::filesystem::exists(artifact)) {
            auto config_artifact = out_dir / config_name() / artifact.filename();
            if (std::filesystem::exists(config_artifact)) artifact = config_artifact;
        }
        if (!std::filesystem::exists(artifact)) throw std::runtime_error("module build did not produce expected artifact '" + artifact.string() + "'");
        return artifact;
    }

public:
    std::vector<std::filesystem::path> extra_search_roots;

    Impl(std::filesystem::path discovery_start, std::vector<std::filesystem::path> roots, ModuleLoaderToolchainConfig tc, LogSink sink)
        : repo_root(discover_repo(std::move(discovery_start))), cache_root(repo_root / "build/iv_runtime_modules"), toolchain(std::move(tc)), log_sink(std::move(sink)) {
        std::filesystem::create_directories(cache_root);
        for (auto const& r : roots) extra_search_roots.push_back(normalize(r));
    }

    LoadedDefinition load_root_definition(std::filesystem::path const& path, ModuleExecutorTarget render_config, Sample* sample_period) const {
        std::lock_guard lock(mutex);
        auto p = normalize(path);
        if (std::filesystem::is_regular_file(p)) {
            if (p.filename() == "iv_module.json") p = p.parent_path();
            else throw std::runtime_error("root module path must be a module directory or iv_module.json");
        }
        auto root = resolve_dir(p, false);
        auto registry = registry_for(root);
        auto artifact = build(root, registry);
        auto library = std::make_shared<DynamicLibrary>(artifact);
        auto getter = reinterpret_cast<iv_get_module_descriptor_fn_v1>(library->symbol("iv_get_module_descriptor_v1"));
        if (!getter) throw std::runtime_error("module '" + artifact.string() + "' does not export iv_get_module_descriptor_v1");
        auto descriptor = getter();
        if (!descriptor || descriptor->abi_version != IV_MODULE_ABI_VERSION_V1 || !descriptor->build) throw std::runtime_error("module '" + artifact.string() + "' has invalid descriptor v1");
        if (!descriptor->id || root.manifest.id != descriptor->id) throw std::runtime_error("module '" + artifact.string() + "' exported unexpected id");

        auto binary = std::make_shared<LoadedBinary>(LoadedBinary{root.manifest.id, artifact, library, descriptor});
        GraphBuilder builder;
        ModuleContext context(builder, render_config, sample_period);
        if (char const* error = descriptor->build(context)) throw std::runtime_error("failed to build root module definition '" + root.manifest.id + "': " + error);
        auto introspection = builder.build_metadata();

        std::vector<ModuleDependency> dependencies;
        dependencies.reserve(registry.effective.size());
        for (auto const& [id, m] : registry.effective) dependencies.push_back({id, m.module_dir, m.entry_file, m.source_stamp});
        std::sort(dependencies.begin(), dependencies.end(), [](auto const& a, auto const& b) { return a.id < b.id; });
        std::vector<ModuleRef> refs{binary};
        return LoadedDefinition(std::move(refs), std::make_unique<GraphBuilder>(std::move(builder)), std::move(introspection), root.module_dir, root.manifest.id, std::move(dependencies));
    }
};

ModuleLoader::LoadedGraph::LoadedGraph(TypeErasedNode root_, std::vector<ModuleRef> refs, std::unique_ptr<GraphBuilder> builder, GraphIntrospectionMetadata introspection_, GraphBuildMetadata build_metadata, std::filesystem::path path, std::string id, std::vector<ModuleDependency> deps)
    : module_refs(std::move(refs)), root(std::move(root_)), canonical_builder(std::move(builder)), introspection(std::move(introspection_)), graph_build_metadata(std::move(build_metadata)), module_path(std::move(path)), module_id(std::move(id)), dependencies(std::move(deps)) {}
ModuleLoader::LoadedDefinition::LoadedDefinition(std::vector<ModuleRef> refs, std::unique_ptr<GraphBuilder> builder, GraphIntrospectionMetadata introspection_, std::filesystem::path path, std::string id, std::vector<ModuleDependency> deps)
    : module_refs(std::move(refs)), canonical_builder(std::move(builder)), introspection(std::move(introspection_)), module_path(std::move(path)), module_id(std::move(id)), dependencies(std::move(deps)) {}
ModuleLoader::ModuleLoader(std::filesystem::path start, std::vector<std::filesystem::path> roots, ModuleLoaderToolchainConfig tc, LogSink sink)
    : _impl(std::make_unique<Impl>(std::move(start), std::move(roots), std::move(tc), std::move(sink))) {}
ModuleLoader::~ModuleLoader() = default;
ModuleLoader::ModuleLoader(ModuleLoader&&) noexcept = default;
ModuleLoader& ModuleLoader::operator=(ModuleLoader&&) noexcept = default;
ModuleLoader::LoadedDefinition ModuleLoader::load_root_definition(std::filesystem::path const& path, ModuleExecutorTarget config, Sample* period) const { return _impl->load_root_definition(path, config, period); }
std::vector<std::filesystem::path> const& ModuleLoader::extra_search_roots() const { return _impl->extra_search_roots; }
}
