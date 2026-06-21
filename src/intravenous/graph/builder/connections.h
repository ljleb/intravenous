#pragma once

#include <intravenous/graph/builder/identity.h>
#include <intravenous/graph/builder/port_refs.h>
#include <intravenous/lane_node/channels.h>

#include <optional>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

namespace iv {
    class GraphBuilderTopology;

    struct GraphBuilderVacantSampleInput {
        PortId target {};
        std::string logical_node_id {};
        size_t member_ordinal = 0;
        InputConfig config {};
    };

    struct GraphBuilderVacantEventInput {
        PortId target {};
        std::string logical_node_id {};
        size_t member_ordinal = 0;
        EventInputConfig config {};
    };

    struct GraphBuilderVacantInputs {
        std::vector<GraphBuilderVacantSampleInput> sample {};
        std::vector<GraphBuilderVacantEventInput> event {};
    };

    struct GraphBuilderLogicalSampleInput {
        PortId target {};
        std::string logical_node_id {};
        size_t member_ordinal = 0;
        InputConfig config {};
        bool has_existing_connection = false;
        bool runtime_filled = false;
    };

    struct GraphBuilderLogicalEventInput {
        PortId target {};
        std::string logical_node_id {};
        size_t member_ordinal = 0;
        EventInputConfig config {};
        bool has_existing_connection = false;
        bool runtime_filled = false;
    };

    struct GraphBuilderLogicalInputs {
        std::vector<GraphBuilderLogicalSampleInput> sample {};
        std::vector<GraphBuilderLogicalEventInput> event {};
    };

    struct GraphBuilderLogicalSampleInputChannel {
        std::optional<PortId> target {};
        bool has_existing_connection = false;
        bool runtime_filled = false;
    };

    struct GraphBuilderLogicalSampleInputFamily {
        std::string logical_node_id {};
        size_t member_ordinal = 0;
        size_t family_ordinal = 0;
        std::string family_name {};
        InputConfig config {};
        ChannelTypeId channel_type = ChannelTypeId::mono;
        std::vector<GraphBuilderLogicalSampleInputChannel> channels {};
    };

    struct GraphBuilderLogicalSampleInputFamilies {
        std::vector<GraphBuilderLogicalSampleInputFamily> families {};
    };

    struct GraphBuilderLogicalSampleOutput {
        PortId source {};
        std::string logical_node_id {};
        size_t member_ordinal = 0;
        OutputConfig config {};
        bool has_existing_downstream_connection = false;
    };

    struct GraphBuilderLogicalEventOutput {
        PortId source {};
        std::string logical_node_id {};
        size_t member_ordinal = 0;
        EventOutputConfig config {};
        bool has_existing_downstream_connection = false;
    };

    struct GraphBuilderLogicalOutputs {
        std::vector<GraphBuilderLogicalSampleOutput> sample {};
        std::vector<GraphBuilderLogicalEventOutput> event {};
    };

    struct GraphBuilderLogicalSampleOutputChannel {
        std::optional<PortId> source {};
        bool has_existing_downstream_connection = false;
    };

    struct GraphBuilderLogicalSampleOutputFamily {
        std::string logical_node_id {};
        size_t member_ordinal = 0;
        size_t family_ordinal = 0;
        std::string family_name {};
        OutputConfig config {};
        ChannelTypeId channel_type = ChannelTypeId::mono;
        std::vector<GraphBuilderLogicalSampleOutputChannel> channels {};
    };

    struct GraphBuilderLogicalSampleOutputFamilies {
        std::vector<GraphBuilderLogicalSampleOutputFamily> families {};
    };

    class GraphBuilderConnections {
    public:
        bool sample_input_is_connected(PortId target) const;
        bool event_input_is_connected(PortId target) const;
        void connect_sample_input(
            GraphBuilderTopology&,
            GraphBuilderIdentity const&,
            PortId target,
            SamplePortRef source
        );
        void connect_event_input(
            GraphBuilderTopology&,
            std::span<EventInputConfig const> graph_event_inputs,
            GraphBuilderIdentity const&,
            PortId target,
            EventPortRef source
        );
        void mark_runtime_filled_sample_input(PortId target);
        void mark_runtime_filled_event_input(PortId target);
        GraphBuilderVacantInputs collect_vacant_inputs(GraphBuilderTopology const&) const;
        GraphBuilderLogicalInputs collect_logical_inputs(GraphBuilderTopology const&) const;
        GraphBuilderLogicalSampleInputFamilies collect_logical_sample_input_families(
            GraphBuilderTopology const&) const;
        GraphBuilderLogicalOutputs collect_logical_outputs(GraphBuilderTopology const&) const;
        GraphBuilderLogicalSampleOutputFamilies collect_logical_sample_output_families(
            GraphBuilderTopology const&) const;
        void import_child(GraphBuilderConnections const& child, size_t child_node_offset);
        template<class Fn>
        void for_each_runtime_filled_sample_input(Fn&& fn) const
        {
            for (PortId const port : _runtime_filled_sample_inputs) {
                fn(port);
            }
        }
        template<class Fn>
        void for_each_runtime_filled_event_input(Fn&& fn) const
        {
            for (PortId const port : _runtime_filled_event_inputs) {
                fn(port);
            }
        }

    private:
        std::unordered_set<PortId> _placed_sample_inputs {};
        std::unordered_set<PortId> _placed_event_inputs {};
        std::unordered_set<PortId> _runtime_filled_sample_inputs {};
        std::unordered_set<PortId> _runtime_filled_event_inputs {};
    };
}
