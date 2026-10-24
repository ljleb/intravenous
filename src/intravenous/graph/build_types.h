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

    struct IntrospectionContinuousPortInfo {
        std::string name {};
        VirtualPortConnectivity connectivity = VirtualPortConnectivity::disconnected;
        size_t index = 0;
        std::vector<ContinuousValueIntrospectionMetadata> value_types {};
        std::optional<ChannelTypeId> channel_type {};
        std::vector<SourceSpan> source_spans {};

        bool operator==(
            IntrospectionContinuousPortInfo const&) const = default;
    };

    struct IntrospectionEventPortInfo {
        std::string name {};
        VirtualPortConnectivity connectivity = VirtualPortConnectivity::disconnected;
        size_t index = 0;
        EventTypeId type = EventTypeId::empty;
        std::vector<SourceSpan> source_spans {};

        bool operator==(IntrospectionEventPortInfo const&) const = default;
    };

    struct IntrospectionVirtualNode {
        struct Member {
            size_t index = 0;
            std::string backing_node_id {};
            std::string kind {};
            std::string type_identity {};
            std::vector<IntrospectionContinuousPortInfo> continuous_inputs {};
            std::vector<IntrospectionContinuousPortInfo> continuous_outputs {};
            std::vector<IntrospectionEventPortInfo> event_inputs {};
            std::vector<IntrospectionEventPortInfo> event_outputs {};
        };

        std::string id {};
        std::string kind {};
        std::string source_identity {};
        std::string type_identity {};
        std::vector<SourceSpan> source_spans {};
        std::vector<IntrospectionContinuousPortInfo> continuous_inputs {};
        std::vector<IntrospectionContinuousPortInfo> continuous_outputs {};
        std::vector<IntrospectionEventPortInfo> event_inputs {};
        std::vector<IntrospectionEventPortInfo> event_outputs {};
        std::vector<std::string> backing_node_ids {};
        std::vector<Member> members {};
    };

    struct GraphBuilderPublicContinuousPortChannel {
        std::vector<size_t> port_indices {};
        std::vector<SourceInfo> source_infos {};
    };

    struct GraphBuilderPublicContinuousPortFamily {
        size_t family_index = 0;
        std::string family_name {};
        ContinuousInputConfig input_config {};
        ContinuousOutputConfig output_config {};
        ChannelTypeId channel_type = ChannelTypeId::mono;
        std::vector<GraphBuilderPublicContinuousPortChannel> channels {};
        std::vector<SourceInfo> source_infos {};
        bool configured_connected = false;
    };

    struct GraphBuilderPublicContinuousPortFamilies {
        std::vector<GraphBuilderPublicContinuousPortFamily> families {};
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
        std::vector<GraphBuilderPublicContinuousPortFamily> public_continuous_inputs;
        std::vector<GraphBuilderPublicEventInput> public_event_inputs;
        std::vector<GraphBuilderPublicContinuousPortFamily> public_continuous_outputs;
        std::vector<GraphBuilderPublicEventOutput> public_event_outputs;
    };

}
