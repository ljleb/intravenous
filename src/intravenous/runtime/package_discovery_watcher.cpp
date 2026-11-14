#include <intravenous/runtime/package_discovery_watcher.h>

#include <intravenous/module/dependency.h>
#include <intravenous/module/package_manifest.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <utility>

#include <sys/inotify.h>
#include <unistd.h>

namespace iv {
namespace {
constexpr std::uint32_t discovery_watch_mask =
    IN_ATTRIB | IN_CLOSE_WRITE | IN_CREATE | IN_DELETE | IN_DELETE_SELF
    | IN_MOVE_SELF | IN_MOVED_FROM | IN_MOVED_TO;

UniqueFileDescriptor open_inotify()
{
    auto const fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd == -1) throw_errno("inotify_init1(package discovery)");
    return UniqueFileDescriptor(fd);
}

std::string watch_key(std::filesystem::path const& path)
{
    std::error_code error;
    auto normalized = std::filesystem::weakly_canonical(path, error);
    if (error) {
        error.clear();
        normalized = std::filesystem::absolute(path, error);
    }
    return (error ? path : normalized).lexically_normal().generic_string();
}

bool is_manifest_name(std::string_view name)
{
    return is_iv_package_manifest_file(std::string(name));
}
} // namespace

PackageDiscoveryWatcher::PackageDiscoveryWatcher(std::vector<std::filesystem::path> roots)
    : fd_(open_inotify())
    , roots_(std::move(roots))
{
    refresh_coverage();
}

void PackageDiscoveryWatcher::ensure_watch(
    std::filesystem::path const& directory,
    std::unordered_set<std::string>& desired_paths)
{
    auto const key = watch_key(directory);
    desired_paths.insert(key);
    if (watch_by_path_.contains(key)) return;

    auto const descriptor =
        inotify_add_watch(fd_.get(), directory.c_str(), discovery_watch_mask | IN_ONLYDIR);
    if (descriptor == -1) {
        // Topology can change between traversal and watch installation. The
        // already-watched parent will report that mutation, so retry on the
        // next coverage pass instead of turning a benign race into a failure.
        if (errno == ENOENT || errno == ENOTDIR) return;
        throw_errno("inotify_add_watch(package discovery)");
    }

    if (auto const previous = path_by_watch_.find(descriptor);
        previous != path_by_watch_.end() && previous->second != key) {
        watch_by_path_.erase(previous->second);
    }
    watch_by_path_.insert_or_assign(key, descriptor);
    path_by_watch_.insert_or_assign(descriptor, key);
}

void PackageDiscoveryWatcher::add_root_coverage(
    std::filesystem::path const& root,
    std::unordered_set<std::string>& desired_paths)
{
    std::error_code error;
    auto const root_is_directory = std::filesystem::is_directory(root, error) && !error;
    if (!root_is_directory) {
        // inotify cannot watch a missing configured root. Keep exactly one
        // nearest existing ancestor so creation/replacement of the root wakes
        // the service, then migrate coverage downward as directories appear.
        auto ancestor = root.parent_path();
        while (!ancestor.empty()) {
            error.clear();
            if (std::filesystem::is_directory(ancestor, error) && !error) {
                ensure_watch(ancestor, desired_paths);
                return;
            }
            auto parent = ancestor.parent_path();
            if (parent == ancestor) break;
            ancestor = std::move(parent);
        }
        return;
    }

    // Establish the root watch before traversal so any directory created while
    // traversal runs is represented either in the traversal or in the queue.
    ensure_watch(root, desired_paths);

    auto const options = std::filesystem::directory_options::skip_permission_denied;
    for (std::filesystem::recursive_directory_iterator it(root, options, error), end;
         !error && it != end;
         it.increment(error)) {
        auto const& entry = *it;
        if (!entry.is_directory(error) || error) {
            error.clear();
            continue;
        }

        if (is_package_tree_ignored_directory(entry.path())) {
            it.disable_recursion_pending();
            continue;
        }
        ensure_watch(entry.path(), desired_paths);
    }
}

void PackageDiscoveryWatcher::remove_stale_watches(
    std::unordered_set<std::string> const& desired_paths)
{
    for (auto it = watch_by_path_.begin(); it != watch_by_path_.end();) {
        if (desired_paths.contains(it->first)) {
            ++it;
            continue;
        }

        auto const descriptor = it->second;
        path_by_watch_.erase(descriptor);
        it = watch_by_path_.erase(it);
        if (inotify_rm_watch(fd_.get(), descriptor) == -1) {
            if (errno != EINVAL) throw_errno("inotify_rm_watch(package discovery)");
        } else {
            // inotify queues IN_IGNORED for an explicit removal. Remember the
            // descriptor generation so a later descriptor reuse cannot make
            // that stale event erase a newly installed watch.
            retired_watch_descriptors_.insert(descriptor);
        }
    }
}

void PackageDiscoveryWatcher::forget_watch(int descriptor) noexcept
{
    auto const path = path_by_watch_.find(descriptor);
    if (path == path_by_watch_.end()) return;
    watch_by_path_.erase(path->second);
    path_by_watch_.erase(path);
}

void PackageDiscoveryWatcher::refresh_coverage()
{
    std::unordered_set<std::string> desired_paths;
    for (auto const& root : roots_) add_root_coverage(root, desired_paths);
    remove_stale_watches(desired_paths);
}

PackageDiscoveryEvents PackageDiscoveryWatcher::consume_events()
{
    PackageDiscoveryEvents result;
    std::array<char, 16 * 1024> buffer{};

    for (;;) {
        auto const count = read(fd_.get(), buffer.data(), buffer.size());
        if (count > 0) {
            std::size_t offset = 0;
            while (offset < static_cast<std::size_t>(count)) {
                auto const* event =
                    reinterpret_cast<inotify_event const*>(buffer.data() + offset);
                offset += sizeof(inotify_event) + event->len;

                if ((event->mask & IN_Q_OVERFLOW) != 0) {
                    result.coverage_may_have_changed = true;
                    result.declarations_may_have_changed = true;
                    continue;
                }

                auto const watched = path_by_watch_.contains(event->wd);
                if ((event->mask & IN_IGNORED) != 0) {
                    if (auto retired = retired_watch_descriptors_.find(event->wd);
                        retired != retired_watch_descriptors_.end()) {
                        retired_watch_descriptors_.erase(retired);
                        continue;
                    }
                    if (watched) {
                        forget_watch(event->wd);
                        result.coverage_may_have_changed = true;
                        result.declarations_may_have_changed = true;
                    }
                    continue;
                }
                if (!watched) continue;

                if ((event->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT)) != 0) {
                    result.coverage_may_have_changed = true;
                    result.declarations_may_have_changed = true;
                    continue;
                }

                auto const topology_mask =
                    event->mask & (IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO);
                if ((event->mask & IN_ISDIR) != 0) {
                    if (topology_mask != 0) {
                        result.coverage_may_have_changed = true;
                        result.declarations_may_have_changed = true;
                    }
                    continue;
                }

                std::string_view name;
                if (event->len != 0) {
                    name = std::string_view(
                        event->name,
                        strnlen(event->name, event->len));
                }

                // File creation/removal/moves can change whether a manifest's
                // relative entry file exists. Writes/attribute changes only
                // affect discovery when the file itself is a package manifest.
                if (topology_mask != 0
                    || (is_manifest_name(name)
                        && (event->mask & (IN_CLOSE_WRITE | IN_ATTRIB)) != 0)) {
                    result.declarations_may_have_changed = true;
                }
            }
            continue;
        }
        if (count == -1 && errno == EINTR) continue;
        if (count == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) return result;
        if (count == 0) return result;
        throw_errno("read(package discovery inotify)");
    }
}
} // namespace iv
