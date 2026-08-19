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
        std::vector<SampleInputChannelId> targets {};
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
        std::vector<SampleOutputChannelId> sources {};
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

    // Semantic sample edge recorded exactly as the builder saw it. Channel
    // vector order is the semantic channel order; the two channel types retain
    // grouping independently of the backing bundle-port layouts.
    struct AuthoredSampleConnection {
        ChannelTypeId source_type = ChannelTypeId::mono;
        std::vector<SampleOutputChannelId> source_channels {};
        ChannelTypeId target_type = ChannelTypeId::mono;
        std::vector<SampleInputChannelId> target_channels {};

        bool operator==(AuthoredSampleConnection const&) const = default;
    };

    class GraphBuilderConnections {
    public:
        void record_authored_sample_connection(AuthoredSampleConnection);
        std::span<AuthoredSampleConnection const> authored_sample_connections() const;
        bool sample_input_is_connected(SampleInputChannelId target) const;
        bool sample_output_is_connected(SampleOutputChannelId source) const;
        bool sample_input_is_runtime_filled(SampleInputChannelId target) const;
        // Compatibility topology state remains until sample lowering moves to
        // completion in the next migration step.
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
        void mark_runtime_filled_sample_input(SampleInputChannelId target);
        void mark_runtime_filled_sample_input(TopologyPortId target);
        void mark_runtime_filled_event_input(TopologyPortId target);
        GraphBuilderVacantInputs collect_vacant_inputs(
            GraphBuilderTopology const&, GraphBuilderNodeBundles const&,
            GraphBuilderVirtualNodes const&) const;
        GraphBuilderVirtualInputs collect_virtual_inputs(
            GraphBuilderTopology const&, GraphBuilderNodeBundles const&,
            GraphBuilderVirtualNodes const&) const;
        GraphBuilderVirtualSampleInputFamilies collect_virtual_sample_input_families(
            GraphBuilderNodeBundles const&, GraphBuilderVirtualNodes const&) const;
        GraphBuilderVirtualOutputs collect_virtual_outputs(
            GraphBuilderTopology const&, GraphBuilderNodeBundles const&,
            GraphBuilderVirtualNodes const&) const;
        GraphBuilderVirtualSampleOutputFamilies collect_virtual_sample_output_families(
            GraphBuilderNodeBundles const&, GraphBuilderVirtualNodes const&) const;
        void import_child(GraphBuilderConnections const& child,
                          size_t child_node_offset,
                          size_t child_node_bundle_offset);
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
        std::vector<AuthoredSampleConnection> _authored_sample_connections {};
        std::vector<SampleInputChannelId> _runtime_filled_sample_channels {};
        // Compatibility lowering state. Sample-side builder introspection must
        // not consult these topology-addressed sets.
        std::unordered_set<TopologyPortId> _placed_sample_inputs {};
        std::unordered_set<TopologyPortId> _placed_event_inputs {};
        std::unordered_set<TopologyPortId> _runtime_filled_sample_inputs {};
        std::unordered_set<TopologyPortId> _runtime_filled_event_inputs {};
    };
}
