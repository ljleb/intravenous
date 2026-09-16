#pragma once

#include <intravenous/graph/builder/connections.hpp>
#include <intravenous/graph/builder/detach.hpp>
#include <intravenous/graph/builder/embedding.h>
#include <intravenous/graph/builder/node_bundles.hpp>
#include <intravenous/graph/builder/public_ports.hpp>
#include <intravenous/graph/builder/virtual_nodes.hpp>

#include <cstddef>
#include <numeric>
#include <utility>
#include <vector>

namespace iv {
struct GraphBuilderChildImport {
  std::size_t bundle_offset = 0;
  std::vector<NodeBundleHandle> node_bundles{};
  std::vector<VirtualNodeHandle> virtual_nodes{};
};

class GraphBuilderChildEmbedder {
public:
  static constexpr GraphBuilderChildImport import(
      GraphBuilderNodeBundles& parent_bundles,
      GraphBuilderConnections& parent_connections,
      GraphBuilderDetach& parent_detach,
      GraphBuilderVirtualNodes& parent_virtual_nodes,
      GraphBuilderNodeBundles const& child_bundles,
      GraphBuilderConnections const& child_connections,
      GraphBuilderDetach const& child_detach,
      GraphBuilderVirtualNodes const& child_virtual_nodes)
  {
    auto const expected_bundle_offset = parent_bundles.size();
    auto const root_scope = expected_bundle_offset + child_bundles.size();
    auto const detach_offset = parent_detach.reserve_child_offset(child_detach);
    auto const bundle_offset =
        parent_bundles.import_child(child_bundles, detach_offset);
    IV_ASSERT(
        bundle_offset == expected_bundle_offset,
        "embedded child bundle offset changed unexpectedly");
    parent_connections.import_child(child_connections, bundle_offset);
    parent_detach.import_child(child_detach, bundle_offset, detach_offset);
    auto virtual_nodes = parent_virtual_nodes.import_child(
        parent_bundles, child_virtual_nodes, bundle_offset, root_scope);

    std::vector<NodeBundleHandle> node_bundles(child_bundles.size());
    std::iota(node_bundles.begin(), node_bundles.end(), bundle_offset);
    return {
        .bundle_offset = bundle_offset,
        .node_bundles = std::move(node_bundles),
        .virtual_nodes = std::move(virtual_nodes),
    };
  }
};
} // namespace iv
