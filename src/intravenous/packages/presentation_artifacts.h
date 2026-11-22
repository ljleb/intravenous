#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace iv {
struct IvPackageDeclaration;
struct IvPackageInfo;

struct PresentationCatalogEntry {
    std::string id;
    std::string provider;
    std::string title;
    std::vector<std::string> definition_ids;
    std::optional<std::string> initial_configuration;
    std::string digest;
    std::string error;

    bool operator==(PresentationCatalogEntry const&) const = default;
};

struct PresentationCatalog {
    bool ready = false;
    std::vector<PresentationCatalogEntry> entries;
};

// The package service reads files on its worker. RPC serves immutable bytes
// from the published snapshot.
class PresentationArtifacts {
    struct Entry;
    struct ReleasedPackage;
    using Entries = std::unordered_map<std::string, std::shared_ptr<Entry const>>;
    std::unordered_map<std::string, std::shared_ptr<ReleasedPackage const>> released_;
    mutable std::mutex mutex_;
    Entries published_;
    bool ready_ = false;

public:
    bool refresh(std::span<IvPackageDeclaration const> declarations,
        std::span<IvPackageInfo const> definitions);
    PresentationCatalog list() const;
    std::shared_ptr<std::string const> fetch(std::string const& digest) const;
};
} // namespace iv
