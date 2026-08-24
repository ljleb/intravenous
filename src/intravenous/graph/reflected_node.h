#pragma once

#include <intravenous/ports.h>

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
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
struct ReflectedNodeOperations {
    size_t (*declare_node)(NodeLayoutBuilder&) = nullptr;
    void (*tick_block)(
        ReflectedNodeTickContext const&,
        size_t,
        size_t) = nullptr;
    void (*skip_block)(
        ReflectedNodeTickContext const&,
        size_t,
        size_t) = nullptr;
    ReflectedNodeOperations (*apply_detach_id_offset)(size_t) = nullptr;

    constexpr bool valid() const
    {
        return declare_node != nullptr && tick_block != nullptr
            && skip_block != nullptr && apply_detach_id_offset != nullptr;
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
    template<class Node>
    consteval ReflectedNodeDescription reflect_node(Node node);

    [[gnu::error(
        "GraphBuilder::node may only be called during constant evaluation")]]
    void runtime_graph_builder_node_call_is_forbidden();
}
}
