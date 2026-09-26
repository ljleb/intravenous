#define IV_INTERNAL_TRANSLATION_UNIT

#include <intravenous/module/watcher.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <utility>

#include <sys/inotify.h>
#include <unistd.h>

namespace iv {
    namespace {
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
    }

    DependencyWatcher::DependencyWatcher()
        : _fd(open_inotify())
    {}

    bool DependencyWatcher::consume_events()
    {
        std::array<char, 16 * 1024> buffer {};
        bool saw_event = false;
        for (;;) {
            auto const count = read(_fd.get(), buffer.data(), buffer.size());
            if (count > 0) {
                saw_event = true;
                continue;
            }
            if (count == -1 && errno == EINTR) {
                continue;
            }
            if (count == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return saw_event;
            }
            if (count == 0) {
                return saw_event;
            }
            throw_errno("read(dependency watcher inotify)");
        }
    }

    void DependencyWatcher::clear_watches()
    {
        for (auto const descriptor : _watch_descriptors) {
            if (inotify_rm_watch(_fd.get(), descriptor) == -1 && errno != EINVAL) {
                throw_errno("inotify_rm_watch(dependency watcher)");
            }
        }
        _watch_descriptors.clear();
        (void)consume_events();
    }

    void DependencyWatcher::add_directory_recursive(std::filesystem::path const& dir)
    {
        std::error_code ec;
        if (!std::filesystem::exists(dir, ec) || ec) {
            return;
        }

        constexpr std::uint32_t mask =
            IN_ATTRIB | IN_CLOSE_WRITE | IN_CREATE | IN_DELETE | IN_DELETE_SELF
            | IN_MOVE_SELF | IN_MOVED_FROM | IN_MOVED_TO;
        auto const root_descriptor = inotify_add_watch(
            _fd.get(),
            dir.string().c_str(),
            mask | IN_ONLYDIR);
        if (root_descriptor == -1) {
            throw_errno("inotify_add_watch(dependency watcher)");
        }
        _watch_descriptors.insert(root_descriptor);

        auto const options = std::filesystem::directory_options::skip_permission_denied;
        for (std::filesystem::recursive_directory_iterator it(dir, options, ec), end;
             it != end;
             it.increment(ec)) {
            if (ec) {
                return;
            }
            auto const& entry = *it;
            if (!entry.is_directory()) {
                continue;
            }
            if (is_module_dependency_ignored_directory(entry.path())) {
                it.disable_recursion_pending();
                continue;
            }
            auto const descriptor = inotify_add_watch(
                _fd.get(),
                entry.path().string().c_str(),
                mask | IN_ONLYDIR);
            if (descriptor == -1) {
                throw_errno("inotify_add_watch(dependency watcher)");
            }
            _watch_descriptors.insert(descriptor);
        }
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

    void DependencyWatcher::update(std::vector<ModuleDependency> dependencies)
    {
        _dependencies = std::move(dependencies);
        clear_watches();

        for (auto const& dependency : _dependencies) {
            add_directory_recursive(dependency.module_dir);
        }

        // Close the stamp-to-watch installation race once per watch-set update.
        // Steady-state scans only happen after an inotify event.
        _rescan_pending = true;
    }

    std::vector<ModuleDependency> DependencyWatcher::changed_dependencies()
    {
        if (_rescan_pending) {
            _rescan_pending = false;
            return scan_changed_dependencies();
        }
        if (!consume_events()) {
            return {};
        }
        return scan_changed_dependencies();
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
