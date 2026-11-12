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
// rebuildStarted is its own cause, before the refresh cause and its build: it
// reports progress at a different moment than the refresh's one outcome report
// (PackageRefreshPublication, sent by PackageWatcher).
void publish_build_started(PackageWatcherRefreshPlan const& plan)
{
    IV_INVOKE_LINKER_EVENT_SOURCE(
        iv_runtime_project_notification_event,
        ProjectNotification(ProjectStatusNotification{
            .level = "info",
            .code = "rebuildStarted",
            .message = plan.build_count == 1
                ? "Building IV package"
                : "Building " + std::to_string(plan.build_count) + " IV packages",
            .package_root = plan.single_package_root,
        }));
}

std::vector<std::filesystem::path> discovery_roots(
    std::filesystem::path const& project_root,
    std::vector<PackageDiscoveryRoot> const& package_roots)
{
    std::vector<std::filesystem::path> roots;
    roots.reserve(package_roots.size() + 1);
    roots.push_back(project_root);
    for (auto const& root : package_roots) roots.push_back(root.root);
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
    std::vector<PackageDiscoveryRoot> package_roots)
    : watcher_(&watcher)
    , project_root_(std::move(project_root))
    , package_roots_(std::move(package_roots))
    , discovery_watcher_(discovery_roots(project_root_, package_roots_))
{}

PackageWatcherService::~PackageWatcherService()
{
    request_shutdown();
    worker_.reset();
}

void PackageWatcherService::report_discovery_error(std::string message)
{
    {
        std::scoped_lock lock(failure_mutex_);
        if (last_discovery_error_.has_value() && *last_discovery_error_ == message) {
            return;
        }
        last_discovery_error_ = message;
    }
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
    {
        std::scoped_lock lock(failure_mutex_);
        if (last_service_error_.has_value() && *last_service_error_ == message) {
            return;
        }
        last_service_error_ = message;
    }
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
        auto declarations = discover_iv_package_declarations(project_root_, package_roots_);
        {
            std::scoped_lock lock(failure_mutex_);
            last_discovery_error_.reset();
        }
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
        auto const plan = watcher_->prepare_refresh();
        if (!plan) return;
        if (plan->build_count > 0) publish_build_started(*plan);

        PackageWatcherRefreshRequest request;
        IV_INVOKE_LINKER_EVENT_SOURCE(
            iv_runtime_package_watcher_refresh_requested_event,
            request);
        {
            std::scoped_lock lock(failure_mutex_);
            if (request.changed) {
                last_refresh_failures_ = request.failed_builds;
            }
        }
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

std::optional<std::string> PackageWatcherService::failure_message() const
{
    std::scoped_lock lock(failure_mutex_);
    if (last_service_error_) {
        return "IV package watcher service failed: " + *last_service_error_;
    }
    if (last_discovery_error_) {
        return "IV package discovery failed: " + *last_discovery_error_;
    }
    if (!last_refresh_failures_.empty()) {
        auto const& failure = last_refresh_failures_.front();
        std::string message = "IV package build failed";
        if (!failure.package_id.empty()) {
            message += " for '" + failure.package_id + "'";
        }
        if (!failure.package_root.empty()) {
            message += " at " + failure.package_root.string();
        }
        return message + ": " + failure.message;
    }
    return std::nullopt;
}

void PackageWatcherService::handle_work_available(PackageWatcherWorkAvailable const&) noexcept
{
    work_signal_.signal();
}
} // namespace iv
