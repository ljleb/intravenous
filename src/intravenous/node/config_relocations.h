#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace iv {

// A node may opt in when a pointer-sized field in its otherwise trivially
// copyable authored configuration refers to immutable string data.
struct NodeConfigStringRelocation {
    std::size_t byte_offset = 0;
    std::string value{};
};

using NodeConfigStringRelocations = std::vector<NodeConfigStringRelocation>;

} // namespace iv
