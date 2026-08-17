#pragma once

#include <cstddef>
#include <functional>

namespace iv {
// Address of a port in GraphBuilderTopology. This deliberately has no
// implicit conversion to the execution graph's ConcretePortId.
struct TopologyPortId {
  size_t node = 0;
  size_t port = 0;

  constexpr TopologyPortId() = default;
  constexpr TopologyPortId(size_t node_, size_t port_)
      : node(node_), port(port_) {}

  bool operator==(TopologyPortId const &) const = default;
};
} // namespace iv

namespace std {
template <>
struct hash<iv::TopologyPortId> {
  hash<size_t> size_t_hash;

  size_t operator()(iv::TopologyPortId const &port) const {
    return size_t_hash(port.node) ^ (~size_t_hash(port.port) - 1);
  }
};
} // namespace std
