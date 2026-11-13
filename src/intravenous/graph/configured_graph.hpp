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
  // ProjectGraph resolves ManualControlId values into authored input ports
  // after GraphBuilder::finish(), including implicit defaults. GraphJit routes
  // module inputs through their boundaries before lowering contributions at
  // concrete consumers; authored graphs do not contain synthetic control nodes.
  std::vector<ConfiguredManualControl> manual_controls{};
};

} // namespace iv
