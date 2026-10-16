#include <intravenous/runtime/package_definitions.h>

#include <intravenous/module/package_manifest.h>
#include <intravenous/runtime/package_pipeline_events.h>
#include <intravenous/runtime/socket_rpc_server.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace iv {
namespace {
bool is_within(std::filesystem::path const& candidate, std::filesystem::path const& root)
{
    auto const relative = candidate.lexically_relative(root);
    if (relative.empty()) return false;
    return std::ranges::none_of(relative, [](auto const& component) {
        return component == std::filesystem::path{".."};
    });
}

bool valid_package_name(std::string const& name)
{
    if (name.empty() || name == "." || name == "..") return false;
    if (!(std::isalpha(static_cast<unsigned char>(name.front())) || name.front() == '_')) {
        return false;
    }
    return std::ranges::all_of(name, [](unsigned char character) {
        return std::isalnum(character) || character == '_' || character == '-';
    });
}

std::string module_identifier(std::string const& name)
{
    std::string identifier = "iv.project.";
    for (unsigned char character : name) {
        identifier += character == '-' ? '_' : static_cast<char>(character);
    }
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

std::string command_quote(std::string_view value)
{
    std::string quoted{"\""};
    for (auto const character : value) {
        if (character == '\\' || character == '\"') quoted += '\\';
        quoted += character;
    }
    return quoted + '\"';
}

void write_initial_compile_commands(
    std::filesystem::path const& package_root,
    std::filesystem::path const& destination)
{
#if !defined(IV_CONFIGURED_MODULE_SOURCE_TEMPLATE_CXX_COMPILER) \
    || !defined(IV_CONFIGURED_MODULE_SOURCE_TEMPLATE_DSL_PCH) \
    || !defined(IV_CONFIGURED_MODULE_SOURCE_TEMPLATE_INCLUDE_DIR) \
    || !defined(IV_CONFIGURED_MODULE_SOURCE_TEMPLATE_THIRD_PARTY_INCLUDE_DIR)
    throw std::runtime_error("no package source clangd command was configured");
#else
    auto const source = package_root / "module.cpp";
    auto const command = command_quote(IV_CONFIGURED_MODULE_SOURCE_TEMPLATE_CXX_COMPILER)
        + " -DIV_ENABLE_JUCE_VST=0"
        + " -I" + command_quote(package_root.generic_string())
        + " -isystem " + command_quote(IV_CONFIGURED_MODULE_SOURCE_TEMPLATE_INCLUDE_DIR)
        + " -isystem " + command_quote(IV_CONFIGURED_MODULE_SOURCE_TEMPLATE_THIRD_PARTY_INCLUDE_DIR)
        + " -O0 -flto=full -std=c++23 -fvisibility=hidden"
          " -fvisibility-inlines-hidden -Wall -Wextra -Wpedantic"
        + " -include-pch " + command_quote(IV_CONFIGURED_MODULE_SOURCE_TEMPLATE_DSL_PCH)
        + " -c " + command_quote(source.generic_string());
    nlohmann::json database = nlohmann::json::array({{
        {"directory", package_root.generic_string()},
        {"command", command},
        {"file", source.generic_string()},
    }});
    std::ofstream output(destination, std::ios::binary | std::ios::noreplace);
    if (!output) {
        throw std::runtime_error(
            "cannot create compile_commands.json: " + destination.string());
    }
    output << database.dump(2) << '\n';
    if (!output) {
        throw std::runtime_error(
            "cannot write compile_commands.json: " + destination.string());
    }
#endif
}
} // namespace

PackageDefinitions::PackageDefinitions(std::filesystem::path project_root)
    : project_root_(std::filesystem::weakly_canonical(std::move(project_root)).lexically_normal())
    , snapshot_(std::make_shared<PackageDefinitionsSnapshot const>())
{}

IvPackageInfo& PackageDefinitions::ensure_package_locked(
    std::string const& package_id,
    std::filesystem::path const& package_root)
{
    auto [it, inserted] = packages_by_id_.try_emplace(package_id, IvPackageInfo{
        .package_id = package_id,
        .package_root = package_root,
        .project_local = !package_root.empty() && is_within(package_root, project_root_),
    });
    if (!inserted && !package_root.empty()) {
        it->second.package_root = package_root;
        it->second.project_local = is_within(package_root, project_root_);
    }
    return it->second;
}

void PackageDefinitions::rebuild_snapshot_locked()
{
    auto next = std::make_shared<PackageDefinitionsSnapshot>();
    next->generation = ++snapshot_generation_;
    next->by_package_id = accepted_revisions_by_package_id_;
    snapshot_ = std::move(next);
}

std::shared_ptr<PackageDefinitionsSnapshot const> PackageDefinitions::snapshot() const
{
    std::scoped_lock lock(mutex_);
    return snapshot_;
}

std::vector<IvPackageInfo> PackageDefinitions::list_packages() const
{
    std::vector<IvPackageInfo> packages;
    {
        std::scoped_lock lock(mutex_);
        packages.reserve(packages_by_id_.size());
        for (auto const& [_, package] : packages_by_id_) packages.push_back(package);
    }
    std::ranges::sort(packages, {}, &IvPackageInfo::package_id);
    return packages;
}

void PackageDefinitions::apply_publication_result(
    PackageDefinitionsPublicationRequest const& request)
{
    std::scoped_lock lock(mutex_);
    for (auto& [package_id, package] : packages_by_id_) {
        auto const modules = request.published_module_ids_by_package_id.find(package_id);
        package.module_ids = modules == request.published_module_ids_by_package_id.end()
            ? std::vector<std::string>{}
            : modules->second;
        auto const leaves = request.published_leaf_ids_by_package_id.find(package_id);
        package.node_type_ids = leaves == request.published_leaf_ids_by_package_id.end()
            ? std::vector<std::string>{}
            : leaves->second;
        auto const message = request.publication_messages_by_package_id.find(package_id);
        package.publication_message = message == request.publication_messages_by_package_id.end()
            ? std::string{}
            : message->second;
    }
}

void PackageDefinitions::handle_package_refresh(
    PackageRefreshTransaction const& transaction)
{
    bool accepted_changed = false;
    std::shared_ptr<PackageDefinitionsSnapshot const> next_snapshot;
    {
        std::scoped_lock lock(mutex_);
        for (auto const& declaration : transaction.declarations.created) {
            auto& package = ensure_package_locked(
                declaration.package_id, declaration.package_root);
            package.build_state = PackageBuildState::queued;
            package.build_message.clear();
        }
        for (auto const& declaration : transaction.declarations.updated) {
            auto& package = ensure_package_locked(
                declaration.package_id, declaration.package_root);
            package.build_state = PackageBuildState::queued;
            package.build_message.clear();
        }
        for (auto const& package_id : transaction.declarations.deleted_package_ids) {
            packages_by_id_.erase(package_id);
            accepted_changed = accepted_revisions_by_package_id_.erase(package_id) > 0
                || accepted_changed;
        }

        for (auto const& revision : transaction.successful_revisions) {
            auto accepted = std::make_shared<PackageRevision const>(revision);
            accepted_revisions_by_package_id_.insert_or_assign(revision.package_id, accepted);
            auto& package = ensure_package_locked(revision.package_id, revision.package_root);
            package.build_state = PackageBuildState::built;
            package.build_message.clear();
            accepted_changed = true;
        }
        for (auto const& failure : transaction.failed_builds) {
            auto& package = ensure_package_locked(failure.package_id, failure.package_root);
            package.build_state = PackageBuildState::failed;
            package.build_message = failure.message;
        }

        if (accepted_changed) {
            rebuild_snapshot_locked();
            next_snapshot = snapshot_;
        }
    }

    if (!accepted_changed) return;

    PackageDefinitionsPublicationRequest request{
        .snapshot = std::move(next_snapshot),
    };
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_package_definitions_publication_requested_event,
        request);
    apply_publication_result(request);
}

void PackageDefinitions::handle_socket_rpc_get_iv_package_definitions(
    GetIvPackageDefinitionsRequest const&,
    SocketRpcIvPackageDefinitionsResultBuilder& builder) const
{
    builder.succeed(list_packages());
}

void PackageDefinitions::handle_socket_rpc_create_iv_package(
    CreateIvPackageRequest const& request,
    SocketRpcIvPackageResultBuilder& builder) const
{
    try {
        builder.succeed(create_project_package(request.name));
    } catch (std::exception const& exception) {
        builder.fail(exception.what());
    }
}

IvPackageInfo PackageDefinitions::create_project_package(std::string const& name) const
{
    if (!valid_package_name(name)) {
        throw std::runtime_error(
            "IV package name must start with a letter or '_' and contain only letters, digits, '_' or '-'");
    }

    auto const root = project_root_ / "modules" / name;
    std::error_code error;
    if (std::filesystem::exists(root, error)) {
        throw std::runtime_error("IV package already exists: " + root.string());
    }
    if (error) {
        throw std::runtime_error("cannot inspect IV package destination: " + error.message());
    }
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
            throw std::runtime_error("cannot write " + std::string(IV_PACKAGE_MANIFEST_FILE));
        }

        std::ofstream package(root / "module.cpp", std::ios::binary | std::ios::noreplace);
        package << package_template(id);
        if (!package) throw std::runtime_error("cannot write module.cpp");
        write_initial_compile_commands(root, root / "compile_commands.json");
    } catch (...) {
        std::filesystem::remove_all(root, error);
        throw;
    }

    auto const normalized_root = std::filesystem::weakly_canonical(root);
    return IvPackageInfo{
        .package_id = normalized_root.generic_string(),
        .package_root = normalized_root,
        .project_local = true,
    };
}
} // namespace iv
