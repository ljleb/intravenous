#pragma once

#include "runtime/lane_graph.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace iv {
    struct LaneInfo {
        uint64_t lane_id = 0;
        LaneDomain domain = LaneDomain::realtime;
        GraphInputPortDescriptor graph_input_port {};
    };

    struct LaneConnectionInfo {
        uint64_t source_lane_id = 0;
        uint64_t target_lane_id = 0;
        PortKind port_kind = PortKind::sample;
        size_t port_ordinal = 0;
    };

    struct LaneQueryFilter {
        std::string kind {};
    };

    struct LaneQuery {
        LaneQueryFilter filter {};
    };

    struct LaneQueryResult {
        size_t start_index = 0;
        size_t visible_lane_count = 0;
        size_t total_lane_count = 0;
        std::vector<LaneInfo> lanes {};
        std::vector<LaneConnectionInfo> connections {};
    };

    struct LaneViewRequest {
        std::string view_id {};
        LaneQuery query {};
        size_t start_index = 0;
        size_t visible_lane_count = 0;
    };

    struct LaneViewResult {
        std::string view_id {};
        LaneQueryResult lanes {};
    };

    using LaneViewQueryProvider = std::function<LaneQueryResult(
        LaneQueryFilter const&,
        std::optional<size_t>,
        std::optional<size_t>
    )>;
    using LaneViewUpdateSink = std::function<void(LaneViewResult const&)>;

    class LaneViewService {
        struct ActiveView {
            LaneQueryFilter filter {};
            size_t start_index = 0;
            size_t visible_lane_count = 0;
            std::unordered_set<uint64_t> visible_lane_ids {};
        };

        LaneViewQueryProvider _query_provider;
        LaneViewUpdateSink _update_sink;
        std::unordered_map<std::string, ActiveView> _active_views;

        LaneQueryResult query_lanes(
            LaneQueryFilter const& filter,
            std::optional<size_t> start_index,
            std::optional<size_t> visible_lane_count
        ) const;
        static std::unordered_set<uint64_t> visible_lane_ids(LaneQueryResult const& result);
        void notify_view_changed(std::string const& view_id);

    public:
        LaneViewService() = default;
        explicit LaneViewService(LaneViewQueryProvider query_provider);

        void set_query_provider(LaneViewQueryProvider query_provider);
        void set_update_sink(LaneViewUpdateSink update_sink);

        LaneViewResult open_view(LaneViewRequest request);
        LaneViewResult update_view(LaneViewRequest request);
        void close_view(std::string const& view_id);

        void mark_lane_set_changed();
        void mark_lane_changed(LaneId lane_id);
    };
}
