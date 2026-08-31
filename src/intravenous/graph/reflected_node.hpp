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

// Every function is specialized on the node type only.  The authored value is
// promoted to static storage and travels as data, so several differently
// configured instances of the same node type share one set of callbacks.
struct ReflectedNodeRuntimeOperations {
    void const* node_data = nullptr;
    size_t (*declare_node)(void const*, NodeLayoutBuilder&) = nullptr;
    void (*tick_block)(
        void const*,
        ReflectedNodeTickContext const&,
        size_t,
        size_t) = nullptr;
    void (*skip_block)(
        void const*,
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
    ReflectedNodeOperations (*apply_detach_id_offset_fn)(void const*, size_t) = nullptr;

    constexpr bool valid() const
    {
        return runtime.valid() && apply_detach_id_offset_fn != nullptr;
    }

    constexpr ReflectedNodeOperations apply_detach_id_offset(size_t offset) const
    {
        return apply_detach_id_offset_fn(runtime.node_data, offset);
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

    template<class Node>
    constexpr ReflectedNodeOperations reflected_node_operations(Node const* node_data);

    template<class Node>
    size_t declare_reflected_node(void const* node_data, NodeLayoutBuilder& builder)
    {
        auto const& node = *static_cast<Node const*>(node_data);
        DeclarationContext<Node> ctx(builder, node);
        if constexpr (details::has_declare<Node>) {
            node.declare(ctx);
        }
        return ctx.node_index();
    }

    template<class Node>
    IV_FORCEINLINE void tick_reflected_node_block(
        void const* node_data,
        ReflectedNodeTickContext const& ctx,
        size_t index,
        size_t block_size)
    {
        auto const& node = *static_cast<Node const*>(node_data);
        do_tick_block(node, TickBlockContext<Node> {
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

    template<class Node>
    IV_FORCEINLINE void skip_reflected_node_block(
        void const* node_data,
        ReflectedNodeTickContext const& ctx,
        size_t index,
        size_t block_size)
    {
        auto const& node = *static_cast<Node const*>(node_data);
        do_skip_block(node, SkipBlockContext<Node> {
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

    template<class Node>
    constexpr ReflectedNodeOperations apply_reflected_detach_id_offset(
        void const* node_data,
        size_t offset)
    {
        auto const& node = *static_cast<Node const*>(node_data);
        if constexpr (
            std::same_as<Node, DetachWriterNode>
            || std::same_as<Node, DetachReaderNode>) {
            if consteval {
                auto adjusted = node;
                adjusted.id.id += offset;
                auto const* adjusted_data = std::define_static_object(adjusted);
                return reflected_node_operations<Node>(adjusted_data);
            } else {
                runtime_graph_builder_node_call_is_forbidden();
                return {};
            }
        } else {
            return reflected_node_operations<Node>(static_cast<Node const*>(node_data));
        }
    }

    template<class Node>
    constexpr ReflectedNodeOperations reflected_node_operations(Node const* node_data)
    {
        return {
            .runtime = {
                .node_data = node_data,
                .declare_node = &declare_reflected_node<Node>,
                .tick_block = &tick_reflected_node_block<Node>,
                .skip_block = &skip_reflected_node_block<Node>,
            },
            .apply_detach_id_offset_fn =
                &apply_reflected_detach_id_offset<Node>,
        };
    }

    template<class Node>
    consteval ReflectedNodeDescription describe_reflected_node(
        Node const& node,
        Node const* node_data)
    {
        ReflectedNodeDescription description;
        for (auto const& input : get_inputs(node)) {
            description.ports.sample_inputs.push_back(input);
        }
        for (auto const& output : get_outputs(node)) {
            description.ports.sample_outputs.push_back(output);
        }
        for (auto const& input : get_event_inputs(node)) {
            description.ports.event_input_configs.push_back(input);
        }
        for (auto const& output : get_event_outputs(node)) {
            description.ports.event_output_configs.push_back(output);
        }

        description.operations = reflected_node_operations<Node>(node_data);
        description.type_name = std::meta::display_string_of(
            std::meta::dealias(^^Node));
        description.internal_latency_samples = get_internal_latency(node);
        description.maximum_block_size = get_max_block_size(node);
        description.default_ttl_samples = get_ttl_samples(node);
        description.block_skippable = get_can_skip_block(node);
        return description;
    }

    template<class Node>
    consteval ReflectedNodeDescription reflect_node(Node node)
    {
        static_assert(
            std::copy_constructible<Node>,
            "authored node values must be copy constructible");
        auto const* node_data = std::define_static_object(node);
        return describe_reflected_node(*node_data, node_data);
    }
} // namespace details
} // namespace iv
