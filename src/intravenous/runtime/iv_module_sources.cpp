#include <intravenous/runtime/iv_module_sources.h>

#include <intravenous/module/source_manifest.h>
#include <intravenous/runtime/iv_module_definitions.h>
#include <intravenous/runtime/iv_module_sources_events.h>
#include <intravenous/runtime/socket_rpc_server.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace iv {
namespace {
struct SourceManifest {
    std::filesystem::path entry;
};

std::optional<SourceManifest> read_manifest(std::filesystem::path const& path)
{
    std::ifstream in(path);
    if (!in) return std::nullopt;
    try {
        auto json = nlohmann::json::parse(in);
        if (json.value("schema", 0) != 2 || !json.contains("entry")) return std::nullopt;
        auto entry = std::filesystem::path(json.at("entry").get<std::string>());
        if (entry.empty() || entry.is_absolute()) return std::nullopt;
        return SourceManifest{std::move(entry)};
    } catch (nlohmann::json::exception const&) {
        return std::nullopt;
    }
}

std::optional<std::filesystem::path> find_source_manifest(
    std::filesystem::path const& directory)
{
    auto const source_manifest = directory / IV_SOURCE_MANIFEST_FILE;
    return std::filesystem::exists(source_manifest)
        ? std::optional<std::filesystem::path>{source_manifest}
        : std::nullopt;
}

bool valid_source_name(std::string const& name)
{
    if (name.empty() || name == "." || name == "..") return false;
    if (!(std::isalpha(static_cast<unsigned char>(name.front())) || name.front() == '_')) return false;
    return std::ranges::all_of(name, [](unsigned char character) {
        return std::isalnum(character) || character == '_' || character == '-';
    });
}

std::string module_identifier(std::string const& name)
{
    std::string identifier = "iv.project.";
    for (unsigned char character : name) identifier += character == '-' ? '_' : static_cast<char>(character);
    return identifier;
}

std::string source_template(std::string_view module_id)
{
    return "#include <intravenous/dsl.h>\n\n"
        "void module_main(iv::GraphBuilder& g)\n"
        "{\n"
        "    using namespace iv;\n"
        "    \n"
        "}\n\n"
        "IV_MODULE(\"" + std::string(module_id) + "\", module_main);\n";
}

void copy_initial_compile_commands(std::filesystem::path const& destination)
{
#ifndef IV_CONFIGURED_MODULE_SOURCE_TEMPLATE_COMPILE_DATABASE
    throw std::runtime_error("no compile_commands.json template was configured");
#else
    std::error_code error;
    std::filesystem::copy_file(IV_CONFIGURED_MODULE_SOURCE_TEMPLATE_COMPILE_DATABASE, destination,
        std::filesystem::copy_options::none, error);
    if (error) throw std::runtime_error("cannot copy compile_commands.json template: " + error.message());
#endif
}
}

IvModuleSources::IvModuleSources(
    std::filesystem::path project_root,
    std::vector<std::filesystem::path> shared_roots,
    IvModuleDefinitions const* definitions)
    : project_root_(std::move(project_root))
    , shared_roots_(std::move(shared_roots))
    , definitions_(definitions)
{}

std::vector<IvModuleSourceInfo> IvModuleSources::list_sources() const
{
    std::vector<IvModuleSourceInfo> result;
    auto scan = [&](std::filesystem::path const& root, bool local) {
        std::error_code error;
        if (!std::filesystem::exists(root, error)) return;
        for (std::filesystem::recursive_directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
            auto const& entry = *it;
            if (entry.is_directory()) {
                auto const name = entry.path().filename();
                if (name == ".git" || name == "build" || name == ".cache") {
                    it.disable_recursion_pending();
                }
                continue;
            }
            if (!entry.is_regular_file()
                || !is_iv_source_manifest_file(entry.path().filename().string())) continue;
            auto const directory = entry.path().parent_path();
            auto manifest_path = find_source_manifest(directory);
            if (!manifest_path || *manifest_path != entry.path()) continue;
            auto manifest = read_manifest(*manifest_path);
            if (!manifest) continue;
            if (!std::filesystem::is_regular_file(directory / manifest->entry)) continue;
            auto const normalized_root = std::filesystem::weakly_canonical(directory);
            result.push_back({
                .source_id = normalized_root.generic_string(),
                .source_root = normalized_root,
                .project_local = local,
                .module_ids = {},
                .node_type_ids = {},
            });
            it.disable_recursion_pending();
        }
    };
    scan(project_root_, true);
    for (auto const& root : shared_roots_) scan(root, false);
    std::unordered_map<std::string, std::size_t> source_index;
    std::vector<IvModuleSourceInfo> unique;
    unique.reserve(result.size());
    for (auto& source : result) {
        auto [found, inserted] = source_index.emplace(source.source_id, unique.size());
        if (inserted) {
            unique.push_back(std::move(source));
        } else if (source.project_local) {
            unique[found->second] = std::move(source);
        }
    }
    result = std::move(unique);
    std::ranges::sort(result, {}, &IvModuleSourceInfo::source_id);
    if (definitions_) {
        for (auto& source : result) {
            source.module_ids = definitions_->module_ids_for_source(source.source_id);
            source.node_type_ids =
                definitions_->node_type_ids_for_source(source.source_id);
        }
    }
    return result;
}

std::optional<IvModuleSourceInfo> IvModuleSources::find_source(
    std::string const& module_id) const
{
    if (!definitions_) return std::nullopt;
    auto const source_root = definitions_->source_root_for_module(module_id);
    if (!source_root) return std::nullopt;
    auto const canonical_root = std::filesystem::weakly_canonical(*source_root);
    auto const sources = list_sources();
    auto const found = std::ranges::find_if(sources, [&](IvModuleSourceInfo const& source) {
        return source.source_root == canonical_root;
    });
    if (found != sources.end()) return *found;
    // A persisted instance may point at an otherwise valid source root that
    // is outside the configured discovery roots. The registry remains the
    // authority for the ID-to-source association, so preserve that result.
    return IvModuleSourceInfo{
        .source_id = canonical_root.generic_string(),
        .source_root = canonical_root,
        .project_local = false,
        .module_ids = {},
        .node_type_ids = {},
    };
}

std::vector<std::pair<std::string, std::filesystem::path>>
IvModuleSources::source_declarations() const
{
    std::vector<std::pair<std::string, std::filesystem::path>> declarations;
    for (auto const& source : list_sources()) {
        declarations.emplace_back(source.source_id, source.source_root);
    }
    return declarations;
}

void IvModuleSources::handle_iv_module_source_lookup(
    std::string const &module_id,
    IvModuleSourceLookupBuilder &builder) const
{
    builder.succeed(find_source(module_id));
}

void IvModuleSources::handle_socket_rpc_get_iv_module_sources(
    GetIvModuleSourcesRequest const &,
    SocketRpcIvModuleSourcesResultBuilder &builder) const
{
    builder.succeed(list_sources());
}

void IvModuleSources::handle_socket_rpc_create_iv_module_source(
    CreateIvModuleSourceRequest const &request,
    SocketRpcIvModuleSourceResultBuilder &builder) const
{
    try {
        builder.succeed(create_project_source(request.name));
    } catch (std::exception const &exception) {
        builder.fail(exception.what());
    }
}

IvModuleSourceInfo IvModuleSources::create_project_source(std::string const& name) const
{
    if (!valid_source_name(name)) {
        throw std::runtime_error("module source name must start with a letter or '_' and contain only letters, digits, '_' or '-'");
    }

    auto const root = project_root_ / "modules" / name;
    std::error_code error;
    if (std::filesystem::exists(root, error)) throw std::runtime_error("module source already exists: " + root.string());
    if (error) throw std::runtime_error("cannot inspect module source destination: " + error.message());
    if (!std::filesystem::create_directories(root, error) || error) {
        throw std::runtime_error("cannot create module source directory: " + root.string());
    }

    auto const id = module_identifier(name);
    try {
        nlohmann::json manifest{{"schema", 2}, {"entry", "module.cpp"}};
        std::ofstream manifest_out(
            root / std::string(IV_SOURCE_MANIFEST_FILE),
            std::ios::binary | std::ios::noreplace);
        manifest_out << manifest.dump(2) << '\n';
        if (!manifest_out) {
            throw std::runtime_error(
                "cannot write " + std::string(IV_SOURCE_MANIFEST_FILE));
        }

        std::ofstream source(root / "module.cpp", std::ios::binary | std::ios::noreplace);
        source << source_template(id);
        if (!source) throw std::runtime_error("cannot write module.cpp");
        copy_initial_compile_commands(root / "compile_commands.json");
    } catch (...) {
        std::filesystem::remove_all(root, error);
        throw;
    }

    auto const normalized_root = std::filesystem::weakly_canonical(root);
    return IvModuleSourceInfo{
        .source_id = normalized_root.generic_string(),
        .source_root = normalized_root,
        .project_local = true,
        .module_ids = {},
        .node_type_ids = {},
    };
}
} // namespace iv
