#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <optional>
#include <string>
#include <vector>

namespace iv {

// Owns an aligned immutable allocation independently of package code lifetime.
// bytes is the lossless representation used by configuration serialization.
struct ImmutableConfigData {
    std::span<std::byte const> bytes{};
    std::size_t alignment = 1;
    std::shared_ptr<void const> storage{};
};

namespace details {
std::shared_ptr<ImmutableConfigData const> copy_immutable_config_data(
    std::span<std::byte const>, std::size_t alignment);
}


// Symbolic relocation for a pointer field in an otherwise trivially copyable
// node configuration. A non-null pointer is represented by the IV package that
// owns the immutable LLVM global, that source's retained-global index, and a
// byte addend, or by graph-owned immutable_data. An empty index with no
// immutable_data represents an explicit null pointer entry.
struct NodeConfigRelocation {
    std::size_t byte_offset = 0;
    std::string package_root{};
    std::optional<std::size_t> retained_global_index{};
    std::size_t addend = 0;
    // Alternative to retained_global_index. The immutable allocation is owned
    // by this relocation, so graph copies and revisions share it safely.
    std::shared_ptr<ImmutableConfigData const> immutable_data{};
};

using NodeConfigRelocations = std::vector<NodeConfigRelocation>;

} // namespace iv
