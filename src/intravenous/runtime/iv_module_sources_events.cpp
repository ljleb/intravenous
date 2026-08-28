#include <intravenous/runtime/iv_module_sources_events.h>

namespace iv {
void IvModuleSourceLookupBuilder::succeed(std::optional<IvModuleSourceInfo> source)
{
    has_response_ = true;
    source_ = std::move(source);
}

bool IvModuleSourceLookupBuilder::has_response() const
{
    return has_response_;
}

std::optional<IvModuleSourceInfo> IvModuleSourceLookupBuilder::source() const
{
    return source_;
}

IV_DEFINE_LINKER_EVENT(
    IvModuleSourceLookupEvent,
    iv_runtime_iv_module_source_lookup_event);
} // namespace iv
