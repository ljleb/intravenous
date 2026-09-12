#pragma once

#include <intravenous/ports.h>

#include <cstddef>

namespace iv {

// These are stable configuration identities, not node-bundle implementation
// details. Keeping them independently includable lets the DSL reference ABI
// name ports without importing GraphBuilderNodeBundles and its storage.
using NodeBundleHandle = size_t;

struct NodeBundlePortId {
  NodeBundleHandle node_bundle_handle = 0;
  PortKind port_kind = PortKind::sample;
  size_t port_ordinal = 0;
  bool operator==(NodeBundlePortId const &) const = default;
};

struct NodeBundlePortIdLess {
  constexpr bool operator()(NodeBundlePortId const& lhs,
                            NodeBundlePortId const& rhs) const {
    if (lhs.node_bundle_handle != rhs.node_bundle_handle)
      return lhs.node_bundle_handle < rhs.node_bundle_handle;
    if (lhs.port_kind != rhs.port_kind)
      return lhs.port_kind < rhs.port_kind;
    return lhs.port_ordinal < rhs.port_ordinal;
  }
};

struct SampleOutputChannelId {
  NodeBundleHandle bundle = 0;
  size_t port = 0;
  size_t channel = 0;
  bool operator==(SampleOutputChannelId const &) const = default;
};

struct SampleInputChannelId {
  NodeBundleHandle bundle = 0;
  size_t port = 0;
  size_t channel = 0;
  bool operator==(SampleInputChannelId const &) const = default;
};

struct EventOutputPortId {
  NodeBundleHandle bundle = 0;
  size_t port = 0;
  bool operator==(EventOutputPortId const &) const = default;
};

struct EventInputPortId {
  NodeBundleHandle bundle = 0;
  size_t port = 0;
  bool operator==(EventInputPortId const &) const = default;
};

} // namespace iv
