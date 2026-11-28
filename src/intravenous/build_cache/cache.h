#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace iv::build_cache {

enum class LockMode { shared, exclusive, try_exclusive };

class Lock {
public:
    // An absent lock file is unlocked when create is false. This lets
    // inspection avoid creating a cache or touching its metadata.
    explicit Lock(std::filesystem::path const& path, LockMode mode, bool create = true);
    ~Lock();
    Lock(Lock const&) = delete;
    Lock& operator=(Lock const&) = delete;

    bool acquired() const { return acquired_; }
    int fd() const { return fd_; }

private:
    int fd_ = -1;
    bool acquired_ = false;
};

std::filesystem::path root();

struct ConfigEdit {
    std::string_view key;
    std::optional<std::string_view> value;
};

enum class SizeUnits { bytes, decimal, binary };

std::string config_table(SizeUnits units);
// A missing value resets one key. An empty list resets all keys.
void edit_config(std::span<ConfigEdit const> edits);

std::string stable_hash(std::string_view text);
void mark_used(std::filesystem::path const& locks, std::string_view source);
void mark_dirty(std::filesystem::path const& locks);

// The child inherits only the requested cache locks across exec. A builder
// remains protected if the parent process dies while it is running.
int run_process(std::span<char* const> argv, std::span<int const> inherited_locks = {});

class ScratchDirectory {
public:
    explicit ScratchDirectory(std::filesystem::path const& cache_root);
    std::filesystem::path const& path() const { return path_; }
    int fd() const { return lease_->fd(); }

private:
    std::filesystem::path path_;
    std::optional<Lock> lease_;
};

enum class Mode { status, dry_run, collect, before_build, after_build };

struct Decision {
    std::filesystem::path path;
    std::string source;
    std::string reason;
    bool remove;
};

struct Report {
    std::vector<Decision> workspaces;
    std::vector<Decision> artifacts;
    std::vector<Decision> scratch;
    std::uint64_t bytes = 0;
    std::uint64_t remaining_bytes = 0;
    std::uint64_t free_bytes = 0;
    std::uint64_t predicted_free_bytes = 0;
};

using Log = std::function<void(std::string const&)>;
Report sweep(std::filesystem::path const& cache_root, Mode mode, Log const& log = {});

} // namespace iv::build_cache
