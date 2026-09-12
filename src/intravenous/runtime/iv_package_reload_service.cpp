#include <intravenous/runtime/iv_package_reload_service.h>

#include <intravenous/runtime/iv_module_definitions.h>
#include <intravenous/runtime/iv_module_definitions_events.h>
#include <intravenous/runtime/iv_module_reload.h>
#include <intravenous/runtime/iv_packages.h>
#include <intravenous/runtime/runtime_project_events.h>

#include <chrono>
#include <exception>
#include <utility>

namespace iv {
namespace {
void publish_package_catalog_changed()
{
    // This deliberately starts after the synchronous definitions/instances
    // propagation has unwound. The package browser is a separate terminal
    // projection; placing it beside runtime consumers in the same source
    // propagation would re-enter SocketRpcServer through sibling paths.
    IV_INVOKE_LINKER_EVENT_SOURCE(
        iv_runtime_iv_package_catalog_changed_event,
        IvPackageCatalogChanged{});
}
} // namespace

IvPackageReloadService::IvPackageReloadService(
    IvModuleReload& reload,
    IvModuleDefinitions& definitions,
    std::filesystem::path project_root,
    std::vector<std::filesystem::path> shared_roots)
    : reload_(&reload)
    , definitions_(&definitions)
    , project_root_(std::move(project_root))
    , shared_roots_(std::move(shared_roots))
{}

IvPackageReloadService::~IvPackageReloadService()
{
    request_shutdown();
}

void IvPackageReloadService::report_discovery_error(std::string message)
{
    if (last_discovery_error_.has_value() && *last_discovery_error_ == message) {
        return;
    }
    last_discovery_error_ = message;
    IV_INVOKE_LINKER_EVENT_SOURCE(
        iv_runtime_project_notification_event,
        ProjectNotification(ProjectStatusNotification{
            .level = "error",
            .code = "packageDiscoveryFailed",
            .message = "IV package discovery failed: " + std::move(message),
        }));
}

bool IvPackageReloadService::synchronize_discovered_packages()
{
    try {
        auto declarations = discover_iv_package_declarations(project_root_, shared_roots_);
        last_discovery_error_.reset();
        if (last_package_declarations_.has_value()
            && declarations == *last_package_declarations_) {
            return false;
        }
        last_package_declarations_ = declarations;
        definitions_->sync_package_declarations(std::move(declarations));
        return true;
    } catch (std::exception const& error) {
        report_discovery_error(error.what());
        return false;
    }
}

void IvPackageReloadService::start()
{
    if (worker_.has_value()) {
        return;
    }
    worker_.emplace([this](std::stop_token stop_token) {
        while (!stop_token.stop_requested()) {
            auto const discovery_changed = synchronize_discovered_packages();
            if (discovery_changed) {
                publish_package_catalog_changed();
            } else {
                if (reload_->has_dirty_packages()) {
                    reload_->compile_dirty_packages();
                } else {
                    reload_->reload_changed_packages();
                }
                // A package registry is control-plane state. It must progress
                // even when the audio graph has no task pass, otherwise an
                // uninstantiated package can compile successfully yet remain
                // invisible to the package browser indefinitely.
                if (reload_->apply_pending_results()) {
                    publish_package_catalog_changed();
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
}

void IvPackageReloadService::request_shutdown()
{
    if (worker_.has_value()) {
        worker_->request_stop();
    }
}
} // namespace iv
