#pragma once

#include "runtime/timeline_events.h"
#include "runtime/lane_graph.h"
#include "runtime/timeline_fwd.h"
#include "sample.h"

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
    struct TimelineGraphRuntime {
        LaneGraph lane_graph;
    };

    class Timeline {
        std::mutex _graphs_mutex;
        std::unordered_map<std::string, TimelineGraphRuntime> _graphs;
        std::string _default_graph_id = "module";

        TimelineGraphRuntime& default_graph_locked()
        {
            return _graphs[_default_graph_id];
        }

    public:
        template<typename Fn>
        decltype(auto) with_default_graph(Fn&& fn)
        {
            std::scoped_lock lock(_graphs_mutex);
            return std::forward<Fn>(fn)(default_graph_locked().lane_graph);
        }

        RealtimeSampleBlockLease pull_realtime_lane_sample_block(
            LaneId lane,
            size_t start_index,
            size_t count
        )
        {
            return with_default_graph([&](LaneGraph& graph) {
                return graph.pull_realtime_sample_block(lane, start_index, count);
            });
        }

        bool contains_lane(LaneId lane)
        {
            return with_default_graph([&](LaneGraph& graph) {
                return graph.contains(lane);
            });
        }

        std::vector<LaneOutputConnection> lane_outputs_for(LaneId lane)
        {
            return with_default_graph([&](LaneGraph& graph) {
                auto const& outputs = graph.outputs_for(lane);
                return std::vector<LaneOutputConnection>{ outputs.begin(), outputs.end() };
            });
        }

        std::vector<LaneId> lane_ids()
        {
            return with_default_graph([&](LaneGraph& graph) {
                std::vector<LaneId> ids;
                graph.for_each_lane([&](LaneRecord const& lane) {
                    ids.push_back(lane.id);
                });
                return ids;
            });
        }

        LaneMetadata lane_metadata(LaneId lane)
        {
            return with_default_graph([&](LaneGraph& graph) {
                return graph.lane(lane).metadata;
            });
        }

        query::LaneQuerySchema lane_query_schema(std::uint64_t revision = 0)
        {
            return with_default_graph([&](LaneGraph& graph) {
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
            return with_default_graph([&](LaneGraph& graph) {
                return graph.lane(lane).metadata.has_unit(key);
            });
        }

        std::optional<int> lane_int_metadata(LaneId lane, std::string_view key)
        {
            return with_default_graph([&](LaneGraph& graph) {
                return graph.lane(lane).metadata.int_value(key);
            });
        }

        std::optional<float> lane_float_metadata(LaneId lane, std::string_view key)
        {
            return with_default_graph([&](LaneGraph& graph) {
                return graph.lane(lane).metadata.float_value(key);
            });
        }

        void apply_lane_batch(TimelineLaneBatchUpdate const &batch)
        {
            with_default_graph([&](LaneGraph& graph) {
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
                    graph.upsert_lane(upsert.lane, upsert.make_node(), upsert.metadata);
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
            with_default_graph([&](LaneGraph& graph) {
                graph.remove_lane(lane);
            });
        }
    };
}
