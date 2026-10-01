#include <intravenous/runtime/package_watcher_service.h>

#include <intravenous/runtime/package_watcher.h>
#include <intravenous/runtime/runtime_project_events.h>

#include <array>
#include <cerrno>
#include <exception>
#include <stdexcept>
#include <utility>

#include <poll.h>

namespace iv {
namespace {
void publish_package_catalog_changed()
{
    IV_INVOKE_LINKER_EVENT_SOURCE(
        iv_runtime_iv_package_catalog_changed_event,
        IvPackageCatalogChanged{});
}

std::vector<std::filesystem::path> discovery_roots(
    std::filesystem::path const& project_root,
    std::vector<std::filesystem::path> const& shared_roots)
{
    std::vector<std::filesystem::path> roots;
    roots.reserve(shared_roots.size() + 1);
    roots.push_back(project_root);
    roots.insert(roots.end(), shared_roots.begin(), shared_roots.end());
    return roots;
}

void wait_for_event(std::array<pollfd, 4>& descriptors)
{
    for (;;) {
        auto const result = poll(descriptors.data(), descriptors.size(), -1);
        if (result > 0) return;
        if (result == -1 && errno == EINTR) continue;
        if (result == -1) throw_errno("poll(package watcher service)");
    }
}

void require_healthy_descriptor(pollfd const& descriptor, char const* name)
{
    if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        throw std::runtime_error(std::string(name) + " descriptor failed");
    }
}
} // namespace

PackageWatcherService::PackageWatcherService(
    PackageWatcher& watcher,
    std::filesystem::path project_root,
    std::vector<std::filesystem::path> shared_roots)
    : watcher_(&watcher)
    , project_root_(std::move(project_root))
    , shared_roots_(std::move(shared_roots))
    , discovery_watcher_(discovery_roots(project_root_, shared_roots_))
{}

PackageWatcherService::~PackageWatcherService()
{
    request_shutdown();
    worker_.reset();
}

void PackageWatcherService::report_discovery_error(std::string message)
{
    if (last_discovery_error_.has_value() && *last_discovery_error_ == message) return;
    last_discovery_error_ = message;
    IV_INVOKE_LINKER_EVENT_SOURCE(
        iv_runtime_project_notification_event,
        ProjectNotification(ProjectStatusNotification{
            .level = "error",
            .code = "packageDiscoveryFailed",
            .message = "IV package discovery failed: " + std::move(message),
        }));
}

void PackageWatcherService::report_service_error(std::string message)
{
    if (last_service_error_.has_value() && *last_service_error_ == message) return;
    last_service_error_ = message;
    IV_INVOKE_LINKER_EVENT_SOURCE(
        iv_runtime_project_notification_event,
        ProjectNotification(ProjectStatusNotification{
            .level = "error",
            .code = "packageWatcherServiceFailed",
            .message = "IV package watcher service failed: " + std::move(message),
        }));
}

bool PackageWatcherService::synchronize_discovered_packages()
{
    try {
        auto declarations = discover_iv_package_declarations(project_root_, shared_roots_);
        last_discovery_error_.reset();
        if (last_package_declarations_.has_value()
            && declarations == *last_package_declarations_) {
            return false;
        }
        last_package_declarations_ = declarations;
        watcher_->synchronize_discovered_packages(std::move(declarations));
        return true;
    } catch (std::exception const& error) {
        report_discovery_error(error.what());
        return false;
    }
}

void PackageWatcherService::process_pending_refresh()
{
    for (;;) {
        watcher_->poll_dependency_changes();
        if (!watcher_->has_pending_refresh()) return;

        PackageWatcherRefreshRequest request;
        IV_INVOKE_LINKER_EVENT_SOURCE(
            iv_runtime_package_watcher_refresh_requested_event,
            request);
        if (request.changed) publish_package_catalog_changed();
    }
}

void PackageWatcherService::start()
{
    if (worker_.has_value()) return;
    worker_.emplace([this](std::stop_token stop_token) {
        try {
            (void)synchronize_discovered_packages();
            process_pending_refresh();

            while (!stop_token.stop_requested()) {
                std::array<pollfd, 4> descriptors{{
                    pollfd{
                        .fd = discovery_watcher_.native_handle(),
                        .events = POLLIN,
                        .revents = 0,
                    },
                    pollfd{
                        .fd = watcher_->dependency_watch_descriptor(),
                        .events = POLLIN,
                        .revents = 0,
                    },
                    pollfd{
                        .fd = work_signal_.native_handle(),
                        .events = POLLIN,
                        .revents = 0,
                    },
                    pollfd{
                        .fd = shutdown_signal_.native_handle(),
                        .events = POLLIN,
                        .revents = 0,
                    },
                }};
                wait_for_event(descriptors);

                if (descriptors[3].revents != 0) {
                    shutdown_signal_.consume();
                    return;
                }
                require_healthy_descriptor(descriptors[0], "package discovery inotify");
                require_healthy_descriptor(descriptors[1], "package dependency inotify");
                require_healthy_descriptor(descriptors[2], "package watcher work");

                if ((descriptors[0].revents & POLLIN) != 0) {
                    auto const events = discovery_watcher_.consume_events();
                    if (events.coverage_may_have_changed) {
                        // Establish watches before rescanning. Any package tree
                        // mutation after coverage installation is then either
                        // visible to this scan or queued for the next wakeup.
                        discovery_watcher_.refresh_coverage();
                    }
                    if (events.declarations_may_have_changed) {
                        (void)synchronize_discovered_packages();
                    }
                }
                if ((descriptors[1].revents & POLLIN) != 0) {
                    watcher_->poll_dependency_changes();
                }
                if ((descriptors[2].revents & POLLIN) != 0) {
                    work_signal_.consume();
                }

                process_pending_refresh();
            }
        } catch (std::exception const& error) {
            report_service_error(error.what());
        }
    });
}

void PackageWatcherService::request_shutdown()
{
    if (!worker_.has_value()) return;
    worker_->request_stop();
    shutdown_signal_.signal();
}

void PackageWatcherService::handle_work_available(PackageWatcherWorkAvailable const&) noexcept
{
    work_signal_.signal();
}
} // namespace iv
