#pragma once

#include <intravenous/graph/node.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace iv {
    struct NodePorts {
        std::vector<InputConfig> sample_inputs {};
        std::vector<OutputConfig> sample_outputs {};
        std::vector<EventInputConfig> event_inputs {};
        std::vector<EventOutputConfig> event_outputs {};

        std::vector<InputConfig> const& inputs() const;
        std::vector<OutputConfig> const& outputs() const;
        std::vector<EventInputConfig> const& event_inputs_view() const;
        std::vector<EventOutputConfig> const& event_outputs_view() const;
    };

    struct NodeMaterialization {
        std::function<TypeErasedNode(size_t)> factory {};

        bool is_placeholder() const;
        TypeErasedNode make(size_t detach_id_offset) const;
    };

    struct NodeLifetime {
        std::optional<size_t> ttl_samples {};
    };

    struct LoweredSubgraphBinding {
        size_t begin = 0;
        size_t count = 0;
        std::vector<std::vector<PortId>> sample_input_targets {};
        std::vector<PortId> sample_output_sources {};
        std::vector<std::vector<PortId>> event_input_targets {};
        std::vector<PortId> event_output_sources {};
        std::string kind {};

        bool active() const;
    };

    struct NodeSourceAnnotations {
        std::vector<SourceInfo> infos {};
    };

    struct LogicalNodeBinding {
        std::vector<std::string> ids {};
    };

    struct VacantInputOwnership {
        std::string logical_node_id {};
    };

    struct NodeTypeIdentity {
        std::string value {};
    };

    struct BuilderNode {
        NodePorts ports {};
        NodeMaterialization materialization {};
        NodeLifetime lifetime {};
        LoweredSubgraphBinding lowered_subgraph {};
        NodeSourceAnnotations source_annotations {};
        LogicalNodeBinding logical_binding {};
        VacantInputOwnership vacant_input_ownership {};
        NodeTypeIdentity type_identity {};

        std::vector<InputConfig> const& inputs() const;
        std::vector<OutputConfig> const& outputs() const;
        std::vector<EventInputConfig> const& event_inputs() const;
        std::vector<EventOutputConfig> const& event_outputs() const;
    };
}
