#pragma once

#include <cstddef>
#include <string_view>
#include <type_traits>

namespace iv {

// A compact, non-owning string used in trivially-copyable node configuration.
// During module authoring it may refer to a literal or caller-owned text. The
// finalized-module loader copies and owns its backing bytes before execution.
struct NodeConfigString {
    char const* data = "";
    std::size_t size = 0;

    constexpr NodeConfigString() = default;

    template<std::size_t N>
    constexpr NodeConfigString(char const (&value)[N]) : data(value), size(N - 1)
    {}

    constexpr explicit NodeConfigString(std::string_view value)
        : data(value.data()), size(value.size())
    {}

    constexpr std::string_view view() const { return {data, size}; }
};

static_assert(std::is_trivially_copyable_v<NodeConfigString>);

} // namespace iv
