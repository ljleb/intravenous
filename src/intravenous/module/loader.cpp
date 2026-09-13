#include <intravenous/module/loader.h>
#include <intravenous/module/abi.h>
#include <intravenous/module/builder_session.h>
#include <intravenous/module/package_manifest.h>
#include <intravenous/module/package_definitions.h>
#include <intravenous/compat.h>
#include <intravenous/graph/builder.h>
#include <intravenous/graph/builder/lowering.hpp>
#include <intravenous/graph/compiler.h>
#include <intravenous/graph/node.h>

#include <nlohmann/json.hpp>

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalAlias.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iterator>
#include <mutex>
#include <ranges>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <atomic>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
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

std::optional<std::filesystem::path> find_package_manifest(
    std::filesystem::path const& directory)
{
    auto const package_manifest = directory / IV_PACKAGE_MANIFEST_FILE;
    return std::filesystem::exists(package_manifest)
        ? std::optional<std::filesystem::path>{package_manifest}
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

struct SharedPackageJit {
    std::mutex mutex{};
    std::unique_ptr<llvm::orc::LLJIT> jit{};
    std::atomic<std::uint64_t> next_package{0};
};

struct LoadedPackageCode {
    std::string id;
    std::string package_root;
    std::filesystem::path bitcode_path;
    std::vector<std::filesystem::path> dynamic_libraries;
    std::shared_ptr<SharedPackageJit> shared_jit;
    llvm::orc::JITDylib* jit_dylib = nullptr;
    llvm::orc::ResourceTrackerSP resources{};
    std::vector<details::PackageDefinition> definitions{};
    std::vector<NodeConfigPointerFieldData> config_pointer_fields{};
    std::vector<RetainedGlobalData> retained_globals{};
    std::vector<details::BuilderNodeStateStructure> node_state_structures{};
    ModuleDependency dependency{};

    ~LoadedPackageCode();
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

std::string describe_exception(std::exception_ptr exception)
{
    try {
        if (exception) std::rethrow_exception(exception);
    } catch (std::exception const& error) {
        return error.what();
    } catch (...) {
        return "unknown exception";
    }
    return "unknown exception";
}

std::string editor_compile_database(std::string const& build_database)
{
    // The package build uses a Clang plugin to emit server metadata. clangd
    // must parse the identical source/PCH command, but it must not execute a
    // compiler plugin with filesystem side effects for every editor reparse.
    // CMake emits these options as single `-fplugin=...` and
    // `-fplugin-arg-iv_module_metadata-...=...` command-line words. Preserve
    // every other flag, particularly `-std=c++23` and `-include-pch`.
    static std::regex const metadata_plugin_option(
        R"((?:^|\s)-fplugin(?:-arg-iv_module_metadata-[^=\s]+)?=(?:"[^"]*"|'[^']*'|[^\s]+))");
    auto database = nlohmann::json::parse(build_database);
    if (!database.is_array()) {
        throw std::runtime_error("package compilation database is not an array");
    }
    for (auto& command : database) {
        if (!command.is_object() || !command.contains("command")
            || !command["command"].is_string()) {
            throw std::runtime_error("package compilation database has an invalid command");
        }
        auto text = command["command"].get<std::string>();
        command["command"] = std::regex_replace(
            text, metadata_plugin_option, "");
    }
    return database.dump(2) + '\n';
}

std::vector<std::filesystem::path> package_dynamic_libraries(
    std::filesystem::path const& bitcode_path)
{
    auto manifest = bitcode_path;
    manifest += ".dynamic-libraries";
    if (!std::filesystem::exists(manifest)) return {};
    if (!std::filesystem::is_regular_file(manifest)) {
        throw std::runtime_error(
            "IV package dynamic-library manifest is not a regular file: '"
            + manifest.string() + "'");
    }

    std::vector<std::filesystem::path> result;
    std::unordered_set<std::string> seen;
    std::istringstream lines(read_text(manifest));
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        auto library = std::filesystem::path(line);
        if (library.is_relative()) library = manifest.parent_path() / library;
        library = normalize(library);
        if (!std::filesystem::is_regular_file(library)) {
            throw std::runtime_error(
                "IV package dynamic library listed by '" + manifest.string()
                + "' does not exist: '" + library.string() + "'");
        }
        if (seen.insert(library.generic_string()).second) {
            result.push_back(std::move(library));
        }
    }
    return result;
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
            if (is_module_dependency_ignored_directory(it->path())) {
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

std::string package_code_name(std::string_view base)
{
    return std::string(base) + ".ivpkg.bc";
}

char const *config_name()
{
#if defined(NDEBUG)
    return "Release";
#else
    return "Debug";
#endif
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

std::string llvm_error_string(llvm::Error error)
{
    std::string result;
    llvm::raw_string_ostream stream(result);
    llvm::logAllUnhandledErrors(std::move(error), stream);
    stream.flush();
    return result;
}

template<class T>
decltype(auto) take_llvm_expected(llvm::Expected<T> value, std::string_view context)
{
    if (!value) {
        throw std::runtime_error(
            std::string(context) + ": " + llvm_error_string(value.takeError()));
    }
    if constexpr (std::is_reference_v<T>) {
        return *value;
    } else {
        return std::move(*value);
    }
}

void check_llvm_error(llvm::Error error, std::string_view context)
{
    if (error) {
        throw std::runtime_error(
            std::string(context) + ": " + llvm_error_string(std::move(error)));
    }
}

void initialize_package_jit_target()
{
    static std::once_flag once;
    std::call_once(once, [] {
        if (llvm::InitializeNativeTarget()) {
            throw std::runtime_error("failed to initialize LLVM native target");
        }
        if (llvm::InitializeNativeTargetAsmPrinter()) {
            throw std::runtime_error("failed to initialize LLVM native asm printer");
        }
        if (llvm::InitializeNativeTargetAsmParser()) {
            throw std::runtime_error("failed to initialize LLVM native asm parser");
        }
    });
}

std::shared_ptr<SharedPackageJit> create_shared_package_jit()
{
    initialize_package_jit_target();
    auto target = take_llvm_expected(
        llvm::orc::JITTargetMachineBuilder::detectHost(),
        "detect package ORC target");
    target.setCodeGenOptLevel(llvm::CodeGenOptLevel::Aggressive);
    auto jit = take_llvm_expected(
        llvm::orc::LLJITBuilder()
            .setJITTargetMachineBuilder(std::move(target))
            .create(),
        "create package ORC JIT");
    auto result = std::make_shared<SharedPackageJit>();
    result->jit = std::move(jit);
    return result;
}

void mark_runtime_reachable(
    llvm::Value const* value,
    llvm::SmallPtrSetImpl<llvm::GlobalValue const*>& reachable)
{
    if (!value) return;
    if (auto const* global = llvm::dyn_cast<llvm::GlobalValue>(value)) {
        if (!reachable.insert(global).second) return;
        if (auto const* function = llvm::dyn_cast<llvm::Function>(global)) {
            if (!function->isDeclaration()) {
                for (auto const& block : *function) {
                    for (auto const& instruction : block) {
                        for (auto const& operand : instruction.operands()) {
                            mark_runtime_reachable(operand.get(), reachable);
                        }
                    }
                }
            }
        } else if (auto const* variable = llvm::dyn_cast<llvm::GlobalVariable>(global)) {
            if (variable->hasInitializer()) {
                mark_runtime_reachable(variable->getInitializer(), reachable);
            }
        } else if (auto const* alias = llvm::dyn_cast<llvm::GlobalAlias>(global)) {
            mark_runtime_reachable(alias->getAliasee(), reachable);
        }
        return;
    }
    if (auto const* constant = llvm::dyn_cast<llvm::Constant>(value)) {
        for (auto const& operand : constant->operands()) {
            mark_runtime_reachable(operand.get(), reachable);
        }
    }
}

void collect_compatibility_runtime_code(
    llvm::Module const& module,
    llvm::SmallPtrSetImpl<llvm::GlobalValue const*>& reachable)
{
    for (auto const& global : module.globals()) {
        auto const section = global.getSection();
        if (section != "iv_node_types" && !section.ends_with("__iv_node_types")) {
            continue;
        }
        auto const* record = llvm::dyn_cast_or_null<llvm::ConstantStruct>(
            global.getInitializer());
        if (!record || record->getNumOperands() != 6) continue;
        auto const* operations = llvm::dyn_cast<llvm::ConstantStruct>(
            record->getOperand(1));
        if (!operations || operations->getNumOperands() != 3) continue;

        // declare_node participates in graph construction and is intentionally
        // left at package O0.  Package/provider configuration is allowed to
        // throw across the JIT boundary, and optimizing that path caused those
        // exceptions to terminate instead of reaching their existing handlers.
        // Only tick/skip are realtime compatibility-runtime code.
        mark_runtime_reachable(operations->getOperand(1), reachable);
        mark_runtime_reachable(operations->getOperand(2), reachable);
    }
}

void optimize_package_for_compatibility_runtime(llvm::Module& module)
{
    // Package artifacts intentionally stop at Clang O0 so source rebuilds stay
    // cheap and the future whole-graph compiler receives the unoptimized IR.
    // The current reflected executor only needs native-quality node tick/skip
    // callbacks. Keep package/module graph-construction code at its original O0
    // semantics: it is allowed to throw through the JIT boundary and some
    // providers deliberately catch configuration failures inside package code.
    llvm::SmallPtrSet<llvm::GlobalValue const*, 32> runtime_code;
    collect_compatibility_runtime_code(module, runtime_code);
    for (auto const* value : runtime_code) {
        auto* function = llvm::dyn_cast<llvm::Function>(
            const_cast<llvm::GlobalValue*>(value));
        if (!function || !function->hasFnAttribute(llvm::Attribute::OptimizeNone)) {
            continue;
        }
        function->removeFnAttr(llvm::Attribute::OptimizeNone);
        function->removeFnAttr(llvm::Attribute::NoInline);
    }

    llvm::PassBuilder pass_builder;
    llvm::LoopAnalysisManager loops;
    llvm::FunctionAnalysisManager functions;
    llvm::CGSCCAnalysisManager cgscc;
    llvm::ModuleAnalysisManager modules;
    pass_builder.registerModuleAnalyses(modules);
    pass_builder.registerCGSCCAnalyses(cgscc);
    pass_builder.registerFunctionAnalyses(functions);
    pass_builder.registerLoopAnalyses(loops);
    pass_builder.crossRegisterProxies(loops, functions, cgscc, modules);
    auto pipeline = pass_builder.buildPerModuleDefaultPipeline(
        llvm::OptimizationLevel::O3);
    pipeline.run(module, modules);
}

void apply_configured_dsl_pch(ModuleLoaderToolchainConfig& toolchain)
{
    // The PCH is an application build product, not something an individual IV
    // package is permitted to generate. An explicit toolchain path takes
    // precedence; otherwise every direct ModuleLoader user receives this
    // build's fixed DSL PCH.
    if (!toolchain.iv_package_pch.has_value()
        || toolchain.iv_package_pch->empty()) {
        toolchain.iv_package_pch.reset();
#if defined(IV_CONFIGURED_IV_DSL_PCH)
        auto const configured_path = std::string_view(IV_CONFIGURED_IV_DSL_PCH);
        if (!configured_path.empty()) {
            toolchain.iv_package_pch = std::filesystem::path(configured_path);
        }
#endif
    }
    if (!toolchain.iv_package_pch.has_value()) {
        throw std::runtime_error(
            "IV package compilation requires an application-built DSL PCH");
    }
    if (!std::filesystem::is_regular_file(*toolchain.iv_package_pch)) {
        throw std::runtime_error(
            "configured IV DSL PCH does not exist: '"
            + toolchain.iv_package_pch->string() + "'");
    }
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
LoadedPackageCode::~LoadedPackageCode()
{
    if (!shared_jit || !shared_jit->jit || !jit_dylib) return;
    std::lock_guard lock(shared_jit->mutex);
    if (auto error = shared_jit->jit->deinitialize(*jit_dylib)) {
        llvm::consumeError(std::move(error));
    }
    if (resources) {
        if (auto error = resources->remove()) {
            llvm::consumeError(std::move(error));
        }
    }
    if (auto error = shared_jit->jit->getExecutionSession().removeJITDylib(*jit_dylib)) {
        llvm::consumeError(std::move(error));
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
        std::filesystem::path artifact;
    };

    std::shared_ptr<SharedPackageJit> package_jit_;
    mutable std::unordered_map<std::string, std::weak_ptr<LoadedPackageCode>>
        loaded_packages_by_bitcode_;
    // Last package revision that completed graph configuration successfully.
    // Broken unrelated rebuilds continue to use this code until a replacement
    // has itself participated in a successful configuration transaction.
    mutable std::unordered_map<std::string, std::shared_ptr<LoadedPackageCode>>
        current_packages_by_root_;

    static std::string key(ResolvedPackage const &module)
    {
        return std::string(module.global ? "global:" : "project:")
            + normalize(module.module_dir).generic_string();
    }

    ResolvedPackage resolve_dir(std::filesystem::path dir, bool global) const
    {
        dir = normalize(dir);
        auto manifest_file = find_package_manifest(dir);
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
        auto const default_package_dir = generated_dir / "default-project";
        auto const custom_cmake = root.module_dir / "CMakeLists.txt";
        auto const package_dir = std::filesystem::exists(custom_cmake)
            ? root.module_dir
            : default_package_dir;

        std::filesystem::create_directories(workspace);
        ScopedModuleBuildLock const build_lock(workspace / "build.lock");
        std::filesystem::create_directories(output_dir);
        std::filesystem::create_directories(generated_dir);


        if (!std::filesystem::exists(custom_cmake)) {
            std::filesystem::create_directories(default_package_dir);
            write_text_if_different(
                default_package_dir / "CMakeLists.txt",
                "cmake_minimum_required(VERSION 3.21)\n"
                "project(iv_runtime_module LANGUAGES CXX)\n"
                "set(CMAKE_EXPORT_COMPILE_COMMANDS ON)\n"
                "include(${IV_SOURCE_DIR}/module/template/ModuleSupport.cmake)\n"
                "iv_add_package(iv_package)\n");
        }

        auto const [cc, cxx] = compilers();
        auto const source_introspection_plugin =
            std::filesystem::path(IV_CONFIGURED_CLANG_SOURCE_INTROSPECTION_PLUGIN);
        auto const module_finalizer =
            std::filesystem::path(IV_CONFIGURED_IV_PACKAGE_FINALIZER);
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
        signature << "iv-package-abi=" << IV_PACKAGE_ABI_VERSION << '\n'
                  << "config=" << config_name() << '\n'
                  << "cmake=" << cmake_program().generic_string() << '\n'
                  << "cc=" << cc.generic_string() << '\n'
                  << "cxx=" << cxx.generic_string() << '\n'
                  << "generator=" << generator << '\n'
                  << "source-introspection="
                  << toolchain_.source_introspection << '\n'
                  << "dsl-pch="
                  << toolchain_.iv_package_pch->generic_string() << '\n'
                  << "dsl-pch-stamp="
                  << std::filesystem::last_write_time(*toolchain_.iv_package_pch)
                         .time_since_epoch().count() << '\n'
                  << "clang-time-trace="
                  << toolchain_.clang_time_trace << '\n'
                  << "core-source-stamp="
                  << directory_stamp(repo_root_ / "src/intravenous")
                         .time_since_epoch().count() << '\n'
                  << read_text(repo_root_ / "src/intravenous/module/abi.h") << '\n'
                  << read_text(repo_root_ / "src/intravenous/module/builder_session.h") << '\n'
                  << read_text(repo_root_ / "src/intravenous/module/template/ModuleSupport.cmake") << '\n';
        signature
            << "package-finalizer=" << module_finalizer.generic_string() << '\n'
            << "package-finalizer-stamp="
            << std::filesystem::last_write_time(module_finalizer)
                   .time_since_epoch().count() << '\n';
        signature
            << "source-introspection-plugin="
            << source_introspection_plugin.generic_string() << '\n'
            << "source-introspection-plugin-stamp="
            << std::filesystem::last_write_time(source_introspection_plugin)
                   .time_since_epoch().count() << '\n';
        // An IV package compile owns only its own implementation files. Package
        // definition IDs resolve through graph configuration, so provider edits do
        // not enter a consumer's C++ compilation signature.
        signature << key(root) << '\n'
                  << read_text(root.manifest_file) << '\n'
                  << root.package_stamp.time_since_epoch().count() << '\n';
        if (std::filesystem::exists(custom_cmake)) {
            signature << read_text(custom_cmake) << '\n';
        }
        auto const signature_file = workspace / "build.signature";
        auto const artifact_stem = "iv_package_" + sanitize(root.package_key)
            + "_" + stable_text_hash(signature.str());
        auto const artifact_name = package_code_name(artifact_stem);
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
                  << " -S " << quote(package_dir)
                  << " -B " << quote(build_dir)
                  << " -DCMAKE_BUILD_TYPE=" << config_name();
        if (!generator.empty()) configure << " -G " << quote(generator);
        if (!cc.empty()) configure << " -DCMAKE_C_COMPILER=" << quote(cc);
        if (!cxx.empty()) configure << " -DCMAKE_CXX_COMPILER=" << quote(cxx);
        configure << " -DIV_INCLUDE_DIR=" << quote(repo_root_ / "src")
                  << " -DIV_SOURCE_DIR=" << quote(repo_root_ / "src/intravenous")
                  << " -DIV_THIRD_PARTY_INCLUDE_DIR=" << quote(repo_root_ / "src/intravenous/third_party")
                  << " -DIV_PACKAGE_DIR=" << quote(root.module_dir)
                  << " -DIV_PACKAGE_ENTRY_FILE=" << quote(root.entry_file)
                  << " -DIV_PACKAGE_INCLUDE_DIRS=\"" << include_list.str() << "\""
                  << " -DIV_PACKAGE_SOURCE_FILES=\"" << source_list.str() << "\""
                  << " -DIV_PACKAGE_OUTPUT_DIR=" << quote(output_dir)
                  << " -DIV_PACKAGE_OUTPUT_NAME=" << artifact_stem
                  << " -DIV_CLANG_SOURCE_INTROSPECTION_PLUGIN="
                  << quote(source_introspection_plugin)
                  << " -DIV_PACKAGE_FINALIZER=" << quote(module_finalizer);
        if (!toolchain_.source_introspection) {
            configure << " -DIV_PACKAGE_SOURCE_INTROSPECTION=OFF";
        }
        configure << " -DIV_DSL_PCH="
                  << quote(*toolchain_.iv_package_pch);
        if (toolchain_.clang_time_trace) {
            configure << " -DIV_PACKAGE_CLANG_TIME_TRACE=ON";
        }
        if (std::string_view(IV_CONFIGURED_IV_BUILDER_LIBRARY).size()) {
            configure << " -DIV_BUILDER_LIBRARY=" << quote(IV_CONFIGURED_IV_BUILDER_LIBRARY);
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
                    editor_compile_database(database_text)
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
                "IV package build did not produce expected LLVM '" + artifact.string() + "'");
        }

        // The signature is part of the filename. A changed IV package produces new
        // LLVM while configured graphs keep the previous ORC resources alive
        // through ModuleRef ownership.
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
          log_sink_(std::move(sink)),
          package_jit_(create_shared_package_jit())
    {
        apply_configured_dsl_pch(toolchain_);
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
                    "root package path must be an IV package directory, " +
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
        auto artifact = build(root, project_root);
        return {
            .root = std::move(root),
            .artifact = std::move(artifact),
        };
    }

    std::filesystem::path compile_package(
        std::filesystem::path const& path) const
    {
        std::lock_guard lock(mutex_);
        return compile_package_unlocked(path).artifact;
    }

    void set_toolchain_config(ModuleLoaderToolchainConfig toolchain)
    {
        std::lock_guard lock(mutex_);
        apply_configured_dsl_pch(toolchain);
        toolchain_ = std::move(toolchain);
    }

    ModuleLoader::LoadedPackage load_compiled_package(
        CompiledPackage const& compiled) const
    {
        auto const& root = compiled.root;
        auto const& bitcode_path = compiled.artifact;
        auto const bitcode_key = normalize(bitcode_path).generic_string();

        std::shared_ptr<LoadedPackageCode> package;
        if (auto existing = loaded_packages_by_bitcode_.find(bitcode_key);
            existing != loaded_packages_by_bitcode_.end()) {
            package = existing->second.lock();
        }

        if (!package) {
            auto const load_started_at = std::chrono::steady_clock::now();
            package = std::make_shared<LoadedPackageCode>();
            package->id = root.package_key;
            package->package_root = root.module_dir.generic_string();
            package->bitcode_path = bitcode_path;
            package->dynamic_libraries = package_dynamic_libraries(bitcode_path);
            package->shared_jit = package_jit_;
            package->dependency = {
                root.package_key,
                root.module_dir,
                root.entry_file,
                root.package_stamp,
            };

            std::lock_guard jit_lock(package_jit_->mutex);
            auto& jit = *package_jit_->jit;
            auto buffer = llvm::MemoryBuffer::getFile(bitcode_path.string());
            if (!buffer) {
                throw std::runtime_error(
                    "cannot read finalized IV package LLVM '" + bitcode_path.string()
                    + "': " + buffer.getError().message());
            }
            auto context = std::make_unique<llvm::LLVMContext>();
            llvm::orc::ThreadSafeContext thread_safe_context(std::move(context));
            auto module = thread_safe_context.withContextDo(
                [&](llvm::LLVMContext* context) {
                    if (!context) {
                        throw std::logic_error("IV package has no LLVM context");
                    }
                    return take_llvm_expected(
                        llvm::parseBitcodeFile(
                            (*buffer)->getMemBufferRef(), *context),
                        "parse finalized IV package LLVM");
                });
            auto const optimize_started_at = std::chrono::steady_clock::now();
            optimize_package_for_compatibility_runtime(*module);
            if (log_sink_) {
                log_sink_(
                    "[package-orc-optimize] elapsed_us="
                    + std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - optimize_started_at).count()));
            }

            auto const suffix = package_jit_->next_package.fetch_add(
                1, std::memory_order_relaxed);
            // Use LLJIT's wrapper rather than ExecutionSession::createJITDylib.
            // LLJIT adds its platform/process JITDylibs to the default link
            // order; package static constructors depend on that platform support.
            auto& jit_dylib = take_llvm_expected(
                jit.createJITDylib(
                    "iv.package." + sanitize(root.package_key) + "."
                    + std::to_string(suffix)),
                "create IV package JITDylib");
            package->jit_dylib = &jit_dylib;
            package->resources = jit_dylib.createResourceTracker();
            auto const global_prefix = jit.getDataLayout().getGlobalPrefix();
            if (std::string_view(IV_CONFIGURED_IV_BUILDER_LIBRARY).size()) {
                jit_dylib.addGenerator(take_llvm_expected(
                    llvm::orc::DynamicLibrarySearchGenerator::Load(
                        IV_CONFIGURED_IV_BUILDER_LIBRARY, global_prefix),
                    "load iv_builder package symbol resolver"));
            }
            for (auto const& library : package->dynamic_libraries) {
                auto const library_path = library.string();
                jit_dylib.addGenerator(take_llvm_expected(
                    llvm::orc::DynamicLibrarySearchGenerator::Load(
                        library_path.c_str(), global_prefix),
                    "load IV package dynamic library '" + library_path + "'"));
            }
            check_llvm_error(
                jit.addIRModule(
                    package->resources,
                    llvm::orc::ThreadSafeModule(
                        std::move(module), std::move(thread_safe_context))),
                "add IV package LLVM to shared ORC JIT");
            check_llvm_error(jit.initialize(jit_dylib), "initialize IV package LLVM");

            auto symbol = [&]<class Function>(char const* name) -> Function {
                auto address = take_llvm_expected(
                    jit.lookup(jit_dylib, name),
                    std::string("lookup IV package symbol '") + name + "'");
                return address.template toPtr<Function>();
            };
            auto const abi_version = symbol.template operator()<iv_package_abi_version_fn>(
                "iv_package_abi_version");
            if (abi_version() != IV_PACKAGE_ABI_VERSION) {
                throw std::runtime_error(
                    "IV package LLVM '" + bitcode_path.string()
                    + "' has incompatible ABI version "
                    + std::to_string(abi_version()) + " (expected "
                    + std::to_string(IV_PACKAGE_ABI_VERSION) + ")");
            }

            auto const definitions_fn =
                symbol.template operator()<iv_package_definitions_fn>(
                    "iv_package_definitions");
            auto const pointer_fields_fn =
                symbol.template operator()<iv_package_node_config_pointer_fields_fn>(
                    "iv_package_node_config_pointer_fields");
            auto const retained_globals_fn =
                symbol.template operator()<iv_package_retained_globals_fn>(
                    "iv_package_retained_globals");
            auto const node_state_structures_fn =
                symbol.template operator()<iv_package_node_state_structures_fn>(
                    "iv_package_node_state_structures");

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
            package->definitions = copy_table(
                definitions_fn(),
                static_cast<details::PackageDefinition*>(nullptr),
                "definition");
            package->config_pointer_fields = copy_table(
                pointer_fields_fn(),
                static_cast<NodeConfigPointerFieldData*>(nullptr),
                "node-config pointer-field");
            package->retained_globals = copy_table(
                retained_globals_fn(),
                static_cast<RetainedGlobalData*>(nullptr),
                "retained LLVM global");
            auto const state_structures = copy_table(
                node_state_structures_fn(),
                static_cast<NodeStateStructureData*>(nullptr),
                "node-state structure");

            auto copy_text = [](ModuleDataView view, std::string_view what) {
                if (!view.data && view.size != 0) {
                    throw std::runtime_error(
                        "IV package " + std::string(what) + " has null data");
                }
                return std::string(
                    static_cast<char const*>(view.data), view.size);
            };
            package->node_state_structures.reserve(state_structures.size());
            for (auto const& state : state_structures) {
                if (!state.fields.data && state.fields.size != 0) {
                    throw std::runtime_error(
                        "IV package node-state field table has null data");
                }
                if (state.fields.size % sizeof(NodeStateFieldData) != 0) {
                    throw std::runtime_error(
                        "IV package node-state field table has invalid size");
                }
                auto const fields = std::span(
                    static_cast<NodeStateFieldData const*>(state.fields.data),
                    state.fields.size / sizeof(NodeStateFieldData));
                NodeStateStructure structure{
                    .size_bits = state.size_bits,
                    .alignment_bits = state.alignment_bits,
                };
                structure.fields.reserve(fields.size());
                for (auto const& field : fields) {
                    structure.fields.push_back({
                        .name = copy_text(field.name, "node-state field name"),
                        .type_name = copy_text(
                            field.type_name, "node-state field type name"),
                        .bit_offset = field.bit_offset,
                        .size_bits = field.size_bits,
                        .alignment_bits = field.alignment_bits,
                        .bit_width = field.has_bit_width
                            ? std::optional<std::size_t>{field.bit_width}
                            : std::nullopt,
                    });
                }
                package->node_state_structures.push_back({
                    .code_key = state.code_key,
                    .structure = std::move(structure),
                });
            }

            // Package ownership comes from the loader's resolved manifest, not
            // a per-package compiler definition. This keeps package code—and
            // therefore its PCH compile environment—independent of its source
            // directory. BuilderPackageView still carries the resolved root
            // for symbolic config relocations and provider selection.
            for (auto& definition : package->definitions) {
                definition.package_root = package->package_root.c_str();
                definition.package_root_size = package->package_root.size();
            }

            loaded_packages_by_bitcode_.insert_or_assign(bitcode_key, package);
            if (log_sink_) {
                log_sink_(
                    "[package-orc-load] elapsed_us="
                    + std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - load_started_at).count()));
            }
        }

        std::vector<LoadedNodeType> loaded_node_types;
        std::unordered_set<std::string> node_type_ids;
        for (auto const& definition : package->definitions) {
            if (definition.kind != details::PackageDefinitionKind::node) continue;
            if (!definition.id || definition.id_size == 0
                || !definition.node_build || !definition.node_compiler_record
                || !definition.signature) {
                throw std::runtime_error("IV package node type definition is incomplete");
            }
            auto node_type_id = std::string(definition.id, definition.id_size);
            if (!node_type_ids.insert(node_type_id).second) {
                throw std::runtime_error(
                    "IV package contains duplicate node type ID '" + node_type_id + "'");
            }
            auto const* compiler_record = static_cast<details::NodeCompilerRecord const*>(
                definition.node_compiler_record);
            if (!compiler_record->operations.valid()) {
                throw std::runtime_error(
                    "IV package node type '" + node_type_id
                    + "' has invalid compiler operations");
            }

            auto const* signature = definition.signature();
            if (!signature) {
                throw std::runtime_error(
                    "IV package node type definition has no construction signature");
            }
            details::validate_registered_signature_shape(node_type_id, *signature);
            loaded_node_types.push_back({
                .node_type_id = std::move(node_type_id),
                .compiler_record = *compiler_record,
                .package_path = root.module_dir,
                .module_refs = {package},
            });
        }

        std::vector<ModuleDependency> dependencies{{
            root.package_key,
            root.module_dir,
            root.entry_file,
            root.package_stamp,
        }};
        return {
            .definitions = {},
            .node_types = std::move(loaded_node_types),
            .dependencies = std::move(dependencies),
            .package_code = package,
        };
    }

    ModuleLoader::LoadedPackage configure_iv_modules(
        CompiledPackage const& compiled,
        ModuleLoader::LoadedPackage const& loaded_package,
        std::vector<std::shared_ptr<LoadedPackageCode>> const& loaded_packages) const
    {
        auto package_result = loaded_package;
        auto const root_package = std::static_pointer_cast<LoadedPackageCode>(
            package_result.package_code);
        if (!root_package || !root_package->jit_dylib) {
            throw std::logic_error("loaded IV package has no executable LLVM");
        }

        std::vector<details::BuilderPackageView> package_views;
        package_views.reserve(loaded_packages.size());
        for (auto const& package : loaded_packages) {
            if (!package || !package->jit_dylib) continue;
            package_views.push_back({
                .package_root = package->package_root,
                .definitions = package->definitions,
                .config_pointer_fields = package->config_pointer_fields,
                .retained_globals = package->retained_globals,
                .node_state_structures = package->node_state_structures,
            });
        }
        if (package_views.size() != loaded_packages.size()) {
            throw std::logic_error("loaded IV package table is incomplete");
        }

        auto const started_at = std::chrono::steady_clock::now();
        std::vector<LoadedDefinition> definitions;
        std::unordered_set<std::string> module_ids;
        for (auto const& definition : root_package->definitions) {
            if (definition.kind != details::PackageDefinitionKind::module) continue;
            if (!definition.id || definition.id_size == 0 || !definition.module_build
                || !definition.signature) {
                throw std::runtime_error("IV module definition is incomplete");
            }
            auto module_id = std::string(definition.id, definition.id_size);
            if (!module_ids.insert(module_id).second) {
                throw std::runtime_error(
                    "IV package contains duplicate iv module ID '" + module_id + "'");
            }

            auto const* signature = definition.signature();
            if (!signature) {
                throw std::runtime_error(
                    "IV module definition has no construction signature");
            }
            details::validate_registered_signature_shape(module_id, *signature);
            // The compatibility instance runtime has no persisted module
            // construction arguments yet. Publish a required-argument module
            // nonetheless: it is a valid provider for another module's
            // g.node<Id>(...) call, but has no default execution root for a
            // project instance to realize.
            if (signature->required_argument_count != 0) {
                definitions.emplace_back(
                    std::vector<ModuleRef>{root_package},
                    WeakTypeErasedNode{},
                    GraphIntrospectionMetadata{},
                    compiled.root.module_dir,
                    std::move(module_id),
                    package_result.dependencies,
                    nullptr);
                continue;
            }

            auto session = std::unique_ptr<details::BuilderSession,
                decltype(&details::iv_builder_session_destroy)>(
                    details::iv_builder_session_create(),
                    details::iv_builder_session_destroy);
            if (!session) {
                throw std::runtime_error("could not create graph configuration session");
            }
            details::set_builder_packages(session.get(), package_views);
            auto const root_package_index = details::builder_package_index(
                session.get(), root_package->package_root);
            details::select_builder_package(session.get(), root_package_index);
            details::begin_builder_module(session.get(), module_id);
            struct ModuleCallScope {
                details::BuilderSession* session = nullptr;
                ~ModuleCallScope() { details::end_builder_module(session); }
            } const module_call{session.get()};

            GraphBuilder builder(session.get());
            details::validate_registered_signature(module_id, *signature, {});
            definition.module_build(builder, {});
            auto configured = std::make_shared<ConfiguredGraph const>(
                details::take_built_graph(session.get()));
            auto plan = GraphCompiler::compile(
                GraphLowerer::lower(*configured, {.execution_root = true}));
            auto runtime_root = std::make_shared<RuntimeGraphRoot>(std::move(plan.graph));

            auto const used_package_indexes = details::builder_used_packages(session.get());
            std::vector<ModuleRef> refs;
            refs.reserve(used_package_indexes.size() + 1);
            for (auto const package_index : used_package_indexes) {
                if (package_index >= loaded_packages.size()) {
                    throw std::logic_error("configured graph references an invalid IV package");
                }
                // Runtime code/data ownership follows every package actually used
                // while configuring this graph. The same package set is retained
                // below as the graph's reload dependency set: providers compile
                // independently, but a provider revision requires every caller
                // that configured against it to be rebuilt.
                refs.push_back(loaded_packages[package_index]);
            }
            refs.push_back(runtime_root);

            std::vector<ModuleDependency> dependencies;
            dependencies.reserve(used_package_indexes.size());
            std::unordered_set<std::string> dependency_roots;
            for (auto const package_index : used_package_indexes) {
                auto const& dependency = loaded_packages[package_index]->dependency;
                auto const dependency_root = normalize(dependency.module_dir).generic_string();
                if (dependency_roots.insert(dependency_root).second) {
                    dependencies.push_back(dependency);
                }
            }

            definitions.emplace_back(
                std::move(refs),
                WeakTypeErasedNode(*runtime_root),
                std::move(plan.introspection),
                compiled.root.module_dir,
                std::move(module_id),
                std::move(dependencies),
                std::move(configured));
        }
        if (log_sink_) {
            log_sink_(
                "[graph-configuration] elapsed_us="
                + std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - started_at).count()));
        }
        package_result.definitions = std::move(definitions);
        // A node-only package has no configured module from which to infer its
        // own watch root. Every successful package must at least watch itself.
        if (package_result.dependencies.empty()) {
            package_result.dependencies.push_back(root_package->dependency);
        } else {
            std::unordered_set<std::string> dependency_roots;
            std::vector<ModuleDependency> dependencies;
            for (auto const& definition : package_result.definitions) {
                for (auto const& dependency : definition.dependencies) {
                    auto const dependency_root = normalize(dependency.module_dir).generic_string();
                    if (dependency_roots.insert(dependency_root).second) {
                        dependencies.push_back(dependency);
                    }
                }
            }
            if (dependencies.empty()) dependencies.push_back(root_package->dependency);
            package_result.dependencies = std::move(dependencies);
        }
        return package_result;
    }

    std::vector<ModuleLoader::PackageLoadResult> load_packages(
        std::vector<std::filesystem::path> const& paths) const
    {
        std::lock_guard lock(mutex_);
        std::vector<ModuleLoader::PackageLoadResult> results(paths.size());

        struct Candidate {
            std::size_t result_index = 0;
            std::string root_key;
            CompiledPackage compiled;
            ModuleLoader::LoadedPackage loaded;
            std::shared_ptr<LoadedPackageCode> code;
            std::optional<ModuleLoader::LoadedPackage> configured;
            bool surviving = true;
        };
        std::vector<Candidate> candidates;
        candidates.reserve(paths.size());
        std::unordered_set<std::string> requested_roots;

        for (std::size_t index = 0; index < paths.size(); ++index) {
            results[index].package_path = paths[index];
            try {
                auto compiled = compile_package_unlocked(paths[index]);
                auto const root_key = normalize(compiled.root.module_dir).generic_string();
                results[index].package_path = compiled.root.module_dir;
                if (!requested_roots.insert(root_key).second) {
                    throw std::runtime_error(
                        "IV package was requested more than once in one reload batch: '"
                        + root_key + "'");
                }
                auto loaded = load_compiled_package(compiled);
                auto code = std::static_pointer_cast<LoadedPackageCode>(loaded.package_code);
                if (!code) {
                    throw std::logic_error("loaded IV package omitted its code ownership");
                }
                candidates.push_back({
                    .result_index = index,
                    .root_key = std::move(root_key),
                    .compiled = std::move(compiled),
                    .loaded = std::move(loaded),
                    .code = std::move(code),
                    .configured = {},
                });
            } catch (...) {
                results[index].error = describe_exception(std::current_exception());
            }
        }

        // A configuration sees last-valid package code, shadowed by every
        // candidate that is still viable in this transaction. Re-running after
        // a rejected candidate prevents consumers from silently publishing a
        // graph against code that cannot itself become the active revision.
        auto available_packages = [&] {
            std::unordered_map<std::string, std::shared_ptr<LoadedPackageCode>> by_root =
                current_packages_by_root_;
            for (auto const& candidate : candidates) {
                if (candidate.surviving) {
                    by_root.insert_or_assign(candidate.root_key, candidate.code);
                }
            }
            std::vector<std::shared_ptr<LoadedPackageCode>> packages;
            packages.reserve(by_root.size());
            for (auto const& [_, package] : by_root) packages.push_back(package);
            std::ranges::sort(packages, {}, [](auto const& package) {
                return package->package_root;
            });
            return packages;
        };

        bool rejected_candidate = false;
        do {
            rejected_candidate = false;
            auto const packages = available_packages();
            for (auto& candidate : candidates) {
                if (!candidate.surviving) continue;
                try {
                    candidate.configured = configure_iv_modules(
                        candidate.compiled, candidate.loaded, packages);
                } catch (...) {
                    candidate.surviving = false;
                    candidate.configured.reset();
                    results[candidate.result_index].error =
                        describe_exception(std::current_exception());
                    rejected_candidate = true;
                }
            }
        } while (rejected_candidate);

        for (auto& candidate : candidates) {
            if (!candidate.surviving) continue;
            current_packages_by_root_.insert_or_assign(candidate.root_key, candidate.code);
            results[candidate.result_index].package = std::move(candidate.configured);
        }
        return results;
    }

    ModuleLoader::LoadedPackage load_package(
        std::filesystem::path const& path) const
    {
        auto results = load_packages({path});
        if (results.empty() || !results.front()) {
            throw std::runtime_error(
                results.empty()
                    ? "IV package load produced no result"
                    : results.front().error);
        }
        return std::move(*results.front().package);
    }

    void remove_package(std::filesystem::path const& path) const
    {
        std::lock_guard lock(mutex_);
        current_packages_by_root_.erase(normalize(path).generic_string());
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

std::vector<ModuleLoader::PackageLoadResult> ModuleLoader::load_packages(
    std::vector<std::filesystem::path> const& paths) const
{
    return _impl->load_packages(paths);
}

void ModuleLoader::remove_package(std::filesystem::path const& path) const
{
    _impl->remove_package(path);
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

void ModuleLoader::set_toolchain_config(ModuleLoaderToolchainConfig toolchain)
{
    _impl->set_toolchain_config(std::move(toolchain));
}

std::vector<std::filesystem::path> const &ModuleLoader::extra_search_roots() const
{
    return _impl->extra_search_roots;
}
} // namespace iv
