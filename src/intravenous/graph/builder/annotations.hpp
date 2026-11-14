#pragma once

#include <intravenous/graph/builder/identity.h>
#include <intravenous/graph/builder/node_bundles.hpp>
#include <intravenous/graph/builder/virtual_nodes.hpp>
#include <intravenous/graph/source_info.h>

namespace iv {

// Source annotations are stored on their NodeBundle and virtual-node records.
// Which live virtual record a declaration joins depends on the builder's
// scope namespaces, so GraphBuilderState performs annotation directly; this
// component carries no state of its own in a ConfiguredGraph.
class GraphBuilderAnnotations {};

} // namespace iv
