#pragma once

#include <intravenous/basic_lane_nodes/controls.h>
#include <intravenous/lane_node/graph.h>

#include <optional>
#include <span>
#include <string>
#include <functional>
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
        static constexpr char const* metadata_graph_input = "graph_input";
        static constexpr char const* metadata_knob = "knob";
        static constexpr char const* metadata_logical = "logical";
        static constexpr char const* metadata_concrete = "concrete";
        static constexpr char const* metadata_sample = "sample";
        static constexpr char const* metadata_event = "event";
        static constexpr char const* metadata_identity_hash = "identity_hash";
        static constexpr char const* metadata_node_hash = "node_hash";
        static constexpr char const* metadata_port_kind = "port_kind";
        static constexpr char const* metadata_port_ordinal = "port_ordinal";
        static constexpr char const* metadata_member_ordinal = "member_ordinal";

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

        static int hash_string(std::string const& value)
        {
            return static_cast<int>(std::hash<std::string>{}(value));
        }

        static bool has_unit_metadata(LaneMetadata const& metadata, std::string_view key)
        {
            return metadata.has_unit(key);
        }

        static std::optional<int> int_metadata(LaneMetadata const& metadata, std::string_view key)
        {
            return metadata.int_value(key);
        }

        static LaneMetadata graph_input_metadata(
            GraphInputPortDescriptor const& port,
            bool knob,
            bool logical,
            bool concrete,
            bool sample,
            bool event,
            std::optional<int> identity_hash = std::nullopt)
        {
            LaneMetadata metadata;
            metadata.set_unit(metadata_graph_input);
            if (knob) {
                metadata.set_unit(metadata_knob);
            }
            if (logical) {
                metadata.set_unit(metadata_logical);
            }
            if (concrete) {
                metadata.set_unit(metadata_concrete);
            }
            if (sample) {
                metadata.set_unit(metadata_sample);
            }
            if (event) {
                metadata.set_unit(metadata_event);
            }
            metadata.set_int(metadata_node_hash, hash_string(port.logical_node_id));
            metadata.set_int(metadata_port_kind, static_cast<int>(port.port_kind == PortKind::event));
            metadata.set_int(metadata_port_ordinal, static_cast<int>(port.port_ordinal));
            if (port.concrete_member_ordinal.has_value()) {
                metadata.set_int(
                    metadata_member_ordinal,
                    static_cast<int>(*port.concrete_member_ordinal));
            }
            if (identity_hash.has_value()) {
                metadata.set_int(metadata_identity_hash, *identity_hash);
            }
            return metadata;
        }

        static bool metadata_matches_port(LaneMetadata const& metadata, GraphInputPortDescriptor const& port)
        {
            auto const node_hash = int_metadata(metadata, metadata_node_hash);
            auto const port_kind = int_metadata(metadata, metadata_port_kind);
            auto const port_ordinal = int_metadata(metadata, metadata_port_ordinal);
            if (!node_hash.has_value() || !port_kind.has_value() || !port_ordinal.has_value()) {
                return false;
            }
            if (*node_hash != hash_string(port.logical_node_id)) {
                return false;
            }
            if (*port_kind != static_cast<int>(port.port_kind == PortKind::event)) {
                return false;
            }
            if (*port_ordinal != static_cast<int>(port.port_ordinal)) {
                return false;
            }
            auto const member_ordinal = int_metadata(metadata, metadata_member_ordinal);
            if (port.concrete_member_ordinal.has_value()) {
                return member_ordinal.has_value() &&
                    *member_ordinal == static_cast<int>(*port.concrete_member_ordinal);
            }
            return !member_ordinal.has_value();
        }

        static LaneId find_knob_lane(LaneGraph const& graph, int identity_hash)
        {
            LaneId found {};
            graph.for_each_lane([&](LaneRecord const& lane) {
                if (int_metadata(lane.metadata, metadata_identity_hash) == identity_hash) {
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
                    has_unit_metadata(lane.metadata, metadata_graph_input)
                    && has_unit_metadata(lane.metadata, metadata_sample)
                    && metadata_matches_port(lane.metadata, port)
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
                    has_unit_metadata(lane.metadata, metadata_graph_input)
                    && has_unit_metadata(lane.metadata, metadata_event)
                    && metadata_matches_port(lane.metadata, port)
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
                auto const identity_hash = hash_string(identity);
                LaneId logical_knob = find_knob_lane(graph, identity_hash);
                if (!logical_knob) {
                    logical_knob = graph.add_lane(
                        KnobLaneNode {},
                        graph_input_metadata(
                            port,
                            true,
                            true,
                            false,
                            false,
                            false,
                            identity_hash)
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
                auto const identity_hash = hash_string(identity);
                LaneId concrete_knob = find_knob_lane(graph, identity_hash);
                bool const concrete_knob_existed = static_cast<bool>(concrete_knob);
                if (!concrete_knob) {
                    concrete_knob = graph.add_lane(
                        KnobLaneNode {},
                        graph_input_metadata(
                            port,
                            true,
                            false,
                            true,
                            false,
                            false,
                            identity_hash)
                    );
                }

                LaneId graph_input = find_graph_sample_input_lane(graph, port);
                bool const graph_input_existed = static_cast<bool>(graph_input);
                if (!graph_input) {
                    graph_input = graph.add_lane(
                        GraphSampleInputLaneNode {},
                        graph_input_metadata(
                            port,
                            false,
                            false,
                            false,
                            true,
                            false)
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
                        graph_input_metadata(
                            port,
                            false,
                            false,
                            false,
                            false,
                            true)
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
            auto const identity_hash = hash_string(identity);
            graph.for_each_lane([&](LaneRecord& lane) {
                auto* knob = lane.node.try_as<KnobLaneNode>();
                if (!knob || int_metadata(lane.metadata, metadata_identity_hash) != identity_hash) {
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
            auto const logical_hash = hash_string(logical_identity);
            auto const concrete_hash = hash_string(concrete_identity);
            graph.for_each_lane([&](LaneRecord const& lane) {
                auto const* knob = lane.node.try_as<KnobLaneNode>();
                if (!knob) {
                    return;
                }
                if (int_metadata(lane.metadata, metadata_identity_hash) == logical_hash) {
                    logical_knob = lane.id;
                } else if (int_metadata(lane.metadata, metadata_identity_hash) == concrete_hash) {
                    concrete_knob = lane.id;
                }
            });
            if (logical_knob && concrete_knob) {
                graph.connect_exclusive(logical_knob, concrete_knob, realtime_sample_input());
            }
        }
    };
}
