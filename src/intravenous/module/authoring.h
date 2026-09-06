#pragma once

#include <intravenous/module/abi.h>

#include <cstddef>
#include <memory>

namespace iv {
class GraphBuilder;

namespace details {
// Copies a trivially-copyable authored Node into storage owned by
// iv_module_shared. The deleter must not live in the temporary ORC authoring
// generation because AuthoredGraph outlives that generation.
std::shared_ptr<void const> copy_authored_node_bytes(
    void const* source, std::size_t size, std::size_t alignment);
}
} // namespace iv
