#pragma once

#include <intravenous/graph/port_ids.h>

#include <cstddef>
#include <stdexcept>
#include <vector>

namespace iv {
using SymbolicNodeHandle = std::size_t;

// Explicit local-to-parent translation for one embedded configured graph.
// The embedded graph's root scope is represented by the synthetic subgraph
// bundle created in the parent. Nested scopes and tiled children are ordinary
// local node-bundle handles and translate through node_bundle().
struct ConfiguredGraphEmbedding {
  NodeBundleHandle root_scope = 0;
  std::vector<NodeBundleHandle> node_bundles{};
  std::vector<SymbolicNodeHandle> symbolic_nodes{};

  [[nodiscard]] NodeBundleHandle node_bundle(NodeBundleHandle local) const
  {
    if (local >= node_bundles.size()) {
      throw std::out_of_range("configured graph local node-bundle handle is out of range");
    }
    return node_bundles[local];
  }

  [[nodiscard]] NodeBundleHandle scope(NodeBundleHandle local_subgraph) const
  {
    return node_bundle(local_subgraph);
  }

  [[nodiscard]] NodeBundleHandle tiled_child(NodeBundleHandle local_child) const
  {
    return node_bundle(local_child);
  }

  [[nodiscard]] SymbolicNodeHandle symbolic_node(SymbolicNodeHandle local) const
  {
    if (local >= symbolic_nodes.size()) {
      throw std::out_of_range("configured graph local symbolic-node handle is out of range");
    }
    return symbolic_nodes[local];
  }
};
} // namespace iv
