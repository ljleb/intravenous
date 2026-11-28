#include <intravenous/packages/presentation_artifacts.h>

#include <intravenous/module/package_manifest.h>
#include <intravenous/packages/package_definitions.h>

#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Base64.h>
#include <llvm/Support/SHA256.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace iv {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;

constexpr std::size_t max_presentations = 32;
constexpr std::size_t max_manifest_bytes = 64 * 1024;
constexpr std::size_t max_artifact_bytes = 256 * 1024;

std::string read_bounded(fs::path const& path, std::size_t maximum)
{
    if (!fs::is_regular_file(fs::symlink_status(path)) || fs::file_size(path) > maximum)
        throw std::runtime_error("Expected a regular presentation file within the size limit: " + path.string());
    std::ifstream input(path, std::ios::binary);
    std::string bytes(maximum + 1, '\0');
    if (!input) throw std::runtime_error("Could not open presentation file: " + path.string());
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (input.bad() || !input.eof())
        throw std::runtime_error("Could not read bounded presentation file: " + path.string());
    bytes.resize(static_cast<std::size_t>(input.gcount()));
    return bytes;
}

std::string sha256(std::string_view bytes)
{
    llvm::SHA256 hash;
    hash.update(llvm::StringRef(bytes.data(), bytes.size()));
    auto const digest = hash.final();
    std::string hex;
    hex.reserve(digest.size() * 2);
    for (auto byte : digest) {
        hex.push_back("0123456789abcdef"[byte >> 4]);
        hex.push_back("0123456789abcdef"[byte & 15]);
    }
    return hex;
}

bool can_present(IvPackageDeclaration const& package, std::string const& owner)
{
    return package.package_namespace == owner
        || is_implicit_package_dependency(package.package_namespace, owner)
        || std::ranges::find(package.package_dependencies, owner) != package.package_dependencies.end();
}

using DefinitionOwners = std::unordered_map<std::string, std::string>;

bool valid_presentation_id(std::string_view id)
{
    return !id.empty() && id.size() <= 128 && id != "." && id != ".."
        && std::ranges::all_of(id, [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        });
}

void validate_binding(Json const& binding, IvPackageDeclaration const& package,
    DefinitionOwners const& definitions)
{
    if (!binding.at("title").is_string() || !binding.at("definitionIds").is_array()
        || binding.at("definitionIds").empty())
        throw std::runtime_error("Presentation requires a title and at least one definition ID");
    for (auto const& target : binding.at("definitionIds")) {
        auto const definition = target.get<std::string>();
        auto const owner = definitions.find(definition);
        if (owner == definitions.end() || !can_present(package, owner->second))
            throw std::runtime_error("Presentation target is unavailable to package: " + definition);
        if (binding.contains("initialConfiguration") && owner->second != package.package_namespace)
            throw std::runtime_error("Only the defining package may specify an initial configuration: " + definition);
    }
}

std::vector<PresentationObservationBinding> read_observations(Json const& binding)
{
    auto const list = binding.find("observations");
    if (list == binding.end()) return {};
    if (!list->is_array() || list->size() > 8)
        throw std::runtime_error("Expected at most eight presentation observations");
    std::vector<PresentationObservationBinding> observations;
    for (auto const& entry : *list) {
        PresentationObservationBinding observation{
            .id = entry.at("id").get<std::string>(),
            .port = entry.at("port").get<std::string>(),
            .direction = entry.at("direction").get<std::string>(),
            .operation = entry.value("operation", std::string("waveform")),
            .points = {}, .group = {},
        };
        if (!valid_presentation_id(observation.id) || observation.port.empty() || observation.port.size() > 128
            || std::ranges::contains(observations, observation.id, &PresentationObservationBinding::id))
            throw std::runtime_error("Invalid presentation observation ID or port");
        if (observation.operation == "waveform") {
            auto const& points = entry.at("points");
            if ((observation.direction != "input" && observation.direction != "output")
                || !points.is_number_integer() || points < 2 || points > 1024)
                throw std::runtime_error("A waveform observation needs an input or output and 2 to 1024 points");
            observation.points = points.get<unsigned>();
        } else if (observation.operation != "trigger-bins" || observation.direction != "output" || entry.contains("points")) {
            throw std::runtime_error("Invalid presentation observation operation");
        }
        if (auto const group = entry.find("group"); group != entry.end()) {
            observation.group = group->get<std::string>();
            if (!valid_presentation_id(*observation.group)) throw std::runtime_error("Invalid presentation observation group");
        }
        observation.when_connected = entry.value("whenConnected", false);
        observation.complete_only = entry.value("completeOnly", false);
        observations.push_back(std::move(observation));
    }
    return observations;
}

struct ArtifactFile {
    Json binding;
    std::string digest{};
    std::shared_ptr<std::string const> encoded{};
    std::string error{};
};

using ArtifactFiles = std::vector<std::pair<std::string, ArtifactFile>>;

ArtifactFiles read_package_files(fs::path const& root)
{
    auto const path = root / IV_PACKAGE_MANIFEST_FILE;
    auto const document = Json::parse(read_bounded(path, max_manifest_bytes));
    auto const group = document.find("presentations");
    if (group == document.end()) return {};
    if (!group->is_object() || group->size() > max_presentations)
        throw std::runtime_error("Expected a presentation map within the entry limit: " + path.string());

    auto const directory = root / IV_PACKAGE_PRESENTATIONS_DIR;
    ArtifactFiles result;
    for (auto const& [id, binding] : group->items()) {
        ArtifactFile file{.binding = binding};
        try {
            if (!valid_presentation_id(id)) throw std::runtime_error("Invalid presentation ID");
            if (!fs::is_directory(fs::symlink_status(directory)))
                throw std::runtime_error("Expected a presentation artifact directory");
            auto const bytes = read_bounded(directory / (id + ".wasm"), max_artifact_bytes);
            if (!bytes.starts_with(std::string_view("\0asm\1\0\0\0", 8)))
                throw std::runtime_error("Expected core WebAssembly");
            file.digest = sha256(bytes);
            file.encoded = std::make_shared<std::string const>(llvm::encodeBase64(bytes));
        } catch (std::exception const& error) {
            file.error = error.what();
        }
        result.emplace_back(id, std::move(file));
    }
    return result;
}

} // namespace

struct PresentationArtifacts::Entry {
    PresentationCatalogEntry metadata;
    std::shared_ptr<std::string const> encoded;
};

struct PresentationArtifacts::ReleasedPackage {
    IvPackageDeclaration declaration;
    ArtifactFiles files;
};

bool PresentationArtifacts::refresh(std::span<IvPackageDeclaration const> declarations,
    std::span<IvPackageInfo const> definitions)
{
    DefinitionOwners owners;
    for (auto const& package : definitions) {
        for (auto const& id : package.module_ids) owners.emplace(id, package.package_namespace);
        for (auto const& id : package.node_type_ids) owners.emplace(id, package.package_namespace);
    }
    std::unordered_map<std::string, std::size_t> namespace_counts;
    for (auto const& package : declarations) ++namespace_counts[package.package_namespace];

    decltype(released_) next_released;
    Entries next_publication;
    for (auto const& package : declarations) {
        if (namespace_counts.at(package.package_namespace) != 1) continue;
        ArtifactFiles development_files;
        auto const* files = &development_files;
        try {
            if (package.provenance == PackageProvenance::project_local) {
                development_files = read_package_files(package.package_root);
            } else {
                auto const existing = released_.find(package.package_id);
                auto pinned = existing != released_.end() && existing->second->declaration == package
                    ? existing->second
                    : std::make_shared<ReleasedPackage const>(ReleasedPackage{package, read_package_files(package.package_root)});
                files = &pinned->files;
                next_released.emplace(package.package_id, std::move(pinned));
            }
        } catch (std::exception const& error) {
            if (next_publication.size() < max_presentations) {
                auto entry = std::make_shared<Entry>();
                entry->metadata.error = error.what();
                next_publication.emplace(package.package_id, std::move(entry));
            }
            continue;
        }
        for (auto const& [id, file] : *files) {
            if (next_publication.size() == max_presentations) break;
            auto const key = package.package_id + "/" + id;
            auto candidate = std::make_shared<Entry>();
            try {
                validate_binding(file.binding, package, owners);
                auto observations = read_observations(file.binding);
                auto& metadata = candidate->metadata;
                metadata.id = package.package_namespace + "/" + id;
                metadata.provider = package.package_namespace;
                metadata.title = file.binding.at("title").get<std::string>();
                metadata.definition_ids = file.binding.at("definitionIds").get<std::vector<std::string>>();
                metadata.observations = std::move(observations);
                if (auto const initial = file.binding.find("initialConfiguration"); initial != file.binding.end())
                    metadata.initial_configuration = initial->get<std::string>();
                if (!file.error.empty()) throw std::runtime_error(file.error);
                metadata.digest = file.digest;
                candidate->encoded = file.encoded;
            } catch (std::exception const& error) {
                auto& metadata = candidate->metadata;
                auto const previous = published_.find(key);
                if (!metadata.id.empty() && previous != published_.end() && previous->second->encoded) {
                    auto const& old = previous->second->metadata;
                    if (metadata.id == old.id && metadata.definition_ids == old.definition_ids
                        && metadata.observations == old.observations
                        && metadata.initial_configuration == old.initial_configuration) {
                        metadata.digest = old.digest;
                        candidate->encoded = previous->second->encoded;
                    }
                }
                metadata.error = package.package_namespace + "/" + id + ": " + error.what();
            }
            next_publication.emplace(key, std::move(candidate));
        }
    }
    released_ = std::move(next_released);
    auto const changed = !ready_ || published_.size() != next_publication.size()
        || std::ranges::any_of(next_publication, [&](auto const& entry) {
            auto const old = published_.find(entry.first);
            return old == published_.end() || old->second->metadata != entry.second->metadata;
        });
    if (changed) {
        std::scoped_lock lock(mutex_);
        published_ = std::move(next_publication);
        ready_ = true;
    }
    return changed;
}

PresentationCatalog PresentationArtifacts::list() const
{
    std::scoped_lock lock(mutex_);
    PresentationCatalog result{.ready = ready_, .entries = {}};
    result.entries.reserve(published_.size());
    for (auto const& [_, entry] : published_) result.entries.push_back(entry->metadata);
    return result;
}

std::shared_ptr<std::string const> PresentationArtifacts::fetch(std::string const& digest) const
{
    std::scoped_lock lock(mutex_);
    for (auto const& [_, entry] : published_) {
        if (entry->encoded && entry->metadata.digest == digest) return entry->encoded;
    }
    return {};
}
} // namespace iv
