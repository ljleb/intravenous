#pragma once

#include <string>

namespace iv {
// Why a live GraphBuilder scope cannot be copied into a closed ConfiguredGraph.
// The scope remains valid live graph state; only independent reuse is refused.
struct ConfiguredGraphSnapshotError {
  enum class Kind {
    // The scope still has nested scopes open, so its contents are incomplete.
    open_nested_scope,
    // The view does not denote an open scope of its session.
    closed_scope,
    // A connection, expression, or virtual-node record of the scope refers to a
    // node bundle outside the scope (a free graph capture).
    free_graph_capture,
    // Construction inside the scope mutated graph state that belongs to an
    // enclosing scope, so the scope does not contain its complete effect.
    enclosing_scope_mutation,
  };

  Kind kind = Kind::free_graph_capture;
  std::string message{};
};
} // namespace iv
