#pragma once

#include <intravenous/linker_event.h>
#include <intravenous/runtime/iv_packages.h>

#include <optional>
#include <string>

namespace iv {
class IvPackageLookupBuilder {
    bool has_response_ = false;
    std::optional<IvPackageInfo> package_ {};

public:
    void succeed(std::optional<IvPackageInfo> package);
    [[nodiscard]] bool has_response() const;
    [[nodiscard]] std::optional<IvPackageInfo> package() const;
};

using IvPackageLookupEvent =
    void (*)(std::string const &, IvPackageLookupBuilder &);

IV_DECLARE_LINKER_EVENT(
    IvPackageLookupEvent,
    iv_runtime_iv_package_lookup_event);
} // namespace iv
