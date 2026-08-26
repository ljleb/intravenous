#pragma once

#include <intravenous/ports.h>

#include <cstdint>
#include <cstdlib>
#include <cxxabi.h>
#include <memory>
#include <string>
#include <string_view>

namespace iv::details {
inline std::string demangle_type_name(char const* name)
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

constexpr std::string event_type_name(EventTypeId type)
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

constexpr std::string stable_identity_suffix(std::string_view value)
{
    std::uint64_t hash = 14695981039346656037ull;
    for (unsigned char character : value) {
        hash ^= character;
        hash *= 1099511628211ull;
    }

    constexpr char hex[] = "0123456789abcdef";
    char digits[16] {};
    size_t begin = sizeof(digits);
    do {
        digits[--begin] = hex[hash & 0xfu];
        hash >>= 4;
    } while (hash != 0);
    return std::string(digits + begin, digits + sizeof(digits));
}

constexpr std::string typed_virtual_node_id(
    std::string_view source_identity,
    std::string_view type_identity)
{
    return std::string(source_identity) + "#type:"
        + stable_identity_suffix(type_identity);
}
} // namespace iv::details
