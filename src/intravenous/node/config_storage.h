#pragma once

#include <cstddef>
#include <memory>

namespace iv::details {
// Copies a trivially-copyable node configuration into storage owned by the
// precompiled builder library. Its deleter cannot live in a temporary module
// build generation because the completed graph outlives that generation.
std::shared_ptr<void const> copy_node_config_bytes(
    void const* source, std::size_t size, std::size_t alignment);
} // namespace iv::details
