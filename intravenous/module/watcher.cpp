#define IV_INTERNAL_TRANSLATION_UNIT

#include "module/watcher.h"

#include <algorithm>
#include <array>

#if defined(__linux__)
#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>
#endif

namespace iv {
    namespace {
        std::filesystem::file_time_type compute_directory_stamp(std::filesystem::path const& dir)
        {
            std::filesystem::file_time_type latest {};
            bool saw_file = false;

            for (auto const& entry : std::filesystem::recursive_directory_iterator(dir)) {
                if (!entry.is_regular_file()) {
                    continue;
                }

                std::error_code ec;
                auto stamp = std::filesystem::last_write_time(entry.path(), ec);
                if (ec) {
                    continue;
                }

                latest = saw_file ? std::max(latest, stamp) : stamp;
                saw_file = true;
            }

            if (!saw_file) {
                std::error_code ec;
                auto stamp = std::filesystem::last_write_time(dir, ec);
                return ec ? std::filesystem::file_time_type {} : stamp;
            }

            return latest;
        }

        class PollingWatcher final : public DependencyWatcher {
            std::vector<ModuleDependency> _dependencies;

        public:
            void update(std::vector<ModuleDependency> dependencies) override
            {
                _dependencies = std::move(dependencies);
            }

            bool has_changes() override
            {
                for (auto const& dependency : _dependencies) {
                    if (compute_directory_stamp(dependency.module_dir) != dependency.source_stamp) {
                        return true;
                    }
                }

                return false;
            }
        };

#if defined(__linux__)
        class InotifyWatcher final : public DependencyWatcher {
            int _fd = -1;
            std::vector<ModuleDependency> _dependencies;

            void reset()
            {
                if (_fd != -1) {
                    close(_fd);
                    _fd = -1;
                }
            }

            void add_directory_recursive(std::filesystem::path const& dir)
            {
                (void)inotify_add_watch(_fd, dir.string().c_str(), IN_CLOSE_WRITE | IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO);

                for (auto const& entry : std::filesystem::recursive_directory_iterator(dir)) {
                    if (entry.is_directory()) {
                        (void)inotify_add_watch(_fd, entry.path().string().c_str(), IN_CLOSE_WRITE | IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO);
                    }
                }
            }

        public:
            InotifyWatcher()
            {
                _fd = inotify_init1(IN_NONBLOCK);
            }

            ~InotifyWatcher() override
            {
                reset();
            }

            void update(std::vector<ModuleDependency> dependencies) override
            {
                _dependencies = std::move(dependencies);
                reset();
                _fd = inotify_init1(IN_NONBLOCK);
                if (_fd == -1) {
                    return;
                }

                for (auto const& dependency : _dependencies) {
                    add_directory_recursive(dependency.module_dir);
                }
            }

            bool has_changes() override
            {
                if (_fd == -1) {
                    return false;
                }

                pollfd fd { .fd = _fd, .events = POLLIN, .revents = 0 };
                if (poll(&fd, 1, 0) <= 0) {
                    return false;
                }

                std::array<char, 4096> buffer {};
                return read(_fd, buffer.data(), buffer.size()) > 0;
            }
        };
#endif
    }

    std::unique_ptr<DependencyWatcher> make_dependency_watcher()
    {
#if defined(__linux__)
        return std::make_unique<InotifyWatcher>();
#else
        return std::make_unique<PollingWatcher>();
#endif
    }
}
