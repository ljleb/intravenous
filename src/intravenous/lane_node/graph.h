#pragma once

#include <intravenous/basic_lane_nodes/type_erased.h>
#include <intravenous/lane_node/channels.h>
#include <intravenous/query/lane_query_schema.h>
#include <intravenous/runtime/lane_graph.h>

#include <algorithm>
#include <functional>
#include <optional>
#include <variant>
#include <memory>
#include <queue>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace iv {
    struct LanePortId {
        LanePortDomain domain = LanePortDomain::compiled;
        PortKind kind = PortKind::sample;
        size_t ordinal = 0;

        bool operator==(LanePortId const&) const = default;
    };

    inline LanePortId realtime_sample_input(size_t ordinal = 0)
    {
        return LanePortId {
            .domain = LanePortDomain::realtime,
            .kind = PortKind::sample,
            .ordinal = ordinal,
        };
    }

    inline LanePortId realtime_event_input(size_t ordinal = 0)
    {
        return LanePortId {
            .domain = LanePortDomain::realtime,
            .kind = PortKind::event,
            .ordinal = ordinal,
        };
    }

    struct LaneInputConnection {
        LaneId source {};
        LanePortId input {};

        bool operator==(LaneInputConnection const&) const = default;
    };

    struct LaneOutputConnection {
        LaneId target {};
        LanePortId input {};

        bool operator==(LaneOutputConnection const&) const = default;
    };

    struct LaneGraphConnection {
        LaneId source {};
        LaneId target {};
        LanePortId input {};

        bool operator==(LaneGraphConnection const&) const = default;
    };

    struct LaneMetadata {
        std::unordered_set<std::string> unit_values {};
        std::unordered_map<std::string, int> int_values {};
        std::unordered_map<std::string, float> float_values {};

        void set_unit(std::string key)
        {
            int_values.erase(key);
            float_values.erase(key);
            unit_values.insert(std::move(key));
        }

        void set_int(std::string key, int value)
        {
            unit_values.erase(key);
            float_values.erase(key);
            int_values[std::move(key)] = value;
        }

        void set_float(std::string key, float value)
        {
            unit_values.erase(key);
            int_values.erase(key);
            float_values[std::move(key)] = value;
        }

        [[nodiscard]] bool has_unit(std::string_view key) const
        {
            return unit_values.contains(std::string(key));
        }

        [[nodiscard]] std::optional<int> int_value(std::string_view key) const
        {
            auto const it = int_values.find(std::string(key));
            if (it == int_values.end()) {
                return std::nullopt;
            }
            return it->second;
        }

        [[nodiscard]] std::optional<float> float_value(std::string_view key) const
        {
            auto const it = float_values.find(std::string(key));
            if (it == float_values.end()) {
                return std::nullopt;
            }
            return it->second;
        }

        [[nodiscard]] std::vector<std::pair<std::string, query::LaneQueryValueType>> schema_entries() const
        {
            std::vector<std::pair<std::string, query::LaneQueryValueType>> entries;
            entries.reserve(unit_values.size() + int_values.size() + float_values.size());
            for (auto const &key : unit_values) {
                entries.emplace_back(key, query::LaneQueryValueType::unit);
            }
            for (auto const &[key, _] : int_values) {
                entries.emplace_back(key, query::LaneQueryValueType::int_);
            }
            for (auto const &[key, _] : float_values) {
                entries.emplace_back(key, query::LaneQueryValueType::float_);
            }
            return entries;
        }
    };

    inline PortKind lane_output_kind(LaneOutputConfig const& output)
    {
        return std::visit([](auto const& config) {
            using Config = std::remove_cvref_t<decltype(config)>;
            if constexpr (
                std::same_as<Config, CompiledEventLaneOutputConfig>
                || std::same_as<Config, RealtimeEventLaneOutputConfig>
            ) {
                return PortKind::event;
            } else {
                return PortKind::sample;
            }
        }, output);
    }

    inline std::optional<ChannelTypeId> default_sample_channel_type(LaneOutputConfig const& output)
    {
        return lane_output_kind(output) == PortKind::sample
            ? std::optional<ChannelTypeId>(ChannelTypeId::stereo)
            : std::nullopt;
    }

    class LaneGraph;

    struct LaneRecord {
        LaneId id {};
        TypeErasedLaneNode node {};
        LaneOutputConfig output {};
        std::optional<ChannelTypeId> sample_channel_type {};
        LaneMetadata metadata {};
        std::vector<std::string> external_task_dependencies {};
    };

    struct CompiledLaneRecord {
        LaneRecord lane {};
    };

    struct RealtimeLaneRecord {
        LaneRecord lane {};
    };

    struct LaneLocation {
        LanePortDomain domain = LanePortDomain::compiled;
        size_t index = 0;
    };

    struct LaneGraphDomainConnections {
        std::unordered_map<LaneId, std::vector<LaneInputConnection>, LaneIdHash> inputs_by_target;
        std::unordered_map<LaneId, std::vector<LaneOutputConnection>, LaneIdHash> outputs_by_source;
    };

    struct LaneGraphLaneStore {
        LaneIdAllocator ids;
        std::vector<CompiledLaneRecord> compiled;
        std::vector<RealtimeLaneRecord> realtime;
        std::unordered_map<LaneId, LaneLocation, LaneIdHash> indices;
    };

    struct LaneGraphDomainConnectionStore {
        LaneGraphDomainConnections compiled;
        LaneGraphDomainConnections realtime;
    };

    struct LaneGraphHierarchy {
        std::unordered_map<LaneId, std::vector<LaneId>, LaneIdHash> children_by_parent;
        std::unordered_map<LaneId, std::vector<LaneId>, LaneIdHash> parents_by_child;
    };

    class LaneGraph {
    public:

    private:
        LaneGraphLaneStore _lanes;
        LaneGraphDomainConnectionStore _connections;
        LaneGraphHierarchy _hierarchy;

        static std::vector<LaneInputConnection> const& empty_inputs()
        {
            static std::vector<LaneInputConnection> const empty;
            return empty;
        }

        static std::vector<LaneOutputConnection> const& empty_outputs()
        {
            static std::vector<LaneOutputConnection> const empty;
            return empty;
        }

        static std::vector<LaneId> const& empty_children()
        {
            static std::vector<LaneId> const empty;
            return empty;
        }

        LaneLocation location_for(LaneId id) const
        {
            auto const it = _lanes.indices.find(id);
            if (it == _lanes.indices.end()) {
                throw std::runtime_error("unknown lane id");
            }
            return it->second;
        }

        static LaneGraphDomainConnections& connections_for_domain(
            LanePortDomain domain,
            LaneGraphDomainConnections& compiled,
            LaneGraphDomainConnections& realtime
        )
        {
            return domain == LanePortDomain::compiled ? compiled : realtime;
        }

        static LaneGraphDomainConnections const& connections_for_domain(
            LanePortDomain domain,
            LaneGraphDomainConnections const& compiled,
            LaneGraphDomainConnections const& realtime
        )
        {
            return domain == LanePortDomain::compiled ? compiled : realtime;
        }

        static size_t input_count(TypeErasedLaneNode const& node, LanePortId input)
        {
            if (input.domain == LanePortDomain::compiled && input.kind == PortKind::sample) {
                return node.compiled_sample_inputs().size();
            }
            if (input.domain == LanePortDomain::compiled && input.kind == PortKind::event) {
                return node.compiled_event_inputs().size();
            }
            if (input.domain == LanePortDomain::realtime && input.kind == PortKind::sample) {
                return node.realtime_sample_inputs().size();
            }
            return node.realtime_event_inputs().size();
        }

        void validate_connection(LaneId source, LaneId target, LanePortId input) const
        {
            auto const& source_record = lane(source);
            auto const& target_record = lane(target);
            if (lane_output_kind(source_record.output) != input.kind) {
                throw std::runtime_error("lane connection kind mismatch");
            }
            auto const source_domain = lane_output_domain(source_record.output);
            if (!(source_domain == input.domain
                    || (source_domain == LanePortDomain::compiled
                        && input.domain == LanePortDomain::realtime))) {
                throw std::runtime_error("lane connection domain mismatch");
            }
            if (input.ordinal >= input_count(target_record.node, input)) {
                throw std::runtime_error("lane connection target input ordinal out of range");
            }
        }

    public:
        LaneGraph() = default;
        LaneGraph(LaneGraph const&) = delete;
        LaneGraph& operator=(LaneGraph const&) = delete;
        LaneGraph(LaneGraph&&) = delete;
        LaneGraph& operator=(LaneGraph&&) = delete;

        LaneId add_lane(
            TypeErasedLaneNode node,
            LaneMetadata metadata = {},
            std::optional<ChannelTypeId> sample_channel_type = std::nullopt)
        {
            LaneId const id = _lanes.ids.next();
            LaneOutputConfig output = node.output();
            auto record = LaneRecord {
                .id = id,
                .node = std::move(node),
                .output = std::move(output),
                .sample_channel_type = sample_channel_type.has_value()
                    ? sample_channel_type
                    : default_sample_channel_type(output),
                .metadata = std::move(metadata),
            };
            LanePortDomain const domain = lane_output_domain(record.output);
            if (domain == LanePortDomain::compiled) {
                _lanes.indices.emplace(id, LaneLocation {
                    .domain = domain,
                    .index = _lanes.compiled.size(),
                });
                _lanes.compiled.push_back(CompiledLaneRecord {
                    .lane = std::move(record),
                });
            } else {
                _lanes.indices.emplace(id, LaneLocation {
                    .domain = domain,
                    .index = _lanes.realtime.size(),
                });
                _lanes.realtime.push_back(RealtimeLaneRecord {
                    .lane = std::move(record),
                });
            }
            return id;
        }

        void upsert_lane(
            LaneId id,
            TypeErasedLaneNode node,
            LaneMetadata metadata = {},
            std::vector<std::string> external_task_dependencies = {},
            std::optional<ChannelTypeId> sample_channel_type = std::nullopt)
        {
            LaneOutputConfig output = node.output();
            LanePortDomain const domain = lane_output_domain(output);
            if (contains(id)) {
                auto const location = location_for(id);
                if (location.domain != domain) {
                    throw std::runtime_error("lane upsert cannot change output domain");
                }
                auto& record = lane(id);
                record.node = std::move(node);
                record.output = std::move(output);
                record.sample_channel_type = sample_channel_type.has_value()
                    ? sample_channel_type
                    : default_sample_channel_type(output);
                record.metadata = std::move(metadata);
                record.external_task_dependencies = std::move(external_task_dependencies);
                return;
            }

            _lanes.ids.observe(id);
            auto record = LaneRecord {
                .id = id,
                .node = std::move(node),
                .output = std::move(output),
                .sample_channel_type = sample_channel_type.has_value()
                    ? sample_channel_type
                    : default_sample_channel_type(output),
                .metadata = std::move(metadata),
                .external_task_dependencies = std::move(external_task_dependencies),
            };
            if (domain == LanePortDomain::compiled) {
                _lanes.indices.emplace(id, LaneLocation {
                    .domain = domain,
                    .index = _lanes.compiled.size(),
                });
                _lanes.compiled.push_back(CompiledLaneRecord {
                    .lane = std::move(record),
                });
            } else {
                _lanes.indices.emplace(id, LaneLocation {
                    .domain = domain,
                    .index = _lanes.realtime.size(),
                });
                _lanes.realtime.push_back(RealtimeLaneRecord {
                    .lane = std::move(record),
                });
            }
        }

        bool contains(LaneId id) const
        {
            return _lanes.indices.contains(id);
        }

        LaneRecord const& lane(LaneId id) const
        {
            auto const location = location_for(id);
            return location.domain == LanePortDomain::compiled
                ? _lanes.compiled[location.index].lane
                : _lanes.realtime[location.index].lane;
        }

        LaneRecord& lane(LaneId id)
        {
            auto const location = location_for(id);
            return location.domain == LanePortDomain::compiled
                ? _lanes.compiled[location.index].lane
                : _lanes.realtime[location.index].lane;
        }

        CompiledLaneRecord const& compiled_lane(LaneId id) const
        {
            auto const location = location_for(id);
            if (location.domain != LanePortDomain::compiled) {
                throw std::runtime_error("lane is not a compiled-output lane");
            }
            return _lanes.compiled[location.index];
        }

        CompiledLaneRecord& compiled_lane(LaneId id)
        {
            auto const location = location_for(id);
            if (location.domain != LanePortDomain::compiled) {
                throw std::runtime_error("lane is not a compiled-output lane");
            }
            return _lanes.compiled[location.index];
        }

        RealtimeLaneRecord const& realtime_lane(LaneId id) const
        {
            auto const location = location_for(id);
            if (location.domain != LanePortDomain::realtime) {
                throw std::runtime_error("lane is not a realtime-output lane");
            }
            return _lanes.realtime[location.index];
        }

        RealtimeLaneRecord& realtime_lane(LaneId id)
        {
            auto const location = location_for(id);
            if (location.domain != LanePortDomain::realtime) {
                throw std::runtime_error("lane is not a realtime-output lane");
            }
            return _lanes.realtime[location.index];
        }

        std::vector<CompiledLaneRecord> const& compiled_lanes() const
        {
            return _lanes.compiled;
        }

        std::vector<RealtimeLaneRecord> const& realtime_lanes() const
        {
            return _lanes.realtime;
        }

        size_t lane_count() const
        {
            return _lanes.compiled.size() + _lanes.realtime.size();
        }

        template<typename Fn>
        void for_each_lane(Fn&& fn) const
        {
            for (auto const& record : _lanes.compiled) {
                std::invoke(fn, record.lane);
            }
            for (auto const& record : _lanes.realtime) {
                std::invoke(fn, record.lane);
            }
        }

        template<typename Fn>
        void for_each_lane(Fn&& fn)
        {
            for (auto& record : _lanes.compiled) {
                std::invoke(fn, record.lane);
            }
            for (auto& record : _lanes.realtime) {
                std::invoke(fn, record.lane);
            }
        }

        void connect(LaneId source, LaneId target, LanePortId input)
        {
            validate_connection(source, target, input);

            LaneInputConnection input_connection {
                .source = source,
                .input = input,
            };
            auto& connections = connections_for_domain(input.domain, _connections.compiled, _connections.realtime);
            auto& inputs = connections.inputs_by_target[target];
            if (std::ranges::find(inputs, input_connection) == inputs.end()) {
                inputs.push_back(input_connection);
            }

            LaneOutputConnection output_connection {
                .target = target,
                .input = input,
            };
            auto& outputs = connections.outputs_by_source[source];
            if (std::ranges::find(outputs, output_connection) == outputs.end()) {
                outputs.push_back(output_connection);
            }
        }

        void disconnect(LaneId source, LaneId target, LanePortId input)
        {
            if (!contains(source) || !contains(target)) {
                return;
            }

            auto& connections = connections_for_domain(input.domain, _connections.compiled, _connections.realtime);
            if (auto it = connections.inputs_by_target.find(target); it != connections.inputs_by_target.end()) {
                auto& inputs = it->second;
                std::erase(inputs, LaneInputConnection { .source = source, .input = input });
                if (inputs.empty()) {
                    connections.inputs_by_target.erase(it);
                }
            }

            if (auto it = connections.outputs_by_source.find(source); it != connections.outputs_by_source.end()) {
                auto& outputs = it->second;
                std::erase(outputs, LaneOutputConnection { .target = target, .input = input });
                if (outputs.empty()) {
                    connections.outputs_by_source.erase(it);
                }
            }
        }

        void disconnect_input(LaneId target, LanePortId input)
        {
            if (!contains(target)) {
                return;
            }
            auto const inputs = inputs_for(target);
            for (auto const& connection : inputs) {
                if (connection.input == input) {
                    disconnect(connection.source, target, input);
                }
            }
        }

        void connect_exclusive(LaneId source, LaneId target, LanePortId input)
        {
            disconnect_input(target, input);
            connect(source, target, input);
        }

        void add_child(LaneId parent, LaneId child)
        {
            (void)location_for(parent);
            (void)location_for(child);

            auto& children = _hierarchy.children_by_parent[parent];
            if (std::ranges::find(children, child) == children.end()) {
                children.push_back(child);
            }

            auto& parents = _hierarchy.parents_by_child[child];
            if (std::ranges::find(parents, parent) == parents.end()) {
                parents.push_back(parent);
            }
        }

        void remove_child(LaneId parent, LaneId child)
        {
            if (auto it = _hierarchy.children_by_parent.find(parent); it != _hierarchy.children_by_parent.end()) {
                std::erase(it->second, child);
                if (it->second.empty()) {
                    _hierarchy.children_by_parent.erase(it);
                }
            }

            if (auto it = _hierarchy.parents_by_child.find(child); it != _hierarchy.parents_by_child.end()) {
                std::erase(it->second, parent);
                if (it->second.empty()) {
                    _hierarchy.parents_by_child.erase(it);
                }
            }
        }

        void remove_lane(LaneId removed)
        {
            if (!contains(removed)) {
                return;
            }

            auto remove_outputs = [&](LaneGraphDomainConnections& connections) {
                auto it = connections.outputs_by_source.find(removed);
                if (it == connections.outputs_by_source.end()) {
                    return;
                }
                auto outputs = std::move(it->second);
                connections.outputs_by_source.erase(it);
                for (auto const& output : outputs) {
                    if (auto target_it = connections.inputs_by_target.find(output.target);
                        target_it != connections.inputs_by_target.end()
                    ) {
                        auto& target_inputs = target_it->second;
                        std::erase(target_inputs, LaneInputConnection {
                            .source = removed,
                            .input = output.input,
                        });
                        if (target_inputs.empty()) {
                            connections.inputs_by_target.erase(target_it);
                        }
                    }
                }
            };
            remove_outputs(_connections.compiled);
            remove_outputs(_connections.realtime);

            auto remove_inputs = [&](LaneGraphDomainConnections& connections) {
                auto it = connections.inputs_by_target.find(removed);
                if (it == connections.inputs_by_target.end()) {
                    return;
                }
                auto inputs = std::move(it->second);
                connections.inputs_by_target.erase(it);
                for (auto const& input : inputs) {
                    if (auto source_it = connections.outputs_by_source.find(input.source);
                        source_it != connections.outputs_by_source.end()
                    ) {
                        auto& source_outputs = source_it->second;
                        std::erase(source_outputs, LaneOutputConnection {
                            .target = removed,
                            .input = input.input,
                        });
                        if (source_outputs.empty()) {
                            connections.outputs_by_source.erase(source_it);
                        }
                    }
                }
            };
            remove_inputs(_connections.compiled);
            remove_inputs(_connections.realtime);

            if (auto it = _hierarchy.children_by_parent.find(removed); it != _hierarchy.children_by_parent.end()) {
                auto children = std::move(it->second);
                _hierarchy.children_by_parent.erase(it);
                for (auto const& child : children) {
                    remove_child(removed, child);
                }
            }

            if (auto it = _hierarchy.parents_by_child.find(removed); it != _hierarchy.parents_by_child.end()) {
                auto parents = std::move(it->second);
                _hierarchy.parents_by_child.erase(it);
                for (auto const& parent : parents) {
                    remove_child(parent, removed);
                }
            }

            auto const location = location_for(removed);
            if (location.domain == LanePortDomain::compiled) {
                size_t const last_index = _lanes.compiled.size() - 1;
                if (location.index != last_index) {
                    std::swap(_lanes.compiled[location.index], _lanes.compiled[last_index]);
                    _lanes.indices[_lanes.compiled[location.index].lane.id] = LaneLocation {
                        .domain = LanePortDomain::compiled,
                        .index = location.index,
                    };
                }
                _lanes.compiled.pop_back();
            } else {
                size_t const last_index = _lanes.realtime.size() - 1;
                if (location.index != last_index) {
                    std::swap(_lanes.realtime[location.index], _lanes.realtime[last_index]);
                    _lanes.indices[_lanes.realtime[location.index].lane.id] = LaneLocation {
                        .domain = LanePortDomain::realtime,
                        .index = location.index,
                    };
                }
                _lanes.realtime.pop_back();
            }
            _lanes.indices.erase(removed);

        }

        std::vector<LaneInputConnection> const& inputs_for(LaneId target) const
        {
            if (auto const it = _connections.realtime.inputs_by_target.find(target);
                it != _connections.realtime.inputs_by_target.end()
            ) {
                return it->second;
            }
            auto const it = _connections.compiled.inputs_by_target.find(target);
            return it == _connections.compiled.inputs_by_target.end() ? empty_inputs() : it->second;
        }

        std::vector<LaneOutputConnection> const& outputs_for(LaneId source) const
        {
            if (!contains(source)) {
                return empty_outputs();
            }
            LanePortDomain const domain = lane_output_domain(lane(source).output);
            auto const& connections = connections_for_domain(
                domain,
                _connections.compiled,
                _connections.realtime
            );
            auto const it = connections.outputs_by_source.find(source);
            return it == connections.outputs_by_source.end() ? empty_outputs() : it->second;
        }

        std::vector<LaneInputConnection> const& compiled_inputs_for(LaneId target) const
        {
            auto const it = _connections.compiled.inputs_by_target.find(target);
            return it == _connections.compiled.inputs_by_target.end() ? empty_inputs() : it->second;
        }

        std::vector<LaneOutputConnection> const& compiled_outputs_for(LaneId source) const
        {
            auto const it = _connections.compiled.outputs_by_source.find(source);
            return it == _connections.compiled.outputs_by_source.end() ? empty_outputs() : it->second;
        }

        std::vector<LaneInputConnection> const& realtime_inputs_for(LaneId target) const
        {
            auto const it = _connections.realtime.inputs_by_target.find(target);
            return it == _connections.realtime.inputs_by_target.end() ? empty_inputs() : it->second;
        }

        std::vector<LaneOutputConnection> const& realtime_outputs_for(LaneId source) const
        {
            auto const it = _connections.realtime.outputs_by_source.find(source);
            return it == _connections.realtime.outputs_by_source.end() ? empty_outputs() : it->second;
        }

        std::vector<LaneId> const& children_for(LaneId parent) const
        {
            auto const it = _hierarchy.children_by_parent.find(parent);
            return it == _hierarchy.children_by_parent.end() ? empty_children() : it->second;
        }

        std::vector<LaneId> const& parents_for(LaneId child) const
        {
            auto const it = _hierarchy.parents_by_child.find(child);
            return it == _hierarchy.parents_by_child.end() ? empty_children() : it->second;
        }
    };
}
