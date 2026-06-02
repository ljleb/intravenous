#include "names.h"

#include <cstdint>
#include <cstdlib>
#include <cxxabi.h>
#include <memory>
#include <sstream>

namespace iv::details {
std::string demangle_type_name(char const* name)
{
    if (name == nullptr || *name == '\0') {
        return {};
    }
#if defined(__GNUG__)
    int status = 0;
    std::unique_ptr<char, void(*)(void*)> demangled(
        abi::__cxa_demangle(name, nullptr, nullptr, &status),
        std::free
    );
    if (status == 0 && demangled) {
        return demangled.get();
    }
#endif
    return name;
}

std::string event_type_name(EventTypeId type)
{
    switch (type) {
    case EventTypeId::midi:
        return "midi";
    case EventTypeId::trigger:
        return "trigger";
    case EventTypeId::boundary:
        return "boundary";
    case EventTypeId::empty:
        return "empty";
    case EventTypeId::count:
        break;
    }
    return "unknown";
}

std::string stable_identity_suffix(std::string_view value)
{
    std::uint64_t hash = 14695981039346656037ull;
    for (unsigned char c : value) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    std::ostringstream out;
    out << std::hex << hash;
    return out.str();
}
}
