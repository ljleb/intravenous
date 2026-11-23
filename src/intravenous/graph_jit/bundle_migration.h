#pragma once

#include <intravenous/graph_jit/stable_graph_identity.h>

namespace iv {
struct ConfiguredGraph;

namespace graph_jit {

// Reconcile directly with the execution predecessor. A cold compilation uses
// the first membership of each bundle to establish its initial identity.
BundleMigrationIdentities assign_bundle_migration_identities(
    ConfiguredGraph const& graph,
    ConfiguredBundleInventory inventory,
    ConfiguredGraph const* predecessor = nullptr,
    BundleMigrationIdentities const* predecessor_identities = nullptr);

} // namespace graph_jit
} // namespace iv
