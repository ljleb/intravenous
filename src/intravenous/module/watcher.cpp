#define IV_INTERNAL_TRANSLATION_UNIT

#include <intravenous/module/watcher.h>

#include <algorithm>
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
        constexpr std::uint32_t dependency_watch_mask =
            IN_ATTRIB | IN_CLOSE_WRITE | IN_CREATE | IN_DELETE | IN_DELETE_SELF
            | IN_MOVE_SELF | IN_MOVED_FROM | IN_MOVED_TO;

        std::filesystem::file_time_type compute_directory_stamp(std::filesystem::path const& dir)
        {
            std::error_code ec;
            if (!std::filesystem::exists(dir, ec) || ec) {
                return {};
            }

            std::filesystem::file_time_type latest {};
            bool saw_file = false;

            auto const options = std::filesystem::directory_options::skip_permission_denied;
            for (std::filesystem::recursive_directory_iterator it(dir, options, ec), end;
                 it != end;
                 it.increment(ec)) {
                if (ec) {
                    return {};
                }
                auto const& entry = *it;
                if (entry.is_directory()) {
                    if (is_module_dependency_ignored_directory(entry.path())) {
                        it.disable_recursion_pending();
                    }
                    continue;
                }
                if (!entry.is_regular_file()) {
                    continue;
                }
                if (!is_module_dependency_package_path(entry.path())) {
                    continue;
                }

                std::error_code stamp_ec;
                auto stamp = std::filesystem::last_write_time(entry.path(), stamp_ec);
                if (stamp_ec) {
                    continue;
                }

                latest = saw_file ? std::max(latest, stamp) : stamp;
                saw_file = true;
            }

            if (!saw_file) {
                std::error_code stamp_ec;
                auto stamp = std::filesystem::last_write_time(dir, stamp_ec);
                return stamp_ec ? std::filesystem::file_time_type {} : stamp;
            }

            return latest;
        }

        UniqueFileDescriptor open_inotify()
        {
            auto const fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
            if (fd == -1) throw_errno("inotify_init1(dependency watcher)");
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

        void append_unique_dependency(
            std::vector<ModuleDependency>& result,
            ModuleDependency const& dependency)
        {
            auto const duplicate = std::ranges::any_of(result, [&](ModuleDependency const& current) {
                return current.id == dependency.id && current.module_dir == dependency.module_dir;
            });
            if (!duplicate) result.push_back(dependency);
        }
    }

    DependencyWatcher::DependencyWatcher()
        : _fd(open_inotify())
    {}

    void DependencyWatcher::clear_watches()
    {
        for (auto const& [_, watch] : _watch_by_path) {
            if (inotify_rm_watch(_fd.get(), watch.descriptor) == -1 && errno != EINVAL) {
                throw_errno("inotify_rm_watch(dependency watcher)");
            }
        }
        _watch_by_path.clear();
        _path_by_watch.clear();
        _retired_watch_descriptors.clear();

        // Explicit removals queue IN_IGNORED. update() intentionally replaces
        // the entire association set, so discard those old-generation events
        // before installing the new watches on the stable inotify descriptor.
        std::array<char, 16 * 1024> buffer {};
        for (;;) {
            auto const count = read(_fd.get(), buffer.data(), buffer.size());
            if (count > 0) continue;
            if (count == -1 && errno == EINTR) continue;
            if (count == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
            if (count == 0) return;
            throw_errno("read(dependency watcher inotify)");
        }
    }

    void DependencyWatcher::ensure_watch(
        std::filesystem::path const& directory,
        std::size_t dependency_index,
        std::unordered_map<std::string, std::unordered_set<std::size_t>>& desired_paths)
    {
        auto const key = watch_key(directory);
        desired_paths[key].insert(dependency_index);

        if (auto existing = _watch_by_path.find(key); existing != _watch_by_path.end()) {
            existing->second.dependency_indices.insert(dependency_index);
            return;
        }

        auto const descriptor = inotify_add_watch(
            _fd.get(), directory.c_str(), dependency_watch_mask | IN_ONLYDIR);
        if (descriptor == -1) {
            // The watched parent reports directory topology changes. A child
            // can disappear between traversal and installation without making
            // the watcher unhealthy; the next coverage reconciliation retries.
            if (errno == ENOENT || errno == ENOTDIR) return;
            throw_errno("inotify_add_watch(dependency watcher)");
        }

        if (auto previous = _path_by_watch.find(descriptor);
            previous != _path_by_watch.end() && previous->second != key) {
            _watch_by_path.erase(previous->second);
        }

        Watch watch{
            .descriptor = descriptor,
            .dependency_indices = {dependency_index},
        };
        _watch_by_path.insert_or_assign(key, std::move(watch));
        _path_by_watch.insert_or_assign(descriptor, key);
    }

    void DependencyWatcher::add_dependency_coverage(
        std::size_t dependency_index,
        std::unordered_map<std::string, std::unordered_set<std::size_t>>& desired_paths)
    {
        auto const& root = _dependencies[dependency_index].module_dir;
        std::error_code error;
        if (!std::filesystem::is_directory(root, error) || error) return;

        // Establish the root watch before traversal. A directory created while
        // traversal runs is therefore either traversed or represented by an
        // event that requests another coverage pass.
        ensure_watch(root, dependency_index, desired_paths);

        auto const options = std::filesystem::directory_options::skip_permission_denied;
        for (std::filesystem::recursive_directory_iterator it(root, options, error), end;
             !error && it != end;
             it.increment(error)) {
            auto const& entry = *it;
            if (!entry.is_directory(error) || error) {
                error.clear();
                continue;
            }
            if (is_module_dependency_ignored_directory(entry.path())) {
                it.disable_recursion_pending();
                continue;
            }
            ensure_watch(entry.path(), dependency_index, desired_paths);
        }
    }

    void DependencyWatcher::remove_stale_watches(
        std::unordered_map<std::string, std::unordered_set<std::size_t>> const& desired_paths)
    {
        for (auto it = _watch_by_path.begin(); it != _watch_by_path.end();) {
            if (auto desired = desired_paths.find(it->first); desired != desired_paths.end()) {
                it->second.dependency_indices = desired->second;
                ++it;
                continue;
            }

            auto const descriptor = it->second.descriptor;
            _path_by_watch.erase(descriptor);
            it = _watch_by_path.erase(it);
            if (inotify_rm_watch(_fd.get(), descriptor) == -1) {
                if (errno != EINVAL) throw_errno("inotify_rm_watch(dependency watcher)");
            } else {
                // Explicit removal queues IN_IGNORED. Preserve its generation
                // so descriptor reuse cannot make that stale event erase a new
                // watch installed by a later reconciliation.
                _retired_watch_descriptors.insert(descriptor);
            }
        }
    }

    void DependencyWatcher::forget_watch(int descriptor) noexcept
    {
        auto const path = _path_by_watch.find(descriptor);
        if (path == _path_by_watch.end()) return;
        _watch_by_path.erase(path->second);
        _path_by_watch.erase(path);
    }

    void DependencyWatcher::refresh_coverage()
    {
        std::unordered_map<std::string, std::unordered_set<std::size_t>> desired_paths;
        for (std::size_t index = 0; index < _dependencies.size(); ++index) {
            add_dependency_coverage(index, desired_paths);
        }
        remove_stale_watches(desired_paths);
    }

    std::vector<ModuleDependency> DependencyWatcher::scan_changed_dependencies() const
    {
        std::vector<ModuleDependency> result;
        for (auto const& dependency : _dependencies) {
            if (compute_directory_stamp(dependency.module_dir) != dependency.package_stamp) {
                result.push_back(dependency);
            }
        }
        return result;
    }

    std::vector<ModuleDependency> DependencyWatcher::consume_events()
    {
        std::vector<ModuleDependency> result;
        std::array<char, 16 * 1024> buffer {};
        bool coverage_may_have_changed = false;

        auto mark_watch_dependencies = [&](int descriptor) {
            auto const path = _path_by_watch.find(descriptor);
            if (path == _path_by_watch.end()) return;
            auto const watch = _watch_by_path.find(path->second);
            if (watch == _watch_by_path.end()) return;
            for (auto const index : watch->second.dependency_indices) {
                if (index < _dependencies.size()) {
                    append_unique_dependency(result, _dependencies[index]);
                }
            }
        };

        auto mark_all_dependencies = [&] {
            for (auto const& dependency : _dependencies) {
                append_unique_dependency(result, dependency);
            }
        };

        for (;;) {
            auto const count = read(_fd.get(), buffer.data(), buffer.size());
            if (count > 0) {
                std::size_t offset = 0;
                while (offset < static_cast<std::size_t>(count)) {
                    auto const* event =
                        reinterpret_cast<inotify_event const*>(buffer.data() + offset);
                    offset += sizeof(inotify_event) + event->len;

                    if ((event->mask & IN_Q_OVERFLOW) != 0) {
                        // Event loss destroys precise attribution. Conservatively
                        // rebuild coverage and dirty every dependency rather than
                        // allow a silent stale graph.
                        coverage_may_have_changed = true;
                        mark_all_dependencies();
                        continue;
                    }

                    auto const watched = _path_by_watch.contains(event->wd);
                    if ((event->mask & IN_IGNORED) != 0) {
                        if (auto retired = _retired_watch_descriptors.find(event->wd);
                            retired != _retired_watch_descriptors.end()) {
                            _retired_watch_descriptors.erase(retired);
                            continue;
                        }
                        if (watched) {
                            mark_watch_dependencies(event->wd);
                            forget_watch(event->wd);
                            coverage_may_have_changed = true;
                        }
                        continue;
                    }
                    if (!watched) continue;

                    if ((event->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT)) != 0) {
                        mark_watch_dependencies(event->wd);
                        coverage_may_have_changed = true;
                        continue;
                    }

                    std::string_view name;
                    if (event->len != 0) {
                        name = std::string_view(event->name, strnlen(event->name, event->len));
                    }

                    auto const topology_mask =
                        event->mask & (IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO);
                    if ((event->mask & IN_ISDIR) != 0) {
                        if (name.empty()
                            || is_module_dependency_ignored_directory(std::filesystem::path(name))) {
                            continue;
                        }
                        if (topology_mask != 0) {
                            // A directory topology event can hide arbitrarily
                            // many file mutations before its recursive watch is
                            // installed. Treat the owning dependency as dirty as
                            // well as reconciling coverage.
                            mark_watch_dependencies(event->wd);
                            coverage_may_have_changed = true;
                        }
                        continue;
                    }

                    if (name.empty()
                        || !is_module_dependency_package_path(std::filesystem::path(name))) {
                        continue;
                    }

                    if (topology_mask != 0
                        || (event->mask & (IN_CLOSE_WRITE | IN_ATTRIB)) != 0) {
                        // A concrete relevant inotify event is authoritative.
                        // Do not veto it with the lossy max-mtime package stamp.
                        mark_watch_dependencies(event->wd);
                    }
                }
                continue;
            }
            if (count == -1 && errno == EINTR) continue;
            if (count == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            if (count == 0) break;
            throw_errno("read(dependency watcher inotify)");
        }

        if (coverage_may_have_changed) refresh_coverage();
        return result;
    }

    void DependencyWatcher::update(std::vector<ModuleDependency> dependencies)
    {
        _dependencies = std::move(dependencies);
        clear_watches();
        refresh_coverage();

        // This scan exists only to close the baseline-to-watch installation
        // race when replacing a watch set. Steady-state changes are attributed
        // from concrete inotify events instead of being filtered through the
        // lossy maximum-mtime stamp.
        _rescan_pending = true;
    }

    std::vector<ModuleDependency> DependencyWatcher::changed_dependencies()
    {
        std::vector<ModuleDependency> result;
        if (_rescan_pending) {
            _rescan_pending = false;
            result = scan_changed_dependencies();
        }

        // Consume queued events even when a post-install rescan was pending.
        // Otherwise a real event can remain stranded until an unrelated future
        // wake if the rescan itself finds no timestamp difference.
        for (auto const& dependency : consume_events()) {
            append_unique_dependency(result, dependency);
        }
        return result;
    }

    bool DependencyWatcher::has_changes()
    {
        return !changed_dependencies().empty();
    }

    DependencyWatcher make_dependency_watcher()
    {
        return DependencyWatcher();
    }
}
