#pragma once

#include <intravenous/graph/builder/annotations.hpp>
#include <intravenous/graph/builder/connections.hpp>
#include <intravenous/graph/builder/identity.h>
#include <intravenous/graph/builder/node_bundles.hpp>
#include <intravenous/graph/builder/public_ports.hpp>
#include <intravenous/graph/builder/virtual_nodes.hpp>
#include <intravenous/node/manual_control.h>

namespace iv {

// The lossless configuration representation. Lowering never reaches back into
// GraphBuilder after this value is finished.
struct ConfiguredGraph {
  GraphBuilderIdentity identity{};
  GraphBuilderNodeBundles node_bundles{};
  GraphBuilderConnections connections{};
  GraphBuilderPublicPorts public_ports{0};
  GraphBuilderAnnotations annotations{};
  GraphBuilderVirtualNodes virtual_nodes{};
  // ProjectGraph resolves semantic input controls and output gains after
  // GraphBuilder::finish(), including implicit defaults. GraphJit expands their
  // private execution stages; the authored graph retains its public topology.
  std::vector<ConfiguredManualControl> manual_controls{};
};

} // namespace iv
