#include <intravenous/app/bridges/package_watcher_package_jit_bridge.h>
#include <intravenous/packages/package_jit.h>
#include <intravenous/packages/package_pipeline_events.h>
namespace iv {
IV_DEFINE_BRIDGE(package_watcher_package_jit_bridge)
IV_SUBSCRIBE_LINKER_EVENT(
    package_watcher_package_jit_bridge,
    iv_runtime_package_jit_batch_requested_event,
    &PackageJit::handle_build_request)
}
