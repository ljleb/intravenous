#pragma once

#include <intravenous/runtime/timeline_events.h>
#include <intravenous/runtime/lane_graph.h>
#include <intravenous/runtime/timeline_fwd.h>
#include <intravenous/runtime/uuid.h>
#include <intravenous/sample.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace iv {
    class Timeline {
        struct PendingPublicConnection {
            InternedString source_id {};
            InternedString target_id {};
            LanePortId input {};
        };

        std::mutex _graph_mutex;
        LaneGraph _graph;
        std::unordered_map<LaneId, InternedString, LaneIdHash> _external_ids_by_lane;
        std::unordered_map<InternedString, LaneId> _lanes_by_external_id;
        std::unordered_map<LaneId, TimelineLaneLifetime, LaneIdHash> _lane_lifetimes;
        std::vector<PendingPublicConnection> _pending_public_connections;

        InternedString ensure_external_id_locked(LaneId lane)
        {
            if (auto const it = _external_ids_by_lane.find(lane);
                it != _external_ids_by_lane.end()) {
                return it->second;
            }
            auto external_id = generate_uuid_v4();
            _external_ids_by_lane.emplace(lane, external_id);
            _lanes_by_external_id.emplace(external_id, lane);
            return external_id;
        }

        bool try_connect_public_lanes_locked(PendingPublicConnection const &connection)
        {
            auto const source = _lanes_by_external_id.find(connection.source_id);
            auto const target = _lanes_by_external_id.find(connection.target_id);
            if (source == _lanes_by_external_id.end() || target == _lanes_by_external_id.end()) {
                return false;
            }
            if (!_graph.contains(source->second) || !_graph.contains(target->second)) {
                return false;
            }
            _graph.connect(source->second, target->second, connection.input);
            return true;
        }

        void resolve_pending_public_connections_locked()
        {
            if (_pending_public_connections.empty()) {
                return;
            }

            auto pending = std::move(_pending_public_connections);
            _pending_public_connections.clear();
            for (auto const &connection : pending) {
                if (!try_connect_public_lanes_locked(connection)) {
                    _pending_public_connections.push_back(connection);
                }
            }
        }

    public:
        template<typename Fn>
        decltype(auto) with_graph(Fn&& fn)
        {
            std::scoped_lock lock(_graph_mutex);
            return std::forward<Fn>(fn)(_graph);
        }

        bool contains_lane(LaneId lane)
        {
            return with_graph([&](LaneGraph& graph) {
                return graph.contains(lane);
            });
        }

        std::vector<LaneOutputConnection> lane_outputs_for(LaneId lane)
        {
            return with_graph([&](LaneGraph& graph) {
                auto const& outputs = graph.outputs_for(lane);
                return std::vector<LaneOutputConnection>{ outputs.begin(), outputs.end() };
            });
        }

        std::vector<LaneId> lane_ids()
        {
            return with_graph([&](LaneGraph& graph) {
                std::vector<LaneId> ids;
                graph.for_each_lane([&](LaneRecord const& lane) {
                    ids.push_back(lane.id);
                });
                return ids;
            });
        }

        std::vector<LaneGraphConnection> lane_connections()
        {
            return with_graph([&](LaneGraph& graph) {
                std::vector<LaneGraphConnection> connections;
                graph.for_each_lane([&](LaneRecord const& lane) {
                    for (auto const &output : graph.outputs_for(lane.id)) {
                        connections.push_back(LaneGraphConnection{
                            .source = lane.id,
                            .target = output.target,
                            .input = output.input,
                        });
                    }
                });
                return connections;
            });
        }

        LaneMetadata lane_metadata(LaneId lane)
        {
            return with_graph([&](LaneGraph& graph) {
                return graph.lane(lane).metadata;
            });
        }

        std::optional<ChannelTypeId> lane_sample_channel_type(LaneId lane)
        {
            return with_graph([&](LaneGraph& graph) {
                return graph.lane(lane).sample_channel_type;
            });
        }

        std::optional<std::string> lane_model_type_id(LaneId lane)
        {
            return with_graph([&](LaneGraph& graph) {
                if (!graph.contains(lane)) {
                    return std::optional<std::string> {};
                }
                return graph.lane(lane).node.lane_model_type_id();
            });
        }

        std::optional<LaneUiStateSnapshot> take_lane_ui_state_update(LaneId lane)
        {
            return with_graph([&](LaneGraph& graph) {
                if (!graph.contains(lane)) {
                    return std::optional<LaneUiStateSnapshot> {};
                }
                return graph.lane(lane).node.take_lane_ui_state_update();
            });
        }

        std::optional<LaneUiStateSnapshot> lane_ui_state_snapshot(LaneId lane)
        {
            return with_graph([&](LaneGraph& graph) {
                if (!graph.contains(lane)) {
                    return std::optional<LaneUiStateSnapshot> {};
                }
                return graph.lane(lane).node.lane_ui_state_snapshot();
            });
        }

        LaneUiStateApplyResult apply_lane_ui_state(
            LaneId lane,
            LaneUiStateWrite const& write)
        {
            return with_graph([&](LaneGraph& graph) {
                if (!graph.contains(lane)) {
                    return LaneUiStateApplyResult{
                        .error_message = "timeline lane not found",
                    };
                }
                return graph.lane(lane).node.apply_lane_ui_state(write);
            });
        }

        query::LaneQuerySchema lane_query_schema(std::uint64_t revision = 0)
        {
            return with_graph([&](LaneGraph& graph) {
                std::vector<std::pair<std::string, query::LaneQueryValueType>> entries;
                std::unordered_map<std::string, query::LaneQueryValueType> type_by_key;
                graph.for_each_lane([&](LaneRecord const& lane) {
                    for (auto const &[key, type] : lane.metadata.schema_entries()) {
                        auto const [it, inserted] = type_by_key.emplace(key, type);
                        if (!inserted && it->second != type) {
                            throw std::runtime_error("lane metadata schema type conflict for key: " + key);
                        }
                    }
                });
                entries.reserve(type_by_key.size());
                for (auto const &[key, type] : type_by_key) {
                    entries.emplace_back(key, type);
                }
                return query::LaneQuerySchema::from_entries(std::move(entries), revision);
            });
        }

        bool lane_has_unit_metadata(LaneId lane, std::string_view key)
        {
            return with_graph([&](LaneGraph& graph) {
                return graph.lane(lane).metadata.has_unit(key);
            });
        }

        std::optional<int> lane_int_metadata(LaneId lane, std::string_view key)
        {
            return with_graph([&](LaneGraph& graph) {
                return graph.lane(lane).metadata.int_value(key);
            });
        }

        std::optional<float> lane_float_metadata(LaneId lane, std::string_view key)
        {
            return with_graph([&](LaneGraph& graph) {
                return graph.lane(lane).metadata.float_value(key);
            });
        }

        void apply_lane_batch(TimelineLaneBatchUpdate const &batch)
        {
            std::scoped_lock lock(_graph_mutex);
            for (auto const lane : batch.removals) {
                if (auto const it = _external_ids_by_lane.find(lane);
                    it != _external_ids_by_lane.end()) {
                    _lanes_by_external_id.erase(it->second);
                    _external_ids_by_lane.erase(it);
                }
                _lane_lifetimes.erase(lane);
            }
            for (auto const &upsert : batch.upserts) {
                if (upsert.lifetime == TimelineLaneLifetime::persistent
                    && upsert.external_id.empty()) {
                    throw std::runtime_error(
                        "persistent timeline lane upsert requires a stable external id");
                }
                if (!upsert.external_id.empty()) {
                    if (auto const existing = _lanes_by_external_id.find(upsert.external_id);
                        existing != _lanes_by_external_id.end() && existing->second != upsert.lane) {
                        throw std::runtime_error(
                            "duplicate timeline external lane id: " + upsert.external_id.str());
                    }
                    if (auto const it = _external_ids_by_lane.find(upsert.lane);
                        it != _external_ids_by_lane.end() && it->second != upsert.external_id) {
                        _lanes_by_external_id.erase(it->second);
                    }
                    _external_ids_by_lane[upsert.lane] = upsert.external_id;
                    _lanes_by_external_id[upsert.external_id] = upsert.lane;
                } else {
                    (void)ensure_external_id_locked(upsert.lane);
                }
                _lane_lifetimes[upsert.lane] = upsert.lifetime;
            }
            auto &graph = _graph;
            [&] {
                for (auto const &child : batch.hierarchy_removals) {
                    graph.remove_child(child.parent, child.child);
                }
                for (auto const &connection : batch.connections_to_remove) {
                    graph.disconnect(connection.source, connection.target, connection.input);
                }
                for (auto const lane : batch.removals) {
                    graph.remove_lane(lane);
                }
                for (auto const &upsert : batch.upserts) {
                    if (!upsert.make_node) {
                        throw std::runtime_error("timeline lane upsert is missing node factory");
                    }
                    graph.upsert_lane(
                        upsert.lane,
                        upsert.make_node(),
                        upsert.metadata,
                        upsert.external_task_dependencies,
                        upsert.sample_channel_type);
                }
                for (auto const &child : batch.hierarchy_additions) {
                    graph.add_child(child.parent, child.child);
                }
                for (auto const &connection : batch.connections_to_add) {
                    graph.connect(connection.source, connection.target, connection.input);
                }
                resolve_pending_public_connections_locked();
            }();
        }

        void remove_lane(LaneId lane)
        {
            std::scoped_lock lock(_graph_mutex);
            if (auto const it = _external_ids_by_lane.find(lane);
                it != _external_ids_by_lane.end()) {
                _lanes_by_external_id.erase(it->second);
                _external_ids_by_lane.erase(it);
            }
            _lane_lifetimes.erase(lane);
            _graph.remove_lane(lane);
        }

        InternedString lane_public_id(LaneId lane)
        {
            std::scoped_lock lock(_graph_mutex);
            return ensure_external_id_locked(lane);
        }

        bool lane_is_persistent(LaneId lane)
        {
            std::scoped_lock lock(_graph_mutex);
            if (auto const it = _lane_lifetimes.find(lane); it != _lane_lifetimes.end()) {
                return it->second == TimelineLaneLifetime::persistent;
            }
            return false;
        }

        std::optional<LaneId> resolve_public_lane_id(InternedString external_id)
        {
            std::scoped_lock lock(_graph_mutex);
            if (auto const it = _lanes_by_external_id.find(external_id);
                it != _lanes_by_external_id.end()) {
                return it->second;
            }
            return std::nullopt;
        }

        void connect_public_lanes_or_defer(
            InternedString source_id,
            InternedString target_id,
            LanePortId input)
        {
            std::scoped_lock lock(_graph_mutex);
            PendingPublicConnection connection{
                .source_id = source_id,
                .target_id = target_id,
                .input = input,
            };
            if (!try_connect_public_lanes_locked(connection)) {
                _pending_public_connections.push_back(std::move(connection));
            }
        }

        std::vector<std::tuple<InternedString, InternedString, LanePortId>>
        pending_public_connections()
        {
            std::scoped_lock lock(_graph_mutex);
            std::vector<std::tuple<InternedString, InternedString, LanePortId>> pending;
            pending.reserve(_pending_public_connections.size());
            for (auto const &connection : _pending_public_connections) {
                pending.emplace_back(
                    connection.source_id,
                    connection.target_id,
                    connection.input);
            }
            return pending;
        }
    };
}
