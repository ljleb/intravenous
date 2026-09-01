#pragma once

#include <intravenous/graph/connection_node.hpp>
#include <intravenous/graph/reflected_node.hpp>
#include <intravenous/graph/runtime_binding_nodes.hpp>
#include <intravenous/basic_nodes/routing.h>
#include <intravenous/basic_nodes/type_erased.h>

#include <stdexcept>
#include <type_traits>
#include <variant>

namespace iv {
struct EventConcatenationNodeSpec {
    size_t input_count = 0;
    EventTypeId type = EventTypeId::empty;
};

struct BroadcastEventNodeSpec {
    size_t output_count = 0;
    EventTypeId type = EventTypeId::empty;
};

struct DummySinkNodeSpec {};
struct DummyEventSinkNodeSpec {};

struct ConstantNodeSpec {
    Sample value = 0.0f;
};

using GeneratedNodeSpec = std::variant<
    std::monostate,
    ConnectionNodeSpec,
    RuntimeSampleInputNodeSpec,
    RuntimeEventInputNodeSpec,
    RuntimeSampleOutputNodeSpec,
    RuntimeEventOutputNodeSpec,
    RuntimeSampleOutputFamilyNodeSpec,
    RuntimeEventOutputFamilyNodeSpec,
    EventConcatenationNodeSpec,
    BroadcastEventNodeSpec,
    DummySinkNodeSpec,
    DummyEventSinkNodeSpec,
    ConstantNodeSpec>;

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

constexpr NodePorts generated_node_ports(
    EventConcatenationNodeSpec const& spec)
{
    return {
        .event_input_configs = std::vector<EventInputConfig>(
            spec.input_count, EventInputConfig{.type = spec.type}),
        .event_output_configs = {{.type = spec.type}},
    };
}

constexpr NodePorts generated_node_ports(BroadcastEventNodeSpec const& spec)
{
    return {
        .event_input_configs = {{.type = spec.type}},
        .event_output_configs = std::vector<EventOutputConfig>(
            spec.output_count, EventOutputConfig{.type = spec.type}),
    };
}

constexpr NodePorts generated_node_ports(DummySinkNodeSpec const&)
{
    return {.sample_inputs = {InputConfig{}}};
}

constexpr NodePorts generated_node_ports(DummyEventSinkNodeSpec const&)
{
    return {.event_input_configs = {{.type = EventTypeId::empty}}};
}

constexpr NodePorts generated_node_ports(ConstantNodeSpec const&)
{
    return {.sample_outputs = {OutputConfig{}}};
}

consteval EventConcatenation freeze_generated_node(
    EventConcatenationNodeSpec const& spec)
{
    return EventConcatenation(spec.input_count, spec.type);
}

consteval BroadcastEvent freeze_generated_node(
    BroadcastEventNodeSpec const& spec)
{
    return BroadcastEvent(spec.output_count, spec.type);
}

consteval DummySink freeze_generated_node(DummySinkNodeSpec const&)
{
    return {};
}

consteval DummyEventSink freeze_generated_node(DummyEventSinkNodeSpec const&)
{
    return {};
}

consteval Constant freeze_generated_node(ConstantNodeSpec const& spec)
{
    return Constant{spec.value};
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
    // libstdc++ dispatches std::visit through a table of function pointers.
    // The callbacks here are immediate because freezing a compiler-owned node
    // promotes its value to static storage, so GCC correctly rejects taking
    // their addresses. Keep this dispatch direct instead.
    switch (spec.index()) {
    case 1: {
        auto const& value = std::get<ConnectionNodeSpec>(spec);
        return materialize_generated_node(
            freeze_generated_node(value), generated_node_ports(value));
    }
    case 2: {
        auto const& value = std::get<RuntimeSampleInputNodeSpec>(spec);
        return materialize_generated_node(
            freeze_generated_node(value), generated_node_ports(value));
    }
    case 3: {
        auto const& value = std::get<RuntimeEventInputNodeSpec>(spec);
        return materialize_generated_node(
            freeze_generated_node(value), generated_node_ports(value));
    }
    case 4: {
        auto const& value = std::get<RuntimeSampleOutputNodeSpec>(spec);
        return materialize_generated_node(
            freeze_generated_node(value), generated_node_ports(value));
    }
    case 5: {
        auto const& value = std::get<RuntimeEventOutputNodeSpec>(spec);
        return materialize_generated_node(
            freeze_generated_node(value), generated_node_ports(value));
    }
    case 6: {
        auto const& value = std::get<RuntimeSampleOutputFamilyNodeSpec>(spec);
        return materialize_generated_node(
            freeze_generated_node(value), generated_node_ports(value));
    }
    case 7: {
        auto const& value = std::get<RuntimeEventOutputFamilyNodeSpec>(spec);
        return materialize_generated_node(
            freeze_generated_node(value), generated_node_ports(value));
    }
    case 8: {
        auto const& value = std::get<EventConcatenationNodeSpec>(spec);
        return materialize_generated_node(
            freeze_generated_node(value), generated_node_ports(value));
    }
    case 9: {
        auto const& value = std::get<BroadcastEventNodeSpec>(spec);
        return materialize_generated_node(
            freeze_generated_node(value), generated_node_ports(value));
    }
    case 10: {
        auto const& value = std::get<DummySinkNodeSpec>(spec);
        return materialize_generated_node(
            freeze_generated_node(value), generated_node_ports(value));
    }
    case 11: {
        auto const& value = std::get<DummyEventSinkNodeSpec>(spec);
        return materialize_generated_node(
            freeze_generated_node(value), generated_node_ports(value));
    }
    case 12: {
        auto const& value = std::get<ConstantNodeSpec>(spec);
        return materialize_generated_node(
            freeze_generated_node(value), generated_node_ports(value));
    }
    default:
        throw std::logic_error("concrete node has no implementation");
    }
}

constexpr ReflectedNodeDescription materialize_generated_node_or_forbid(
    GeneratedNodeSpec const& spec)
{
    if consteval {
        return materialize_generated_node(spec);
    } else {
        runtime_graph_builder_node_call_is_forbidden();
        return {};
    }
}
} // namespace details
} // namespace iv
