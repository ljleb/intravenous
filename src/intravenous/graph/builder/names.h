#pragma once

#include <intravenous/graph/compiler.h>

#include <string>
#include <string_view>

namespace iv::details {
    std::string demangle_type_name(char const* name);
    std::string event_type_name(EventTypeId type);
    std::string stable_identity_suffix(std::string_view value);
}
