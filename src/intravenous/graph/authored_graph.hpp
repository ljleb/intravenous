#pragma once

#include <intravenous/graph/builder/annotations.hpp>
#include <intravenous/graph/builder/connections.hpp>
#include <intravenous/graph/builder/detach.hpp>
#include <intravenous/graph/builder/identity.h>
#include <intravenous/graph/builder/node_bundles.hpp>
#include <intravenous/graph/builder/public_ports.hpp>
#include <intravenous/graph/builder/virtual_nodes.hpp>

namespace iv {

// The lossless authoring representation. Lowering never reaches back into
// GraphBuilder after this value is finished.
struct AuthoredGraph {
  GraphBuilderIdentity identity{};
  GraphBuilderNodeBundles node_bundles{};
  GraphBuilderConnections connections{};
  GraphBuilderPublicPorts public_ports{0};
  GraphBuilderDetach detach{};
  GraphBuilderAnnotations annotations{};
  GraphBuilderVirtualNodes virtual_nodes{};
};

} // namespace iv
