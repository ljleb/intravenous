#include <intravenous/runtime/iv_module_sources.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <stdexcept>

namespace iv {
namespace {
std::optional<std::string> module_id(std::filesystem::path const& source)
{
    std::ifstream in(source);
    std::string text((std::istreambuf_iterator<char>(in)), {});
    auto const macro = text.find("IV_EXPORT_MODULE");
    auto const first = text.find('"', macro);
    auto const last = first == std::string::npos ? first : text.find('"', first + 1);
    if (macro == std::string::npos || first == std::string::npos || last == std::string::npos) return std::nullopt;
    return text.substr(first + 1, last - first - 1);
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
    for (unsigned char character : name) {
        identifier += character == '-' ? '_' : static_cast<char>(character);
    }
    return identifier;
}

std::string source_template(std::string const& name)
{
    return "#include <intravenous/dsl.h>\n\n"
        "inline void module_main(iv::ModuleContext const& ctx)\n"
        "{\n"
        "    using namespace iv;\n"
        "    auto& g = ctx.builder();\n"
        "    g.outputs();\n"
        "}\n\n"
        "IV_EXPORT_MODULE(\"" + module_identifier(name) + "\", module_main);\n";
}
}

IvModuleSources::IvModuleSources(std::filesystem::path project_root, std::vector<std::filesystem::path> shared_roots)
    : project_root_(std::move(project_root)), shared_roots_(std::move(shared_roots)) {}

std::vector<IvModuleSourceInfo> IvModuleSources::list_sources() const
{
    std::vector<IvModuleSourceInfo> result;
    auto scan = [&](std::filesystem::path const& root, bool local) {
        std::error_code error;
        for (std::filesystem::recursive_directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
            auto const& entry = *it;
            if (!entry.is_regular_file() || entry.path().filename() != "module.cpp") continue;
            auto const directory = entry.path().parent_path();
            if (!std::filesystem::is_regular_file(directory / "iv_module.json")) continue;
            if (auto id = module_id(entry.path())) result.push_back({.module_id = *id, .module_root = directory, .project_local = local});
        }
    };
    scan(project_root_ / "modules", true);
    for (auto const& root : shared_roots_) scan(root, false);
    std::ranges::sort(result, {}, &IvModuleSourceInfo::module_id);
    return result;
}

IvModuleSourceInfo IvModuleSources::create_project_source(std::string const& name) const
{
    if (!valid_source_name(name)) {
        throw std::runtime_error("module source name must start with a letter or '_' and contain only letters, digits, '_' or '-'");
    }

    auto const root = project_root_ / "modules" / name;
    std::error_code error;
    if (std::filesystem::exists(root, error)) {
        throw std::runtime_error("module source already exists: " + root.string());
    }
    if (error) {
        throw std::runtime_error("cannot inspect module source destination: " + error.message());
    }
    if (!std::filesystem::create_directories(root, error) || error) {
        throw std::runtime_error("cannot create module source directory: " + root.string());
    }

    try {
        std::ofstream manifest(root / "iv_module.json", std::ios::binary | std::ios::noreplace);
        manifest << "{}\n";
        if (!manifest) throw std::runtime_error("cannot write iv_module.json");

        std::ofstream source(root / "module.cpp", std::ios::binary | std::ios::noreplace);
        source << source_template(name);
        if (!source) throw std::runtime_error("cannot write module.cpp");
    } catch (...) {
        std::filesystem::remove_all(root, error);
        throw;
    }

    return IvModuleSourceInfo{
        .module_id = module_identifier(name),
        .module_root = root,
        .project_local = true,
    };
}
} // namespace iv
