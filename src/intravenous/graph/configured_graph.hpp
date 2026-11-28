#pragma once

#include <intravenous/graph/builder/annotations.hpp>
#include <intravenous/graph/builder/connections.hpp>
#include <intravenous/graph/builder/identity.h>
#include <intravenous/graph/builder/node_bundles.hpp>
#include <intravenous/graph/builder/public_ports.hpp>
#include <intravenous/graph/builder/symbolic_nodes.hpp>
#include <intravenous/node/address.h>
#include <intravenous/node/manual_control.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <unordered_map>

namespace iv {

struct ConfiguredInvocationBundles {
  NodeBundleHandle first = 0;
  std::size_t count = 0;
  // Retained while this project instance reuses the same frozen invocation.
  std::uint64_t birth_snapshot = 0;
  std::uint64_t frozen_invocation_id = 0;
};

// The lossless configuration representation. Lowering never reaches back into
// GraphBuilder after this value is finished.
struct ConfiguredGraph {
  GraphBuilderIdentity identity{};
  GraphBuilderNodeBundles node_bundles{};
  GraphBuilderConnections connections{};
  GraphBuilderPublicPorts public_ports{0};
  GraphBuilderAnnotations annotations{};
  GraphBuilderSymbolicNodes symbolic_nodes{};
  // ProjectGraph resolves semantic input controls and output gains after
  // GraphBuilder::finish(), including implicit defaults. GraphJit expands their
  // private execution stages; the authored graph retains its public topology.
  std::vector<ConfiguredManualControl> manual_controls{};
  // ProjectGraph supplies authored instance IDs for top-level concrete leaves.
  // Package configuration snapshots have no project placements.
  std::unordered_map<NodeBundleHandle, std::string> project_leaf_instances{};
  std::map<NodeAddress, ConfiguredInvocationBundles> project_invocations{};
};

} // namespace iv
