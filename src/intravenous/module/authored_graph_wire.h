#pragma once

#include <intravenous/graph/authored_graph.hpp>
#include <intravenous/graph/reflected_node.hpp>
#include <intravenous/module/abi.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace iv {

struct AuthoredNodeConfigBytes {
    std::vector<std::byte> bytes{};
    std::size_t alignment = 1;
    NodeConfigStringRelocations string_relocations{};
};

// This is the exact data copied into the finalized module. The graph archive
// has native scalar fields; only strings and ranges have explicit lengths.
struct SerializedAuthoredGraph {
    std::vector<std::byte> bytes{};
    std::vector<AuthoredNodeConfigBytes> node_configs{};
};

} // namespace iv

#include <intravenous/module/authored_graph_binary_archive.hpp>

namespace iv {

inline SerializedAuthoredGraph serialize_authored_graph(
    AuthoredGraph const& authored,
    std::span<std::pair<NodeCodeKey, NodeStateStructure> const> state_structures = {})
{
    return serialize_binary_authored_graph(authored, state_structures);
}

inline AuthoredGraph deserialize_authored_graph(
    std::span<std::byte const> bytes,
    std::span<details::NodeCompilerRecord const> node_types,
    std::span<ModuleNodeConfigRecord const> node_configs,
    std::span<std::shared_ptr<void const> const> node_config_storage = {})
{
    return deserialize_binary_authored_graph(bytes, node_types, node_configs, node_config_storage);
}

} // namespace iv
