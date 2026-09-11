#pragma once

#include <intravenous/graph/configured_graph.hpp>
#include <intravenous/graph/reflected_node_description.h>
#include <intravenous/module/abi.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace iv {

struct ConfiguredNodeConfigBytes {
    std::vector<std::byte> bytes{};
    std::size_t alignment = 1;
    NodeConfigRelocations relocations{};
};

// This is the exact data copied into the finalized module. The graph archive
// has native scalar fields; only strings and ranges have explicit lengths.
struct SerializedConfiguredGraph {
    std::vector<std::byte> bytes{};
    std::vector<ConfiguredNodeConfigBytes> node_configs{};
};

} // namespace iv

#include <intravenous/module/configured_graph_binary_archive.hpp>

namespace iv {

inline SerializedConfiguredGraph serialize_configured_graph(
    ConfiguredGraph const& configured,
    std::span<std::pair<NodeCodeKey, NodeStateStructure> const> state_structures = {})
{
    return serialize_binary_configured_graph(configured, state_structures);
}

inline ConfiguredGraph deserialize_configured_graph(
    std::span<std::byte const> bytes,
    std::span<details::NodeCompilerRecord const> node_types,
    std::span<ModuleNodeConfigRecord const> node_configs,
    std::span<std::shared_ptr<void const> const> node_config_storage = {})
{
    return deserialize_binary_configured_graph(bytes, node_types, node_configs, node_config_storage);
}

} // namespace iv
