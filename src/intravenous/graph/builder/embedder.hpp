#pragma once

#include <intravenous/graph/builder/connections.hpp>
#include <intravenous/graph/builder/embedding.h>
#include <intravenous/graph/builder/node_bundles.hpp>
#include <intravenous/graph/builder/public_ports.hpp>
#include <intravenous/graph/builder/symbolic_nodes.hpp>

#include <cstddef>
#include <numeric>
#include <string_view>
#include <utility>
#include <vector>

namespace iv {
struct GraphBuilderChildImport {
  std::size_t bundle_offset = 0;
  std::vector<NodeBundleHandle> node_bundles{};
  std::vector<SymbolicNodeHandle> symbolic_nodes{};
};

class GraphBuilderChildEmbedder {
public:
  static constexpr GraphBuilderChildImport import(
      GraphBuilderNodeBundles& parent_bundles,
      GraphBuilderConnections& parent_connections,
      GraphBuilderSymbolicNodes& parent_symbolic_nodes,
      GraphBuilderNodeBundles const& child_bundles,
      GraphBuilderConnections const& child_connections,
      GraphBuilderSymbolicNodes const& child_symbolic_nodes,
      std::string_view qualifier)
  {
    auto const expected_bundle_offset = parent_bundles.size();
    auto const bundle_offset = parent_bundles.import_child(child_bundles, parent_symbolic_nodes.size());
    IV_ASSERT(
        bundle_offset == expected_bundle_offset,
        "embedded child bundle offset changed unexpectedly");
    parent_connections.import_child(child_connections, bundle_offset);
    auto symbolic_nodes = parent_symbolic_nodes.import_child(
        child_symbolic_nodes, bundle_offset, qualifier);

    std::vector<NodeBundleHandle> node_bundles(child_bundles.size());
    std::iota(node_bundles.begin(), node_bundles.end(), bundle_offset);
    return {
        .bundle_offset = bundle_offset,
        .node_bundles = std::move(node_bundles),
        .symbolic_nodes = std::move(symbolic_nodes),
    };
  }
};
} // namespace iv
