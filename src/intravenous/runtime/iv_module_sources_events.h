#pragma once

#include <intravenous/linker_event.h>
#include <intravenous/runtime/iv_module_sources.h>

#include <optional>
#include <string>

namespace iv {
class IvModuleSourceLookupBuilder {
    bool has_response_ = false;
    std::optional<IvModuleSourceInfo> source_ {};

public:
    void succeed(std::optional<IvModuleSourceInfo> source);
    [[nodiscard]] bool has_response() const;
    [[nodiscard]] std::optional<IvModuleSourceInfo> source() const;
};

using IvModuleSourceLookupEvent =
    void (*)(std::string const &, IvModuleSourceLookupBuilder &);

IV_DECLARE_LINKER_EVENT(
    IvModuleSourceLookupEvent,
    iv_runtime_iv_module_source_lookup_event);
} // namespace iv
