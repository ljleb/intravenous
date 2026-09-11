#include <intravenous/runtime/iv_packages_events.h>

namespace iv {
void IvPackageLookupBuilder::succeed(std::optional<IvPackageInfo> package)
{
    has_response_ = true;
    package_ = std::move(package);
}

bool IvPackageLookupBuilder::has_response() const
{
    return has_response_;
}

std::optional<IvPackageInfo> IvPackageLookupBuilder::package() const
{
    return package_;
}

IV_DEFINE_LINKER_EVENT(
    IvPackageLookupEvent,
    iv_runtime_iv_package_lookup_event);
} // namespace iv
