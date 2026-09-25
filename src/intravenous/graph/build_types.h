#pragma once

#include <intravenous/graph/source_info.h>
#include <intravenous/ports.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace iv {
    enum class VirtualPortConnectivity : std::uint8_t {
        disconnected,
        connected,
        mixed,
    };

    struct IntrospectionPortInfo {
        std::string name {};
        std::string type {};
        VirtualPortConnectivity connectivity = VirtualPortConnectivity::disconnected;
        size_t index = 0;
        Sample default_value = 0.0f;
        std::optional<Sample> min {};
        std::optional<Sample> max {};
        size_t history = 0;
        size_t latency = 0;
        std::optional<ChannelTypeId> sample_channel_type {};
        std::vector<SourceSpan> source_spans {};

        bool operator==(IntrospectionPortInfo const&) const = default;
    };

    struct VirtualPortInfo {
        std::string name {};
        std::string type {};
        VirtualPortConnectivity connectivity = VirtualPortConnectivity::disconnected;
        size_t index = 0;
        Sample default_value = 0.0f;
        std::optional<Sample> min {};
        std::optional<Sample> max {};
        Sample current_value = 0.0f;
        bool has_concrete_override = false;
        std::optional<ChannelTypeId> sample_channel_type {};
        std::string state_value {};

        bool operator==(VirtualPortInfo const&) const = default;
    };

    struct IntrospectionVirtualNode {
        struct Member {
            size_t index = 0;
            std::string backing_node_id {};
            std::string kind {};
            std::string type_identity {};
            std::vector<IntrospectionPortInfo> sample_inputs {};
            std::vector<IntrospectionPortInfo> sample_outputs {};
            std::vector<IntrospectionPortInfo> event_inputs {};
            std::vector<IntrospectionPortInfo> event_outputs {};
        };

        std::string id {};
        std::string kind {};
        std::string source_identity {};
        std::string type_identity {};
        std::vector<SourceSpan> source_spans {};
        std::vector<IntrospectionPortInfo> sample_inputs {};
        std::vector<IntrospectionPortInfo> sample_outputs {};
        std::vector<IntrospectionPortInfo> event_inputs {};
        std::vector<IntrospectionPortInfo> event_outputs {};
        std::vector<std::string> backing_node_ids {};
        std::vector<Member> members {};
    };

    struct GraphBuilderPublicSamplePortChannel {
        std::vector<size_t> port_indices {};
        std::vector<SourceInfo> source_infos {};
    };

    struct GraphBuilderPublicSamplePortFamily {
        size_t family_index = 0;
        std::string family_name {};
        SampleInputConfig input_config {};
        SampleOutputConfig output_config {};
        ChannelTypeId channel_type = ChannelTypeId::mono;
        std::vector<GraphBuilderPublicSamplePortChannel> channels {};
        std::vector<SourceInfo> source_infos {};
        bool configured_connected = false;
    };

    struct GraphBuilderPublicSamplePortFamilies {
        std::vector<GraphBuilderPublicSamplePortFamily> families {};
    };

    struct GraphBuilderPublicEventInput {
        size_t port_index = 0;
        EventInputConfig config {};
        std::vector<SourceInfo> source_infos {};
        bool graph_connected = false;
    };

    struct GraphBuilderPublicEventOutput {
        size_t port_index = 0;
        EventOutputConfig config {};
        std::vector<SourceInfo> source_infos {};
    };

    struct GraphIntrospectionMetadata {
        std::vector<IntrospectionVirtualNode> virtual_nodes;
        std::vector<GraphBuilderPublicSamplePortFamily> public_sample_inputs;
        std::vector<GraphBuilderPublicEventInput> public_event_inputs;
        std::vector<GraphBuilderPublicSamplePortFamily> public_sample_outputs;
        std::vector<GraphBuilderPublicEventOutput> public_event_outputs;
    };

}
