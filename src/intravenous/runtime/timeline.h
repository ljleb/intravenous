#pragma once

#include <intravenous/runtime/timeline_events.h>
#include <intravenous/runtime/lane_graph.h>
#include <intravenous/runtime/timeline_fwd.h>
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
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace iv {
    class Timeline {
        std::mutex _graph_mutex;
        LaneGraph _graph;

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

        LaneMetadata lane_metadata(LaneId lane)
        {
            return with_graph([&](LaneGraph& graph) {
                return graph.lane(lane).metadata;
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
            with_graph([&](LaneGraph& graph) {
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
                        upsert.external_task_dependencies);
                }
                for (auto const &child : batch.hierarchy_additions) {
                    graph.add_child(child.parent, child.child);
                }
                for (auto const &connection : batch.connections_to_add) {
                    graph.connect(connection.source, connection.target, connection.input);
                }
            });
        }

        void remove_lane(LaneId lane)
        {
            with_graph([&](LaneGraph& graph) {
                graph.remove_lane(lane);
            });
        }
    };
}
