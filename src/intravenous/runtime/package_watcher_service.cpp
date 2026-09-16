#include <intravenous/runtime/package_watcher_service.h>

#include <intravenous/runtime/package_pipeline_events.h>
#include <intravenous/runtime/package_watcher.h>
#include <intravenous/runtime/runtime_project_events.h>

#include <chrono>
#include <exception>
#include <utility>

namespace iv {
namespace {
void publish_package_catalog_changed()
{
    IV_INVOKE_LINKER_EVENT_SOURCE(
        iv_runtime_iv_package_catalog_changed_event,
        IvPackageCatalogChanged{});
}
} // namespace

PackageWatcherService::PackageWatcherService(
    PackageWatcher& watcher,
    std::filesystem::path project_root,
    std::vector<std::filesystem::path> shared_roots)
    : watcher_(&watcher)
    , project_root_(std::move(project_root))
    , shared_roots_(std::move(shared_roots))
{}

PackageWatcherService::~PackageWatcherService()
{
    request_shutdown();
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

void PackageWatcherService::start()
{
    if (worker_.has_value()) return;
    worker_.emplace([this](std::stop_token stop_token) {
        while (!stop_token.stop_requested()) {
            (void)synchronize_discovered_packages();
            watcher_->poll_dependency_changes();
            if (watcher_->has_pending_refresh()) {
                PackageWatcherRefreshRequest request;
                IV_INVOKE_LINKER_EVENT_SOURCE(
                    iv_runtime_package_watcher_refresh_requested_event,
                    request);
                if (request.changed) publish_package_catalog_changed();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
}

void PackageWatcherService::request_shutdown()
{
    if (worker_.has_value()) worker_->request_stop();
}
} // namespace iv
