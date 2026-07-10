#pragma once

#include <intravenous/graph/builder/names.h>

#include <algorithm>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace iv::details {
    struct LogicalConcretePortInfo {
        std::string name;
        std::string type;
        bool connected = false;
        size_t history = 0;
        size_t latency = 0;
        Sample default_value = 0.0f;
    };

    struct LogicalConcreteNode {
        std::string id;
        std::string kind;
        std::string type_identity;
        size_t construction_order = 0;
        std::vector<SourceInfo> source_infos;
        std::vector<LogicalConcretePortInfo> sample_inputs;
        std::vector<LogicalConcretePortInfo> sample_outputs;
        std::vector<LogicalConcretePortInfo> event_inputs;
        std::vector<LogicalConcretePortInfo> event_outputs;
    };

    struct MetadataGraphNode {
        std::vector<InputConfig> input_configs;
        std::vector<OutputConfig> output_configs;
        std::vector<EventInputConfig> event_input_configs;
        std::vector<EventOutputConfig> event_output_configs;

        std::vector<InputConfig> const& inputs() const
        {
            return input_configs;
        }

        std::vector<OutputConfig> const& outputs() const
        {
            return output_configs;
        }

        std::vector<EventInputConfig> const& event_inputs() const
        {
            return event_input_configs;
        }

        std::vector<EventOutputConfig> const& event_outputs() const
        {
            return event_output_configs;
        }

        void tick(TickSampleContext<MetadataGraphNode> const&) const
        {
        }
    };

    LogicalPortConnectivity aggregate_connectivity(std::span<LogicalConcretePortInfo const> ports);
    void sort_and_deduplicate_spans(std::vector<SourceSpan>& spans);
    std::vector<SourceSpan> source_spans_for(
        std::span<LogicalConcreteNode const* const> nodes
    );

    inline std::vector<IntrospectionPortInfo> aggregate_ports(
        std::span<LogicalConcreteNode const* const> nodes,
        auto LogicalConcreteNode::* member
    )
    {
        if (nodes.empty()) {
            return {};
        }

        auto const& first_ports = nodes.front()->*member;
        std::vector<IntrospectionPortInfo> logical_ports;
        logical_ports.reserve(first_ports.size());
        for (size_t i = 0; i < first_ports.size(); ++i) {
            std::vector<LogicalConcretePortInfo> concrete_ports;
            concrete_ports.reserve(nodes.size());
            for (auto const* node : nodes) {
                concrete_ports.push_back((node->*member)[i]);
            }

            logical_ports.push_back(IntrospectionPortInfo {
                .name = first_ports[i].name,
                .type = first_ports[i].type,
                .connectivity = aggregate_connectivity(concrete_ports),
                .ordinal = i,
                .default_value = first_ports[i].default_value,
                .history = first_ports[i].history,
                .latency = first_ports[i].latency,
            });
        }
        return logical_ports;
    }
    using LogicalMetadataBuildResult =
        std::pair<std::vector<IntrospectionLogicalNode>, std::unordered_map<std::string, std::vector<std::string>>>;

    LogicalMetadataBuildResult build_logical_metadata(
        PreparedGraph const& g,
        std::span<LoweredSubgraphSpec const> lowered_scopes
    );
}
