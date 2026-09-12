#include <intravenous/runtime/iv_packages.h>

#include <intravenous/module/package_manifest.h>
#include <intravenous/runtime/iv_module_definitions.h>
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
struct PackageManifest {
    std::filesystem::path entry;
};

std::optional<PackageManifest> read_manifest(std::filesystem::path const& path)
{
    std::ifstream in(path);
    if (!in) return std::nullopt;
    try {
        auto json = nlohmann::json::parse(in);
        if (json.value("schema", 0) != 2 || !json.contains("entry")) return std::nullopt;
        auto entry = std::filesystem::path(json.at("entry").get<std::string>());
        if (entry.empty() || entry.is_absolute()) return std::nullopt;
        return PackageManifest{std::move(entry)};
    } catch (nlohmann::json::exception const&) {
        return std::nullopt;
    }
}

std::optional<std::filesystem::path> find_package_manifest(
    std::filesystem::path const& directory)
{
    auto const package_manifest = directory / IV_PACKAGE_MANIFEST_FILE;
    return std::filesystem::exists(package_manifest)
        ? std::optional<std::filesystem::path>{package_manifest}
        : std::nullopt;
}

bool is_within(
    std::filesystem::path const& candidate,
    std::filesystem::path const& root)
{
    auto const relative = candidate.lexically_relative(root);
    if (relative.empty()) {
        return false;
    }
    return std::ranges::none_of(relative, [](auto const& component) {
        return component == std::filesystem::path{".."};
    });
}

bool valid_package_name(std::string const& name)
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

std::string package_template(std::string_view module_id)
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

IvPackages::IvPackages(
    std::filesystem::path project_root,
    IvModuleDefinitions& definitions,
    IvModuleReload const& reload)
    : project_root_(std::filesystem::weakly_canonical(std::move(project_root)).lexically_normal())
    , definitions_(definitions)
    , reload_(reload)
{}

std::vector<IvPackageInfo> discover_iv_packages(
    std::filesystem::path const& project_root,
    std::vector<std::filesystem::path> const& shared_roots)
{
    std::vector<IvPackageInfo> result;
    auto scan = [&](std::filesystem::path const& root, bool local) {
        std::error_code error;
        if (!std::filesystem::exists(root, error)) return;
        for (std::filesystem::recursive_directory_iterator it(root, error), end;
             !error && it != end;
             it.increment(error)) {
            auto const& entry = *it;
            if (entry.is_directory()) {
                auto const name = entry.path().filename();
                if (name == ".git" || name == "build" || name == ".cache") {
                    it.disable_recursion_pending();
                }
                continue;
            }
            if (!entry.is_regular_file()
                || !is_iv_package_manifest_file(entry.path().filename().string())) {
                continue;
            }
            auto const directory = entry.path().parent_path();
            auto manifest_path = find_package_manifest(directory);
            if (!manifest_path || *manifest_path != entry.path()) continue;
            auto manifest = read_manifest(*manifest_path);
            if (!manifest) continue;
            if (!std::filesystem::is_regular_file(directory / manifest->entry)) continue;
            auto const normalized_root = std::filesystem::weakly_canonical(directory);
            result.push_back({
                .package_id = normalized_root.generic_string(),
                .package_root = normalized_root,
                .project_local = local,
                .module_ids = {},
                .node_type_ids = {},
            });
            it.disable_recursion_pending();
        }
    };
    scan(project_root, true);
    for (auto const& root : shared_roots) scan(root, false);

    std::unordered_map<std::string, std::size_t> package_index;
    std::vector<IvPackageInfo> unique;
    unique.reserve(result.size());
    for (auto& package : result) {
        auto [found, inserted] = package_index.emplace(
            package.package_id, unique.size());
        if (inserted) {
            unique.push_back(std::move(package));
        } else if (package.project_local) {
            unique[found->second] = std::move(package);
        }
    }
    std::ranges::sort(unique, {}, &IvPackageInfo::package_id);
    return unique;
}

std::vector<std::pair<std::string, std::filesystem::path>>
discover_iv_package_declarations(
    std::filesystem::path const& project_root,
    std::vector<std::filesystem::path> const& shared_roots)
{
    std::vector<std::pair<std::string, std::filesystem::path>> declarations;
    for (auto const& package : discover_iv_packages(project_root, shared_roots)) {
        declarations.emplace_back(package.package_id, package.package_root);
    }
    return declarations;
}

std::vector<IvPackageInfo> IvPackages::list_packages() const
{
    auto const registry = definitions_.package_definition_snapshots();
    auto const build_statuses = reload_.package_build_statuses();

    std::unordered_map<std::string, IvPackageBuildStatus> status_by_package_id;
    status_by_package_id.reserve(build_statuses.size());
    for (auto const& status : build_statuses) {
        status_by_package_id.emplace(status.package_id, status);
    }

    std::vector<IvPackageInfo> packages;
    packages.reserve(registry.size());
    for (auto const& package : registry) {
        auto const& declaration = package.declaration;
        auto const status = status_by_package_id.find(declaration.package_id);
        packages.push_back(IvPackageInfo{
            .package_id = declaration.package_id,
            .package_root = declaration.package_root,
            .project_local = is_within(declaration.package_root, project_root_),
            .module_ids = package.published_module_ids,
            .node_type_ids = package.published_node_type_ids,
            .build_state = status == status_by_package_id.end()
                ? IvPackageBuildState::queued
                : status->second.state,
            .build_message = status == status_by_package_id.end()
                ? std::string{}
                : status->second.message,
            .publication_message = package.publication_message,
        });
    }
    std::ranges::sort(packages, {}, &IvPackageInfo::package_id);
    return packages;
}

void IvPackages::handle_socket_rpc_get_iv_packages(
    GetIvPackagesRequest const &,
    SocketRpcIvPackagesResultBuilder &builder) const
{
    builder.succeed(list_packages());
}

void IvPackages::handle_socket_rpc_create_iv_package(
    CreateIvPackageRequest const &request,
    SocketRpcIvPackageResultBuilder &builder) const
{
    try {
        builder.succeed(create_project_package(request.name));
    } catch (std::exception const &exception) {
        builder.fail(exception.what());
    }
}

IvPackageInfo IvPackages::create_project_package(std::string const& name) const
{
    if (!valid_package_name(name)) {
        throw std::runtime_error("IV package name must start with a letter or '_' and contain only letters, digits, '_' or '-'");
    }

    auto const root = project_root_ / "modules" / name;
    std::error_code error;
    if (std::filesystem::exists(root, error)) throw std::runtime_error("IV package already exists: " + root.string());
    if (error) throw std::runtime_error("cannot inspect IV package destination: " + error.message());
    if (!std::filesystem::create_directories(root, error) || error) {
        throw std::runtime_error("cannot create IV package directory: " + root.string());
    }

    auto const id = module_identifier(name);
    try {
        nlohmann::json manifest{{"schema", 2}, {"entry", "module.cpp"}};
        std::ofstream manifest_out(
            root / std::string(IV_PACKAGE_MANIFEST_FILE),
            std::ios::binary | std::ios::noreplace);
        manifest_out << manifest.dump(2) << '\n';
        if (!manifest_out) {
            throw std::runtime_error(
                "cannot write " + std::string(IV_PACKAGE_MANIFEST_FILE));
        }

        std::ofstream package(root / "module.cpp", std::ios::binary | std::ios::noreplace);
        package << package_template(id);
        if (!package) throw std::runtime_error("cannot write module.cpp");
        copy_initial_compile_commands(root / "compile_commands.json");
    } catch (...) {
        std::filesystem::remove_all(root, error);
        throw;
    }

    auto const normalized_root = std::filesystem::weakly_canonical(root);
    auto const package_id = normalized_root.generic_string();
    definitions_.declare_package(package_id, normalized_root);
    return IvPackageInfo{
        .package_id = package_id,
        .package_root = normalized_root,
        .project_local = true,
        .module_ids = {},
        .node_type_ids = {},
        .build_state = IvPackageBuildState::queued,
        .build_message = {},
        .publication_message = {},
    };
}
} // namespace iv
