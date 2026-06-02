#pragma once

#include "identity.h"
#include "port_refs.h"

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
