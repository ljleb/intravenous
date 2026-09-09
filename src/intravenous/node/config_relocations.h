#pragma once

#include <cstddef>
#include <vector>

namespace iv {

// Builder-owned symbolic relocation data for pointer fields in an otherwise
// trivially copyable node configuration. `target` is an opaque finalizer
// handle for a retained immutable LLVM global; a null target is an explicit
// null pointer slot. Node types do not opt in or provide a trait.
struct NodeConfigRelocation {
    std::size_t byte_offset = 0;
    void const* target = nullptr;
    std::size_t addend = 0;
};

using NodeConfigRelocations = std::vector<NodeConfigRelocation>;

} // namespace iv
