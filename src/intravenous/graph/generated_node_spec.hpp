#pragma once

#include <intravenous/graph/connection_node.hpp>
#include <intravenous/graph/reflected_node.hpp>
#include <intravenous/graph/runtime_binding_nodes.hpp>

#include <stdexcept>
#include <type_traits>
#include <variant>

namespace iv {
using GeneratedNodeSpec = std::variant<
    std::monostate,
    ConnectionNodeSpec,
    RuntimeSampleInputNodeSpec,
    RuntimeEventInputNodeSpec,
    RuntimeSampleOutputNodeSpec,
    RuntimeEventOutputNodeSpec,
    RuntimeSampleOutputFamilyNodeSpec,
    RuntimeEventOutputFamilyNodeSpec>;

namespace details {
// Compiler-owned nodes are materialized here rather than through the authored
// reflection entry point. Lowering only asks for a complete concrete-node
// description; it never needs to reflect an arbitrary authored C++ value.
template<class Node>
consteval ReflectedNodeDescription materialize_compiler_node(Node node)
{
    static_assert(
        std::copy_constructible<Node>,
        "compiler-generated node values must be copy constructible");
    auto const* node_data = std::define_static_object(node);
    return describe_reflected_node(*node_data, node_data);
}

template<class Node>
constexpr ReflectedNodeDescription materialize_compiler_node_or_forbid(
    Node node)
{
    if consteval {
        return materialize_compiler_node(node);
    } else {
        runtime_graph_builder_node_call_is_forbidden();
        return {};
    }
}

constexpr NodePorts generated_node_ports(ConnectionNodeSpec const& spec)
{
    NodePorts ports;
    ports.sample_inputs.reserve(spec.input_configs.size());
    for (auto const& input : spec.input_configs) {
        ports.sample_inputs.push_back(input.input);
    }
    ports.sample_outputs.push_back(spec.output_config);
    return ports;
}

constexpr NodePorts generated_node_ports(RuntimeSampleInputNodeSpec const& spec)
{
    return {.sample_outputs = {spec.output}};
}

constexpr NodePorts generated_node_ports(RuntimeEventInputNodeSpec const& spec)
{
    return {.event_output_configs = {{.type = spec.type}}};
}

constexpr NodePorts generated_node_ports(RuntimeSampleOutputNodeSpec const& spec)
{
    return {.sample_inputs = {spec.input}};
}

constexpr NodePorts generated_node_ports(RuntimeEventOutputNodeSpec const& spec)
{
    return {.event_input_configs = {{.type = spec.type}}};
}

constexpr NodePorts generated_node_ports(
    RuntimeSampleOutputFamilyNodeSpec const& spec)
{
    return {.sample_inputs = spec.input_configs};
}

constexpr NodePorts generated_node_ports(
    RuntimeEventOutputFamilyNodeSpec const& spec)
{
    return {.event_input_configs = std::vector<EventInputConfig>(
        spec.member_count, EventInputConfig{.type = spec.type})};
}

template<class Node>
constexpr ReflectedNodeDescription materialize_generated_node(
    Node node,
    NodePorts ports)
{
    auto description = materialize_compiler_node(std::move(node));
    description.ports = std::move(ports);
    return description;
}

consteval ReflectedNodeDescription materialize_generated_node(
    GeneratedNodeSpec const& spec)
{
    return std::visit(
        []<class Spec>(Spec const& concrete) -> ReflectedNodeDescription {
            if constexpr (std::same_as<Spec, std::monostate>) {
                throw std::logic_error("concrete node has no implementation");
            } else {
                return materialize_generated_node(
                    freeze_generated_node(concrete),
                    generated_node_ports(concrete));
            }
        },
        spec);
}
} // namespace details
} // namespace iv
