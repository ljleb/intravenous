#pragma once

#include <intravenous/basic_nodes/routing.h>
#include <intravenous/graph/reflected_node.h>
#include <intravenous/node/lifecycle.h>

#include <meta>

#include <concepts>
#include <string>
#include <type_traits>
#include <utility>

namespace iv::details {
    template<auto NodeValue>
    constexpr ReflectedNodeOperations reflected_node_operations();

    template<auto NodeValue>
    size_t declare_reflected_node(NodeLayoutBuilder& builder)
    {
        using Node = std::remove_cvref_t<decltype(NodeValue)>;
        DeclarationContext<Node> ctx(
            builder,
            std::integral_constant<decltype(NodeValue), NodeValue> {});
        if constexpr (details::has_declare<Node>) {
            NodeValue.declare(ctx);
        }
        return ctx.node_index();
    }

    template<auto NodeValue>
    IV_FORCEINLINE void tick_reflected_node_block(
        ReflectedNodeTickContext const& ctx,
        size_t index,
        size_t block_size)
    {
        using Node = std::remove_cvref_t<decltype(NodeValue)>;
        do_tick_block(NodeValue, TickBlockContext<Node> {
            TickContext<Node> {
                .inputs = ctx.inputs,
                .outputs = ctx.outputs,
                .event_inputs = ctx.event_inputs,
                .event_outputs = ctx.event_outputs,
                .sample_rate = ctx.sample_rate,
                .scc_feedback_latency = ctx.scc_feedback_latency,
                .buffer = ctx.state,
            },
            index,
            block_size,
        });
    }

    template<auto NodeValue>
    IV_FORCEINLINE void skip_reflected_node_block(
        ReflectedNodeTickContext const& ctx,
        size_t index,
        size_t block_size)
    {
        using Node = std::remove_cvref_t<decltype(NodeValue)>;
        do_skip_block(NodeValue, SkipBlockContext<Node> {
            TickContext<Node> {
                .inputs = ctx.inputs,
                .outputs = ctx.outputs,
                .event_inputs = ctx.event_inputs,
                .event_outputs = ctx.event_outputs,
                .sample_rate = ctx.sample_rate,
                .scc_feedback_latency = ctx.scc_feedback_latency,
                .buffer = ctx.state,
            },
            index,
            block_size,
        });
    }

    template<auto NodeValue>
    constexpr ReflectedNodeOperations apply_reflected_detach_id_offset(
        size_t offset)
    {
        if consteval {
            using Node = std::remove_cvref_t<decltype(NodeValue)>;
            if constexpr (
                std::same_as<Node, DetachWriterNode>
                || std::same_as<Node, DetachReaderNode>) {
                auto adjusted = NodeValue;
                adjusted.id.id += offset;
                return reflect_node(adjusted).operations;
            } else {
                return reflected_node_operations<NodeValue>();
            }
        } else {
            runtime_graph_builder_node_call_is_forbidden();
            return {};
        }
    }

    template<auto NodeValue>
    constexpr ReflectedNodeOperations reflected_node_operations()
    {
        return {
            .declare_node = &declare_reflected_node<NodeValue>,
            .tick_block = &tick_reflected_node_block<NodeValue>,
            .skip_block = &skip_reflected_node_block<NodeValue>,
            .apply_detach_id_offset =
                &apply_reflected_detach_id_offset<NodeValue>,
        };
    }

    template<auto NodeValue>
    constexpr ReflectedNodeDescription describe_reflected_node()
    {
        using Node = std::remove_cvref_t<decltype(NodeValue)>;

        ReflectedNodeDescription description;
        for (auto const& input : get_inputs(NodeValue)) {
            description.ports.sample_inputs.push_back(input);
        }
        for (auto const& output : get_outputs(NodeValue)) {
            description.ports.sample_outputs.push_back(output);
        }
        for (auto const& input : get_event_inputs(NodeValue)) {
            description.ports.event_input_configs.push_back(input);
        }
        for (auto const& output : get_event_outputs(NodeValue)) {
            description.ports.event_output_configs.push_back(output);
        }

        description.operations = reflected_node_operations<NodeValue>();
        description.type_name = std::meta::display_string_of(^^Node);
        description.internal_latency_samples = get_internal_latency(NodeValue);
        description.maximum_block_size = get_max_block_size(NodeValue);
        description.default_ttl_samples = get_ttl_samples(NodeValue);
        description.block_skippable = get_can_skip_block(NodeValue);
        return description;
    }

    template<class Node>
    consteval ReflectedNodeDescription reflect_node(Node node)
    {
        static_assert(
            std::copy_constructible<Node>,
            "authored node values must be copy constructible");
        auto const reflected_node = std::meta::reflect_constant(node);
        auto const description_specialization = std::meta::substitute(
            ^^describe_reflected_node,
            {reflected_node});
        auto const describe = std::meta::extract<
            ReflectedNodeDescription (*)()>(description_specialization);
        return describe();
    }
}
