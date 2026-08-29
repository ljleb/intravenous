#pragma once

#include <intravenous/basic_nodes/routing.h>
#include <intravenous/node/lifecycle.h>
#include <intravenous/ports.h>

#include <meta>

#include <concepts>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace iv {
struct NodeLayoutBuilder;

struct NodePorts {
    std::vector<InputConfig> sample_inputs {};
    std::vector<OutputConfig> sample_outputs {};
    std::vector<EventInputConfig> event_input_configs {};
    std::vector<EventOutputConfig> event_output_configs {};

    constexpr std::vector<InputConfig> const& inputs() const
    {
        return sample_inputs;
    }

    constexpr std::vector<OutputConfig> const& outputs() const
    {
        return sample_outputs;
    }

    constexpr std::vector<EventInputConfig> const& event_inputs() const
    {
        return event_input_configs;
    }

    constexpr std::vector<EventOutputConfig> const& event_outputs() const
    {
        return event_output_configs;
    }
};

struct ReflectedNodeTickContext {
    std::span<InputPort> inputs {};
    std::span<OutputPort> outputs {};
    std::span<EventInputPort> event_inputs {};
    std::span<EventOutputPort> event_outputs {};
    size_t sample_rate = 48000;
    size_t scc_feedback_latency = 0;
    std::span<std::byte> state {};
};

// Every function is specialized on the exact reflected structural node value.
// No authored node object or erased object pointer is stored here.
struct ReflectedNodeRuntimeOperations {
    size_t (*declare_node)(NodeLayoutBuilder&) = nullptr;
    void (*tick_block)(
        ReflectedNodeTickContext const&,
        size_t,
        size_t) = nullptr;
    void (*skip_block)(
        ReflectedNodeTickContext const&,
        size_t,
        size_t) = nullptr;

    constexpr bool valid() const
    {
        return declare_node != nullptr && tick_block != nullptr
            && skip_block != nullptr;
    }
};

struct ReflectedNodeOperations {
    ReflectedNodeRuntimeOperations runtime {};
    ReflectedNodeOperations (*apply_detach_id_offset)(size_t) = nullptr;

    constexpr bool valid() const
    {
        return runtime.valid() && apply_detach_id_offset != nullptr;
    }
};

struct ReflectedNodeDescription {
    NodePorts ports {};
    ReflectedNodeOperations operations {};
    std::string_view type_name {};
    size_t internal_latency_samples = 0;
    size_t maximum_block_size = MAX_BLOCK_SIZE;
    std::optional<size_t> default_ttl_samples {};
    bool block_skippable = false;

    constexpr std::vector<InputConfig> const& inputs() const
    {
        return ports.sample_inputs;
    }

    constexpr std::vector<OutputConfig> const& outputs() const
    {
        return ports.sample_outputs;
    }

    constexpr std::vector<EventInputConfig> const& event_inputs() const
    {
        return ports.event_input_configs;
    }

    constexpr std::vector<EventOutputConfig> const& event_outputs() const
    {
        return ports.event_output_configs;
    }

    constexpr size_t internal_latency() const
    {
        return internal_latency_samples;
    }

    constexpr size_t max_block_size() const
    {
        return maximum_block_size;
    }

    constexpr std::optional<size_t> ttl_samples() const
    {
        return default_ttl_samples;
    }

    constexpr bool can_skip_block() const
    {
        return block_skippable;
    }
};

namespace details {
    [[gnu::error(
        "GraphBuilder::node may only be called during constant evaluation")]]
    void runtime_graph_builder_node_call_is_forbidden();

    template<class Node>
    consteval ReflectedNodeDescription reflect_node(Node node);

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
    consteval ReflectedNodeOperations apply_reflected_detach_id_offset(
        size_t offset)
    {
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
    }

    template<auto NodeValue>
    constexpr ReflectedNodeOperations reflected_node_operations()
    {
        return {
            .runtime = {
                .declare_node = &declare_reflected_node<NodeValue>,
                .tick_block = &tick_reflected_node_block<NodeValue>,
                .skip_block = &skip_reflected_node_block<NodeValue>,
            },
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
        description.type_name = std::meta::display_string_of(
            std::meta::dealias(^^Node));
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
        if constexpr (std::is_empty_v<Node> && std::default_initializable<Node>) {
            return describe_reflected_node<Node {}>();
        } else {
            auto const reflected_node = std::meta::reflect_constant(node);
            auto const description_specialization = std::meta::substitute(
                ^^describe_reflected_node,
                {reflected_node});
            auto const describe = std::meta::extract<
                ReflectedNodeDescription (*)()>(description_specialization);
            return describe();
        }
    }
} // namespace details
} // namespace iv
