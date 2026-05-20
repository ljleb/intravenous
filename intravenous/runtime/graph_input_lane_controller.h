#pragma once

#include "basic_lane_nodes/controls.h"
#include "lane_node/graph.h"

#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace iv {
    struct GraphInputLaneBinding {
        GraphInputPortDescriptor port {};
        LaneId knob_lane {};
        LaneId graph_input_lane {};
        std::optional<LaneId> logical_knob_lane {};
    };

    struct GraphInputLaneBindings {
        std::vector<GraphInputLaneBinding> logical_sample_knobs {};
        std::vector<GraphInputLaneBinding> sample_inputs {};
        std::vector<GraphInputLaneBinding> event_inputs {};
    };

    class GraphInputLaneController {
        static std::string port_identity(GraphInputPortDescriptor const& port)
        {
            return port.logical_node_id
                + "\x1fkind:" + (port.port_kind == PortKind::sample ? "sample" : "event")
                + "\x1fordinal:" + std::to_string(port.port_ordinal);
        }

        static std::string port_descriptor_identity(GraphInputPortDescriptor const& port)
        {
            std::string key = port_identity(port);
            key += "\x1fmember:";
            key += port.concrete_member_ordinal.has_value()
                ? std::to_string(*port.concrete_member_ordinal)
                : "logical";
            return key;
        }

        static std::string logical_knob_identity(GraphInputPortDescriptor const& port)
        {
            return "graph-input:logical-knob:" + port_identity(port);
        }

        static std::string concrete_knob_identity(GraphInputPortDescriptor const& port)
        {
            return "graph-input:concrete-knob:" + port_descriptor_identity(port);
        }

        static LaneId find_knob_lane(LaneGraph const& graph, std::string const& semantic_identity)
        {
            LaneId found {};
            graph.for_each_lane([&](LaneRecord const& lane) {
                if (lane.metadata.semantic_identity == semantic_identity) {
                    found = lane.id;
                }
            });
            return found;
        }

        static LaneId find_graph_sample_input_lane(
            LaneGraph const& graph,
            GraphInputPortDescriptor const& port
        )
        {
            LaneId found {};
            graph.for_each_lane([&](LaneRecord const& lane) {
                if (
                    lane.metadata.graph_input_port.has_value()
                    && *lane.metadata.graph_input_port == port
                    && lane.node.try_as<GraphSampleInputLaneNode>()
                ) {
                    found = lane.id;
                }
            });
            return found;
        }

        static LaneId find_graph_event_input_lane(
            LaneGraph const& graph,
            GraphInputPortDescriptor const& port
        )
        {
            LaneId found {};
            graph.for_each_lane([&](LaneRecord const& lane) {
                if (
                    lane.metadata.graph_input_port.has_value()
                    && *lane.metadata.graph_input_port == port
                    && lane.node.try_as<GraphEventInputLaneNode>()
                ) {
                    found = lane.id;
                }
            });
            return found;
        }

        static bool has_concrete_descriptor_for_port(
            std::span<GraphInputPortDescriptor const> ports,
            GraphInputPortDescriptor const& logical_port
        )
        {
            for (auto const& port : ports) {
                if (
                    port.logical_node_id == logical_port.logical_node_id
                    && port.port_kind == logical_port.port_kind
                    && port.port_ordinal == logical_port.port_ordinal
                    && port.concrete_member_ordinal.has_value()
                ) {
                    return true;
                }
            }
            return false;
        }

    public:
        GraphInputLaneBindings reconcile(
            LaneGraph& graph,
            std::span<GraphInputPortDescriptor const> ports
        ) const
        {
            GraphInputLaneBindings result;
            std::unordered_map<std::string, LaneId> logical_sample_knobs;

            for (auto const& port : ports) {
                if (
                    port.port_kind != PortKind::sample
                    || port.concrete_member_ordinal.has_value()
                ) {
                    continue;
                }

                auto const identity = logical_knob_identity(port);
                LaneId logical_knob = find_knob_lane(graph, identity);
                if (!logical_knob) {
                    logical_knob = graph.add_lane(
                        KnobLaneNode {},
                        LaneMetadata {
                            .semantic_identity = identity,
                            .tags = { "graph-input", "knob", "logical" },
                            .graph_input_port = port,
                        }
                    );
                }
                logical_sample_knobs.emplace(port_identity(port), logical_knob);
                result.logical_sample_knobs.push_back(GraphInputLaneBinding {
                    .port = port,
                    .knob_lane = logical_knob,
                });
            }

            for (auto const& port : ports) {
                if (port.port_kind != PortKind::sample) {
                    continue;
                }
                if (
                    !port.concrete_member_ordinal.has_value()
                    && has_concrete_descriptor_for_port(ports, port)
                ) {
                    continue;
                }

                std::optional<LaneId> logical_knob;
                if (auto it = logical_sample_knobs.find(port_identity(port)); it != logical_sample_knobs.end()) {
                    logical_knob = it->second;
                }

                auto const identity = concrete_knob_identity(port);
                LaneId concrete_knob = find_knob_lane(graph, identity);
                bool const concrete_knob_existed = static_cast<bool>(concrete_knob);
                if (!concrete_knob) {
                    concrete_knob = graph.add_lane(
                        KnobLaneNode {},
                        LaneMetadata {
                            .semantic_identity = identity,
                            .tags = { "graph-input", "knob", "concrete" },
                            .graph_input_port = port,
                        }
                    );
                }

                LaneId graph_input = find_graph_sample_input_lane(graph, port);
                bool const graph_input_existed = static_cast<bool>(graph_input);
                if (!graph_input) {
                    graph_input = graph.add_lane(
                        GraphSampleInputLaneNode {},
                        LaneMetadata {
                            .tags = { "graph-input", "sample" },
                            .graph_input_port = port,
                        }
                    );
                }

                if (
                    logical_knob.has_value()
                    && *logical_knob != concrete_knob
                    && !concrete_knob_existed
                ) {
                    graph.connect(*logical_knob, concrete_knob, realtime_sample_input());
                }
                if (!graph_input_existed) {
                    graph.connect(concrete_knob, graph_input, realtime_sample_input());
                }
                graph.add_child(concrete_knob, graph_input);

                result.sample_inputs.push_back(GraphInputLaneBinding {
                    .port = port,
                    .knob_lane = concrete_knob,
                    .graph_input_lane = graph_input,
                    .logical_knob_lane = logical_knob,
                });
            }

            for (auto const& port : ports) {
                if (port.port_kind != PortKind::event) {
                    continue;
                }
                if (
                    !port.concrete_member_ordinal.has_value()
                    && has_concrete_descriptor_for_port(ports, port)
                ) {
                    continue;
                }

                LaneId graph_input = find_graph_event_input_lane(graph, port);
                if (!graph_input) {
                    graph_input = graph.add_lane(
                        GraphEventInputLaneNode {},
                        LaneMetadata {
                            .tags = { "graph-input", "event" },
                            .graph_input_port = port,
                        }
                    );
                }
                result.event_inputs.push_back(GraphInputLaneBinding {
                    .port = port,
                    .graph_input_lane = graph_input,
                });
            }

            return result;
        }

        void set_sample_value(
            LaneGraph& graph,
            GraphInputPortDescriptor const& port,
            Sample value
        ) const
        {
            auto const identity = port.concrete_member_ordinal.has_value()
                ? concrete_knob_identity(port)
                : logical_knob_identity(port);
            graph.for_each_lane([&](LaneRecord& lane) {
                auto* knob = lane.node.try_as<KnobLaneNode>();
                if (!knob || lane.metadata.semantic_identity != identity) {
                    return;
                }
                knob->value = value;
                graph.clear_realtime_sample_caches();
                if (port.concrete_member_ordinal.has_value()) {
                    graph.disconnect_input(lane.id, realtime_sample_input());
                }
            });
        }

        void restore_sample_inheritance(
            LaneGraph& graph,
            GraphInputPortDescriptor const& port
        ) const
        {
            if (!port.concrete_member_ordinal.has_value()) {
                return;
            }

            GraphInputPortDescriptor logical_port = port;
            logical_port.concrete_member_ordinal = std::nullopt;

            LaneId logical_knob;
            LaneId concrete_knob;
            auto const logical_identity = logical_knob_identity(logical_port);
            auto const concrete_identity = concrete_knob_identity(port);
            graph.for_each_lane([&](LaneRecord const& lane) {
                auto const* knob = lane.node.try_as<KnobLaneNode>();
                if (!knob) {
                    return;
                }
                if (lane.metadata.semantic_identity == logical_identity) {
                    logical_knob = lane.id;
                } else if (lane.metadata.semantic_identity == concrete_identity) {
                    concrete_knob = lane.id;
                }
            });
            if (logical_knob && concrete_knob) {
                graph.connect_exclusive(logical_knob, concrete_knob, realtime_sample_input());
            }
        }
    };
}
