#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace iv {

// Builder-owned relocation data for C-string fields in an otherwise trivially
// copyable node configuration. The compiler discovers the fields; node types
// do not opt in or provide a trait.
struct NodeConfigStringRelocation {
    std::size_t byte_offset = 0;
    std::string value{};
};

using NodeConfigStringRelocations = std::vector<NodeConfigStringRelocation>;

} // namespace iv
