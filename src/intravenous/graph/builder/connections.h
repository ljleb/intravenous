#pragma once

#include <intravenous/graph/builder/identity.h>
#include <intravenous/graph/builder/port_refs.h>
#include <intravenous/channel_layout.h>

#include <optional>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

namespace iv {
    class GraphBuilderTopology;
    class GraphBuilderNodeBundles;
    class GraphBuilderVirtualNodes;

    struct GraphBuilderVacantSampleInput {
        TopologyPortId target {};
        std::string virtual_node_id {};
        size_t member_ordinal = 0;
        InputConfig config {};
    };

    struct GraphBuilderVacantEventInput {
        TopologyPortId target {};
        std::string virtual_node_id {};
        size_t member_ordinal = 0;
        EventInputConfig config {};
    };

    struct GraphBuilderVacantInputs {
        std::vector<GraphBuilderVacantSampleInput> sample {};
        std::vector<GraphBuilderVacantEventInput> event {};
    };

    struct GraphBuilderVirtualSampleInput {
        TopologyPortId target {};
        std::string virtual_node_id {};
        size_t member_ordinal = 0;
        InputConfig config {};
        bool has_existing_connection = false;
        bool runtime_filled = false;
    };

    struct GraphBuilderVirtualEventInput {
        TopologyPortId target {};
        std::string virtual_node_id {};
        size_t member_ordinal = 0;
        EventInputConfig config {};
        bool has_existing_connection = false;
        bool runtime_filled = false;
    };

    struct GraphBuilderVirtualInputs {
        std::vector<GraphBuilderVirtualSampleInput> sample {};
        std::vector<GraphBuilderVirtualEventInput> event {};
    };

    struct GraphBuilderVirtualSampleInputChannel {
        std::vector<TopologyPortId> targets {};
        bool has_existing_connection = false;
        bool runtime_filled = false;
    };

    struct GraphBuilderVirtualSampleInputFamily {
        std::string virtual_node_id {};
        size_t member_ordinal = 0;
        size_t family_ordinal = 0;
        std::string family_name {};
        InputConfig config {};
        ChannelTypeId channel_type = ChannelTypeId::mono;
        std::vector<GraphBuilderVirtualSampleInputChannel> channels {};
    };

    struct GraphBuilderVirtualSampleInputFamilies {
        std::vector<GraphBuilderVirtualSampleInputFamily> families {};
    };

    struct GraphBuilderVirtualSampleOutput {
        TopologyPortId source {};
        std::string virtual_node_id {};
        size_t member_ordinal = 0;
        OutputConfig config {};
        bool has_existing_downstream_connection = false;
    };

    struct GraphBuilderVirtualEventOutput {
        TopologyPortId source {};
        std::string virtual_node_id {};
        size_t member_ordinal = 0;
        EventOutputConfig config {};
        bool has_existing_downstream_connection = false;
    };

    struct GraphBuilderVirtualOutputs {
        std::vector<GraphBuilderVirtualSampleOutput> sample {};
        std::vector<GraphBuilderVirtualEventOutput> event {};
    };

    struct GraphBuilderVirtualSampleOutputChannel {
        std::vector<TopologyPortId> sources {};
        bool has_existing_downstream_connection = false;
    };

    struct GraphBuilderVirtualSampleOutputFamily {
        std::string virtual_node_id {};
        size_t member_ordinal = 0;
        size_t family_ordinal = 0;
        std::string family_name {};
        OutputConfig config {};
        ChannelTypeId channel_type = ChannelTypeId::mono;
        std::vector<GraphBuilderVirtualSampleOutputChannel> channels {};
    };

    struct GraphBuilderVirtualSampleOutputFamilies {
        std::vector<GraphBuilderVirtualSampleOutputFamily> families {};
    };

    class GraphBuilderConnections {
    public:
        bool sample_input_is_connected(TopologyPortId target) const;
        bool event_input_is_connected(TopologyPortId target) const;
        void connect_sample_input(
            GraphBuilderTopology&,
            GraphBuilderIdentity const&,
            TopologyPortId target,
            TopologyPortId source
        );
        void connect_event_input(
            GraphBuilderTopology&,
            std::span<EventInputConfig const> graph_event_inputs,
            GraphBuilderIdentity const&,
            TopologyPortId target,
            EventPortRef source
        );
        void mark_runtime_filled_sample_input(TopologyPortId target);
        void mark_runtime_filled_event_input(TopologyPortId target);
        GraphBuilderVacantInputs collect_vacant_inputs(
            GraphBuilderTopology const&, GraphBuilderVirtualNodes const&) const;
        GraphBuilderVirtualInputs collect_virtual_inputs(
            GraphBuilderTopology const&, GraphBuilderVirtualNodes const&) const;
        GraphBuilderVirtualSampleInputFamilies collect_virtual_sample_input_families(
            GraphBuilderNodeBundles const&, GraphBuilderVirtualNodes const&) const;
        GraphBuilderVirtualOutputs collect_virtual_outputs(
            GraphBuilderTopology const&, GraphBuilderVirtualNodes const&) const;
        GraphBuilderVirtualSampleOutputFamilies collect_virtual_sample_output_families(
            GraphBuilderTopology const&, GraphBuilderNodeBundles const&,
            GraphBuilderVirtualNodes const&) const;
        void import_child(GraphBuilderConnections const& child, size_t child_node_offset);
        template<class Fn>
        void for_each_runtime_filled_sample_input(Fn&& fn) const
        {
            for (TopologyPortId const port : _runtime_filled_sample_inputs) {
                fn(port);
            }
        }
        template<class Fn>
        void for_each_runtime_filled_event_input(Fn&& fn) const
        {
            for (TopologyPortId const port : _runtime_filled_event_inputs) {
                fn(port);
            }
        }

    private:
        std::unordered_set<TopologyPortId> _placed_sample_inputs {};
        std::unordered_set<TopologyPortId> _placed_event_inputs {};
        std::unordered_set<TopologyPortId> _runtime_filled_sample_inputs {};
        std::unordered_set<TopologyPortId> _runtime_filled_event_inputs {};
    };
}
