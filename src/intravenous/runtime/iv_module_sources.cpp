#include <intravenous/runtime/iv_module_sources.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <stdexcept>

namespace iv {
namespace {
struct SourceManifest {
    std::string id;
    std::filesystem::path entry;
};

std::optional<SourceManifest> read_manifest(std::filesystem::path const& path)
{
    std::ifstream in(path);
    if (!in) return std::nullopt;
    try {
        auto json = nlohmann::json::parse(in);
        if (json.value("schema", 0) != 1 || !json.contains("id") || !json.contains("entry")) return std::nullopt;
        auto id = json.at("id").get<std::string>();
        auto entry = std::filesystem::path(json.at("entry").get<std::string>());
        if (id.empty() || entry.empty() || entry.is_absolute()) return std::nullopt;
        return SourceManifest{std::move(id), std::move(entry)};
    } catch (nlohmann::json::exception const&) {
        return std::nullopt;
    }
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

std::string source_template()
{
    return "#include <intravenous/dsl.h>\n\n"
        "constexpr void module_main(iv::GraphBuilder& g)\n"
        "{\n"
        "    using namespace iv;\n"
        "    \n"
        "}\n";
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

IvModuleSources::IvModuleSources(std::filesystem::path project_root, std::vector<std::filesystem::path> shared_roots)
    : project_root_(std::move(project_root)), shared_roots_(std::move(shared_roots)) {}

std::vector<IvModuleSourceInfo> IvModuleSources::list_sources() const
{
    std::vector<IvModuleSourceInfo> result;
    auto scan = [&](std::filesystem::path const& root, bool local) {
        std::error_code error;
        if (!std::filesystem::exists(root, error)) return;
        for (std::filesystem::recursive_directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
            auto const& entry = *it;
            if (!entry.is_regular_file() || entry.path().filename() != "iv_module.json") continue;
            auto const directory = entry.path().parent_path();
            auto manifest = read_manifest(entry.path());
            if (!manifest) continue;
            if (!std::filesystem::is_regular_file(directory / manifest->entry)) continue;
            result.push_back({.module_id = manifest->id, .module_root = directory, .project_local = local});
            it.disable_recursion_pending();
        }
    };
    scan(project_root_ / "modules", true);
    for (auto const& root : shared_roots_) scan(root, false);
    std::ranges::sort(result, {}, &IvModuleSourceInfo::module_id);
    return result;
}

std::optional<IvModuleSourceInfo> IvModuleSources::find_source(std::string const& module_id) const
{
    auto const sources = list_sources();
    auto const found = std::ranges::find(sources, module_id, &IvModuleSourceInfo::module_id);
    if (found == sources.end()) return std::nullopt;
    return *found;
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
        nlohmann::json manifest{{"schema", 1}, {"id", id}, {"entry", "module.cpp"}, {"main", "module_main"}};
        std::ofstream manifest_out(root / "iv_module.json", std::ios::binary | std::ios::noreplace);
        manifest_out << manifest.dump(2) << '\n';
        if (!manifest_out) throw std::runtime_error("cannot write iv_module.json");

        std::ofstream source(root / "module.cpp", std::ios::binary | std::ios::noreplace);
        source << source_template();
        if (!source) throw std::runtime_error("cannot write module.cpp");
        copy_initial_compile_commands(root / "compile_commands.json");
    } catch (...) {
        std::filesystem::remove_all(root, error);
        throw;
    }

    return IvModuleSourceInfo{.module_id = id, .module_root = root, .project_local = true};
}
} // namespace iv
