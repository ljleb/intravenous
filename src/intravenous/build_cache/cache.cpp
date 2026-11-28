#include "cache.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <tuple>
#include <unordered_set>
#include <utility>

#include <fcntl.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace iv::build_cache {
namespace {
namespace fs = std::filesystem;
constexpr std::uint64_t gib = 1024ull * 1024 * 1024;
constexpr std::string_view size_suffixes = "KMGT";

fs::path user_directory(char const* variable, char const* fallback)
{
    if (auto const* value = std::getenv(variable); value && fs::path(value).is_absolute()) {
        return value;
    }
    if (auto const* home = std::getenv("HOME"); home && fs::path(home).is_absolute()) {
        return fs::path(home) / fallback;
    }
    throw std::runtime_error(std::string("Set HOME or ") + variable + " for the build cache");
}

bool plain_directory(fs::path const& path)
{
    return fs::is_directory(fs::symlink_status(path));
}

bool plain_file(fs::path const& path)
{
    return fs::is_regular_file(fs::symlink_status(path));
}

std::string read_line(fs::path const& path)
{
    std::string value;
    std::ifstream input(path);
    std::getline(input, value);
    return value;
}

void write_text(fs::path const& path, std::string_view text)
{
    std::ofstream file;
    file.exceptions(std::ios::failbit | std::ios::badbit);
    try {
        file.open(path, std::ios::binary | std::ios::trunc);
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        file.close();
    } catch (std::ios_base::failure const&) {
        throw std::runtime_error("Could not write " + path.string());
    }
}

struct Policy {
    std::uint64_t threshold = 40 * gib;
    std::uint64_t target = 30 * gib;
    std::uint64_t minimum_free = 15 * gib;
    std::uint64_t idle_days = 30;
};

struct Setting {
    std::string_view name;
    std::uint64_t Policy::* field;
    bool size;
};

constexpr std::array settings{
    Setting{"thresholdBytes", &Policy::threshold, true},
    Setting{"targetBytes", &Policy::target, true},
    Setting{"minimumFreeBytes", &Policy::minimum_free, true},
    Setting{"maxIdleDays", &Policy::idle_days, false},
};

Setting const& get_setting(std::string_view name)
{
    auto const it = std::ranges::find(settings, name, &Setting::name);
    if (it == settings.end()) throw std::invalid_argument("Unknown build cache setting " + std::string(name));
    return *it;
}

fs::path config_path()
{
    return user_directory("XDG_CONFIG_HOME", ".config") / "intravenous/build-cache.json";
}

nlohmann::json read_config(fs::path const& path)
{
    if (!fs::exists(path)) return nlohmann::json::object();
    std::ifstream input(path);
    auto values = nlohmann::json::parse(input, nullptr, false);
    if (!values.is_object()) throw std::runtime_error("Invalid build cache configuration in " + path.string());
    return values;
}

Policy parse_policy(nlohmann::json const& values, fs::path const& path)
{
    Policy policy;
    for (auto const& [name, value] : values.items()) {
        auto const& setting = get_setting(name);
        if (!value.is_number_unsigned()) {
            throw std::runtime_error(name + " in " + path.string() + " must be a nonnegative integer");
        }
        policy.*setting.field = value.get<std::uint64_t>();
    }
    if (policy.target > policy.threshold) {
        throw std::runtime_error("targetBytes must be at most thresholdBytes in " + path.string());
    }
    return policy;
}

Policy read_policy()
{
    auto const path = config_path();
    return parse_policy(read_config(path), path);
}

std::uint64_t parse_unsigned(std::string_view text, std::string_view key)
{
    std::uint64_t value = 0;
    if (!text.empty()) {
        auto const end = text.data() + text.size();
        auto const [position, error] = std::from_chars(text.data(), end, value);
        if (error == std::errc{} && position == end) return value;
    }
    throw std::invalid_argument(std::string(key) + " requires a nonnegative whole number");
}

std::uint64_t size_multiplier(std::string_view suffix)
{
    if (suffix.empty() || suffix == "B" || suffix == "b") return 1;
    auto const first = suffix.front();
    auto const upper = size_suffixes.find(first);
    auto const index = upper != size_suffixes.npos ? upper : std::string_view("kmgt").find(first);
    if (index == size_suffixes.npos) return 0;
    auto const ending = suffix.substr(1);
    bool const binary = ending.empty()
        || (ending.size() == 2 && (ending[0] == 'i' || ending[0] == 'I')
            && (ending[1] == 'B' || ending[1] == 'b'));
    if (!binary && ending != "B" && ending != "b") return 0;
    std::uint64_t const base = binary ? 1024 : 1000;
    std::uint64_t multiplier = 1;
    for (std::size_t i = 0; i <= index; ++i) multiplier *= base;
    return multiplier;
}

void print_size(std::ostream& output, std::uint64_t bytes, SizeUnits units)
{
    if (units == SizeUnits::bytes) {
        output << bytes;
        return;
    }
    bool const binary = units == SizeUnits::binary;
    std::uint64_t const base = binary ? 1024 : 1000;
    std::uint64_t scale = 1;
    std::size_t index = 0;
    while (index < size_suffixes.size() && bytes / scale >= base) {
        scale *= base;
        ++index;
    }
    auto const hundredths = (bytes % scale * 100 + scale / 2) / scale;
    output << bytes / scale + hundredths / 100;
    if (auto const fraction = hundredths % 100; fraction != 0) {
        output << '.' << fraction / 10;
        if (fraction % 10 != 0) output << fraction % 10;
    }
    if (index != 0) output << (!binary && index == 1 ? 'k' : size_suffixes[index - 1]);
    if (!binary || index == 0) output << 'B';
}

std::uint64_t parse_size(std::string_view text, std::string_view key)
{
    auto const suffix_begin = text.find_first_not_of("0123456789.");
    auto const multiplier = size_multiplier(text.substr(std::min(suffix_begin, text.size())));
    if (multiplier == 0) throw std::invalid_argument(std::string(key) + " has an unknown size suffix");
    auto const number = text.substr(0, suffix_begin);
    auto const dot = number.find('.');
    auto const whole = parse_unsigned(number.substr(0, dot), key);
    std::uint64_t fraction = 0;
    std::uint64_t discarded = 0;
    if (dot != number.npos) {
        auto const digits = number.substr(dot + 1);
        if (digits.empty() || digits.find('.') != digits.npos) {
            throw std::invalid_argument(std::string(key) + " requires digits after the decimal point");
        }
        // Work from the least significant digit to keep the intermediate
        // product bounded and round to a byte without floating point.
        for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
            auto const scaled = multiplier * static_cast<unsigned>(*it - '0') + fraction;
            fraction = scaled / 10;
            discarded = scaled % 10;
        }
        if (discarded >= 5) ++fraction;
    }
    if (whole > (std::numeric_limits<std::uint64_t>::max() - fraction) / multiplier) {
        throw std::invalid_argument(std::string(key) + " exceeds the supported size in bytes");
    }
    return whole * multiplier + fraction;
}

void write_config(fs::path const& path, nlohmann::json const& values)
{
    auto temporary = path.string() + ".XXXXXX";
    int const fd = ::mkstemp(temporary.data());
    if (fd < 0) throw std::system_error(errno, std::generic_category(), "Could not stage " + path.string());
    ::close(fd);
    try {
        write_text(temporary, values.dump(2) + '\n');
        fs::rename(temporary, path);
    } catch (...) {
        std::error_code error;
        fs::remove(temporary, error);
        throw;
    }
}

std::vector<fs::path> directories(fs::path const& path)
{
    std::vector<fs::path> result;
    if (plain_directory(path)) {
        for (auto const& entry : fs::directory_iterator(path)) {
            if (fs::is_directory(entry.symlink_status())) result.push_back(entry.path());
        }
    }
    return result;
}

std::uint64_t file_blocks(fs::path const& path, bool changing)
{
    struct stat info {};
    if (::lstat(path.c_str(), &info) == 0) return static_cast<std::uint64_t>(info.st_blocks) * 512;
    if (changing && (errno == ENOENT || errno == ENOTDIR)) return 0;
    throw std::system_error(errno, std::generic_category(), "Could not inspect " + path.string());
}

std::uint64_t allocated_bytes(fs::path const& path, bool changing)
{
    auto size = file_blocks(path, changing);
    std::error_code error;
    fs::recursive_directory_iterator it(path, error), end;
    while (!error && it != end) {
        size += file_blocks(it->path(), changing);
        it.increment(error);
    }
    if (error && !(changing && (error == std::errc::no_such_file_or_directory
                                || error == std::errc::not_a_directory))) {
        throw fs::filesystem_error("Could not measure build cache entry", path, error);
    }
    return size;
}

std::uint64_t workspace_size(fs::path const& path, fs::path const& locks, bool changing, bool update)
{
    auto const dirty = locks / "dirty";
    auto const checkpoint = locks / "size";
    std::uint64_t size;
    if (!fs::exists(dirty) && (std::ifstream(checkpoint) >> size)) return size;
    size = allocated_bytes(path, changing);
    if (update) {
        auto const next = locks / "size.next";
        write_text(next, std::to_string(size));
        fs::rename(next, checkpoint);
        fs::remove(dirty);
    }
    return size;
}

std::uint64_t available_space(fs::path path)
{
    while (!fs::exists(path) && path.has_relative_path()) path = path.parent_path();
    return fs::space(path).available;
}

bool expired(fs::file_time_type used, std::uint64_t days)
{
    auto const age = std::chrono::duration_cast<std::chrono::seconds>(
        fs::file_time_type::clock::now() - used).count();
    return age >= 0 && static_cast<std::uint64_t>(age) / 86400 >= days;
}

void remove_workspace(fs::path const& path, fs::path const& locks)
{
    fs::remove_all(path);
    fs::remove(locks / "size");
    fs::remove(locks / "dirty");
}

std::string in_gib(std::uint64_t bytes)
{
    std::ostringstream output;
    output << std::fixed << std::setprecision(2) << static_cast<double>(bytes) / gib;
    return output.str();
}

struct Workspace {
    fs::path path;
    fs::path locks;
    fs::file_time_type used;
    std::uint64_t bytes;
    std::size_t decision_index;
};

} // namespace

Lock::Lock(fs::path const& path, LockMode mode, bool create)
{
    if (create) fs::create_directories(path.parent_path());
    fd_ = ::open(path.c_str(), O_CLOEXEC | O_NOFOLLOW | (create ? O_CREAT | O_RDWR : O_RDONLY), 0600);
    if (fd_ < 0) {
        if (!create && errno == ENOENT) {
            acquired_ = true;
            return;
        }
        throw std::system_error(errno, std::generic_category(), "Could not open lock " + path.string());
    }
    auto const flags = mode == LockMode::shared ? LOCK_SH
        : (mode == LockMode::try_exclusive ? LOCK_EX | LOCK_NB : LOCK_EX);
    int result;
    do {
        result = ::flock(fd_, flags);
    } while (result < 0 && errno == EINTR);
    if (result == 0) {
        acquired_ = true;
        return;
    }
    auto const error = errno;
    ::close(fd_);
    fd_ = -1;
    if (mode != LockMode::try_exclusive || error != EWOULDBLOCK) {
        throw std::system_error(error, std::generic_category(), "Could not acquire lock " + path.string());
    }
}

Lock::~Lock()
{
    if (fd_ >= 0) ::close(fd_);
}

fs::path root()
{
    if (auto const* value = std::getenv("IV_BUILD_CACHE_DIR"); value && *value) {
        if (!fs::path(value).is_absolute()) throw std::runtime_error("IV_BUILD_CACHE_DIR must be an absolute path");
        return fs::weakly_canonical(value);
    }
    return fs::weakly_canonical(user_directory("XDG_CACHE_HOME", ".cache") / "intravenous/build-cache");
}

std::string config_table(SizeUnits units)
{
    auto const policy = read_policy();
    std::ostringstream output;
    output << std::left << std::setw(18) << "SETTING" << "VALUE\n";
    for (auto const& setting : settings) {
        output << std::setw(18) << setting.name;
        auto const value = policy.*setting.field;
        if (setting.size) print_size(output, value, units);
        else output << value;
        output << '\n';
    }
    return output.str();
}

void edit_config(std::span<ConfigEdit const> edits)
{
    auto const path = fs::weakly_canonical(config_path());
    Lock const writer(path.string() + ".lock", LockMode::exclusive);
    auto values = edits.empty() ? nlohmann::json::object() : read_config(path);
    for (auto const& edit : edits) {
        auto const& setting = get_setting(edit.key);
        auto const key = std::string(setting.name);
        if (edit.value) {
            values[key] = setting.size ? parse_size(*edit.value, key) : parse_unsigned(*edit.value, key);
        } else {
            values.erase(key);
        }
    }
    parse_policy(values, path);
    write_config(path, values);
}

std::string stable_hash(std::string_view text)
{
    std::uint64_t hash = 1469598103934665603ull;
    for (unsigned char byte : text) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    std::ostringstream result;
    result << std::hex << hash;
    return result.str();
}

void mark_used(fs::path const& locks, std::string_view source)
{
    if (read_line(locks / "source") != source) write_text(locks / "source", source);
    auto const used = locks / "last-used";
    if (!fs::exists(used)) write_text(used, "");
    fs::last_write_time(used, fs::file_time_type::clock::now());
}

void mark_dirty(fs::path const& locks)
{
    write_text(locks / "dirty", "");
}

int run_process(std::span<char* const> argv, std::span<int const> inherited_locks)
{
    posix_spawn_file_actions_t actions;
    auto error = ::posix_spawn_file_actions_init(&actions);
    if (error) throw std::system_error(error, std::generic_category(), "Could not prepare child process");
    for (auto fd : inherited_locks) {
        error = ::posix_spawn_file_actions_adddup2(&actions, fd, fd);
        if (error) break;
    }
    pid_t child{};
    if (!error) error = ::posix_spawnp(&child, argv.front(), &actions, nullptr, argv.data(), environ);
    ::posix_spawn_file_actions_destroy(&actions);
    if (error) throw std::system_error(error, std::generic_category(), "Could not start " + std::string(argv.front()));
    int status;
    while (::waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) throw std::system_error(errno, std::generic_category(), "Could not wait for child process");
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

ScratchDirectory::ScratchDirectory(fs::path const& cache_root)
{
    Lock collector(cache_root / "locks/collect", LockMode::exclusive);
    fs::create_directories(cache_root / "scratch");
    auto pattern = (cache_root / "scratch/run-XXXXXXXX").string();
    if (!::mkdtemp(pattern.data())) {
        throw std::system_error(errno, std::generic_category(), "Could not create cache scratch directory");
    }
    path_ = pattern;
    lease_.emplace(path_ / ".use", LockMode::exclusive);
}

Report sweep(fs::path const& cache_root, Mode mode, Log const& log)
{
    bool const inspect = mode == Mode::status;
    bool const preview = mode == Mode::dry_run;
    bool const erase = !inspect && !preview;
    bool const report_details = inspect || preview || mode == Mode::collect;
    auto const policy = inspect ? Policy{} : read_policy();
    Lock collector(cache_root / "locks/collect", LockMode::exclusive, erase);
    Report report;
    report.free_bytes = available_space(cache_root);
    std::uint64_t reclaimed = 0;
    auto decide = [&](auto& decisions, fs::path const& path, std::string_view reason,
                      bool remove, fs::path const& locks = {}) {
        if (report_details) decisions.push_back({path, locks.empty() ? "" : read_line(locks / "source"),
                                                 std::string(reason), remove});
    };
    for (auto const& scratch : directories(cache_root / "scratch")) {
        if (!scratch.filename().string().starts_with("run-") || !plain_file(scratch / ".use")) continue;
        Lock lease(scratch / ".use", LockMode::try_exclusive, erase);
        if (lease.acquired() && !inspect) {
            if (preview) reclaimed += allocated_bytes(scratch, false);
            else fs::remove_all(scratch);
            decide(report.scratch, scratch, "scratch lease ended", true);
        } else {
            report.remaining_bytes += allocated_bytes(scratch, !lease.acquired());
            decide(report.scratch, scratch, lease.acquired() ? "scratch lease ended" : "scratch is in use", false);
        }
    }

    std::vector<Workspace> candidates;
    for (auto [category, depth] : std::array{
             std::pair{"server", 2}, std::pair{"project-packages", 3}, std::pair{"shared-packages", 2}}) {
        std::vector<fs::path> level{cache_root / category};
        while (depth-- > 0) {
            std::vector<fs::path> next;
            for (auto const& parent : level) {
                for (auto& dir : directories(parent)) next.push_back(std::move(dir));
            }
            level = std::move(next);
        }
        for (auto const& path : level) {
            auto const locks = cache_root / "locks" / path.lexically_relative(cache_root);
            if (!plain_file(locks / "last-used")) continue;
            Lock writer(locks / "write", LockMode::try_exclusive, erase);
            std::string_view reason = "workspace writer lock is held";
            std::uint64_t reclaimed_artifacts = 0;
            if (writer.acquired()) {
                if (std::string_view(category) != "server") {
                    auto const current = read_line(path / ".current-artifact");
                    for (auto const& artifact : directories(path / "out")) {
                        if (artifact.filename() == current && !inspect) {
                            decide(report.artifacts, artifact, "latest successful package build", false);
                            continue;
                        }
                        Lock lease(artifact / ".use", LockMode::try_exclusive, erase);
                        if (lease.acquired() && !inspect) {
                            decide(report.artifacts, artifact, plain_file(artifact / ".complete")
                                ? "superseded package artifact" : "incomplete package artifact", true);
                            if (preview) reclaimed_artifacts += allocated_bytes(artifact, false);
                            else {
                                mark_dirty(locks);
                                fs::remove_all(artifact);
                            }
                        } else {
                            decide(report.artifacts, artifact, !lease.acquired() ? "package artifact is leased"
                                : artifact.filename() == current ? "latest successful package build"
                                : "package artifact is idle", false);
                        }
                    }
                }
                Lock lease(locks / "use", LockMode::try_exclusive, erase);
                if (lease.acquired() && !inspect && expired(fs::last_write_time(locks / "last-used"), policy.idle_days)) {
                    if (preview) reclaimed += std::max(workspace_size(path, locks, false, false), reclaimed_artifacts);
                    else remove_workspace(path, locks);
                    decide(report.workspaces, path, "idle limit reached", true, locks);
                    continue;
                }
                reason = lease.acquired() ? (inspect ? "workspace is idle" : "within idle limit") : "workspace is leased";
            }
            auto size = workspace_size(path, locks, !writer.acquired(), erase && writer.acquired());
            reclaimed += reclaimed_artifacts;
            size -= std::min(size, reclaimed_artifacts);
            report.remaining_bytes += size;
            auto const index = report.workspaces.size();
            decide(report.workspaces, path, reason, false, locks);
            candidates.push_back({path, locks, fs::last_write_time(locks / "last-used"), size, index});
        }
    }

    report.bytes = report.remaining_bytes + reclaimed;
    auto free_now = [&] { return preview ? report.free_bytes + reclaimed : available_space(cache_root); };
    auto free = free_now();
    if (!inspect && (report.remaining_bytes > policy.threshold || free < policy.minimum_free)) {
        std::ranges::sort(candidates, [](Workspace const& a, Workspace const& b) {
            return std::tie(a.used, a.path) < std::tie(b.used, b.path);
        });
        for (auto const& candidate : candidates) {
            if (report.remaining_bytes <= policy.target && free >= policy.minimum_free) break;
            auto const& [path, locks, used, size, index] = candidate;
            auto revise_decision = [&](std::string_view reason, bool remove = false) {
                if (report_details) {
                    report.workspaces[index].reason = reason;
                    report.workspaces[index].remove = remove;
                }
            };
            Lock writer(locks / "write", LockMode::try_exclusive, erase);
            if (!writer.acquired()) {
                revise_decision("workspace writer lock is held");
                continue;
            }
            Lock lease(locks / "use", LockMode::try_exclusive, erase);
            if (!lease.acquired()) {
                revise_decision("workspace is leased");
                continue;
            }
            if (fs::last_write_time(locks / "last-used") != used) {
                revise_decision("used again during this sweep");
                continue;
            }
            if (preview) reclaimed += size;
            else remove_workspace(path, locks);
            report.remaining_bytes -= size;
            free = free_now();
            revise_decision("least recently used under storage pressure", true);
            if (log && !report_details) {
                log("Build cache evicted " + read_line(locks / "source") + " (" + path.string() + ")");
            }
        }
    }
    report.predicted_free_bytes = free;
    if (!preview) report.free_bytes = free;
    if (report_details) {
        std::unordered_set<fs::path> removed_workspaces;
        for (auto const& workspace : report.workspaces) {
            if (workspace.remove) removed_workspaces.insert(workspace.path);
        }
        std::erase_if(report.artifacts, [&](Decision const& artifact) {
            return removed_workspaces.contains(artifact.path.parent_path().parent_path());
        });
        auto by_path = [](Decision const& left, Decision const& right) { return left.path < right.path; };
        std::ranges::sort(report.workspaces, by_path);
        std::ranges::sort(report.artifacts, by_path);
        std::ranges::sort(report.scratch, by_path);
    }
    if (!inspect && free < policy.minimum_free) {
        auto const message = "Build cache filesystem has " + in_gib(free)
            + " GiB free, below the configured floor of " + std::to_string(policy.minimum_free) + " bytes";
        if (mode == Mode::before_build) throw std::runtime_error(message);
        if (log) log(message);
    }
    if (!inspect && report.remaining_bytes > policy.threshold && log) {
        log("Build cache is above its threshold (" + in_gib(report.remaining_bytes) + " GiB). Entries in use were kept.");
    }
    return report;
}

} // namespace iv::build_cache
