#pragma once

#include <intravenous/ports.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace iv::graph_jit {

enum class PortDirection : std::uint8_t {
    input,
    output,
};

// A persistent identity exists only when the configured concrete node belongs
// to a stable virtual node. Anonymous concrete nodes deliberately receive no
// substitute identity derived from a generation-local bundle handle.
struct StableConcreteNodeId {
    std::string graph{};
    std::string virtual_node{};
    std::size_t direct_member = 0;

    bool operator==(StableConcreteNodeId const&) const = default;
};

struct StableOutputPortId {
    StableConcreteNodeId node{};
    PortKind kind = PortKind::sample;
    std::string port_name{};
    std::size_t port_index = 0;

    bool operator==(StableOutputPortId const&) const = default;
};

} // namespace iv::graph_jit
