#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace iv {

// Symbolic relocation for a pointer field in an otherwise trivially copyable
// node configuration. A non-null pointer is represented by the IV package that
// owns the immutable LLVM global, that source's retained-global index, and a
// byte addend. An empty index represents an explicit null pointer entry.
struct NodeConfigRelocation {
    std::size_t byte_offset = 0;
    std::string package_root{};
    std::optional<std::size_t> retained_global_index{};
    std::size_t addend = 0;
};

using NodeConfigRelocations = std::vector<NodeConfigRelocation>;

} // namespace iv
