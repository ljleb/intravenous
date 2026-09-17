#pragma once

// Host-side retained representation of one concrete node.  This is not part
// of the module-facing builder boundary; module code sends NodeBuildRequest
// to libiv_builder instead.

#include <intravenous/graph/node_ports.h>
#include <intravenous/graph/reflected_node_operations.h>
#include <intravenous/node/build_request.h>
#include <intravenous/node/config_relocations.h>
#include <intravenous/node/config_storage.h>
#include <intravenous/node/registered_type_identity.h>

#include <memory>
#include <optional>
#include <string_view>

namespace iv {

struct ReflectedNodeDescription {
    NodePorts ports {};
    ReflectedNodeOperations operations {};
    std::shared_ptr<void const> node_storage {};
    std::shared_ptr<NodeStateStructures const> state_structures_storage {};
    NodeConfigRelocations config_relocations {};
    NodeCodeKey code_key {};
    std::optional<RegisteredNodeTypeIdentity> registered_node_type_identity {};
    std::size_t node_size = 0;
    std::size_t node_alignment = 1;
    std::string_view type_name {};
    std::size_t internal_latency_samples = 0;
    std::size_t maximum_block_size = MAX_BLOCK_SIZE;
    std::optional<std::size_t> default_ttl_samples {};
    bool block_skippable = false;
    std::optional<Sample> static_sample_value {};

    constexpr std::vector<InputConfig> const& inputs() const
    {
        return ports.input_configs;
    }

    constexpr std::vector<OutputConfig> const& outputs() const
    {
        return ports.output_configs;
    }

    constexpr std::vector<SampleInputConfig> sample_inputs() const { return ports.sample_inputs(); }
    constexpr std::vector<SampleOutputConfig> sample_outputs() const { return ports.sample_outputs(); }
    constexpr std::vector<EventInputConfig> event_inputs() const { return ports.event_inputs(); }
    constexpr std::vector<EventOutputConfig> event_outputs() const { return ports.event_outputs(); }

    constexpr std::size_t internal_latency() const
    {
        return internal_latency_samples;
    }

    constexpr std::size_t max_block_size() const
    {
        return maximum_block_size;
    }

    constexpr std::optional<std::size_t> ttl_samples() const
    {
        return default_ttl_samples;
    }

    constexpr bool can_skip_block() const
    {
        return block_skippable;
    }
};

namespace details {

ReflectedNodeDescription materialize_node_description(
    NodeBuildRequest const&,
    std::shared_ptr<void const>,
    NodeConfigRelocations = {});

// Internal graph code occasionally creates built-in concrete nodes directly.
// This helper intentionally remains outside the DSL include path.
template<class Node>
ReflectedNodeDescription reflect_node(Node const& node)
{
    auto request = make_node_build_request(node);
    auto storage = copy_node_config_bytes(
        request.config, request.config_size, request.config_alignment);
    return materialize_node_description(request, std::move(storage));
}

} // namespace details
} // namespace iv
