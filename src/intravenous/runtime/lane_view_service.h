#pragma once

#include <intravenous/lane_node/graph.h>
#include <intravenous/runtime/uuid.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace iv {
    struct LaneInputInfo {
        LanePortDomain domain = LanePortDomain::realtime;
        PortKind kind = PortKind::sample;
        size_t ordinal = 0;
        std::string name {};
    };

    struct LaneInfo {
        InternedString lane_id {};
        LaneId runtime_lane {};
        LaneDomain domain = LaneDomain::realtime;
        std::optional<ChannelTypeId> sample_channel_type {};
        LaneMetadata metadata {};
        std::optional<std::string> model_type_id {};
        PortKind output_kind = PortKind::sample;
        std::vector<LaneInputInfo> inputs {};
    };

    struct LaneConnectionInfo {
        InternedString source_lane_id {};
        InternedString target_lane_id {};
        LanePortDomain port_domain = LanePortDomain::realtime;
        PortKind port_kind = PortKind::sample;
        size_t port_ordinal = 0;
    };

    struct LaneQueryFilter {
        std::string source {};
    };

    struct LaneQuery {
        LaneQueryFilter filter {};
    };

    struct LaneQueryResult {
        size_t start_index = 0;
        size_t visible_lane_count = 0;
        size_t total_lane_count = 0;
        std::optional<std::string> error_message {};
        std::vector<LaneInfo> lanes {};
        std::vector<LaneConnectionInfo> connections {};
    };

    struct LaneViewRequest {
        InternedString view_id {};
        LaneQuery query {};
        size_t start_index = 0;
        size_t visible_lane_count = 0;
        size_t first_sample_index = 0;
        size_t last_sample_index = 0;
        size_t display_sample_count = 0;
    };

    struct LaneViewResult {
        InternedString view_id {};
        LaneQueryResult lanes {};
        size_t first_sample_index = 0;
        size_t last_sample_index = 0;
        size_t display_sample_count = 0;
    };

    using LaneViewQueryProvider = std::function<LaneQueryResult(
        LaneQueryFilter const&,
        std::optional<size_t>,
        std::optional<size_t>
    )>;
    using LaneViewUpdateSink = std::function<void(LaneViewResult const&)>;

    class LaneViewService {
    public:
        struct ActiveViewSnapshot {
            std::string view_id {};
            
            LaneQueryFilter filter {};
            size_t start_index = 0;
            size_t visible_lane_count = 0;
            size_t first_sample_index = 0;
            size_t last_sample_index = 0;
            size_t display_sample_count = 0;
        };

    private:
        struct ActiveView {
            LaneQueryFilter filter {};
            size_t start_index = 0;
            size_t visible_lane_count = 0;
            size_t first_sample_index = 0;
            size_t last_sample_index = 0;
            size_t display_sample_count = 0;
            std::unordered_set<uint64_t> visible_lane_ids {};
        };

        LaneViewQueryProvider _query_provider;
        LaneViewUpdateSink _update_sink;
        std::unordered_map<InternedString, ActiveView> _active_views;

        LaneQueryResult query_lanes(
            LaneQueryFilter const& filter,
            std::optional<size_t> start_index,
            std::optional<size_t> visible_lane_count
        ) const;
        static std::unordered_set<uint64_t> visible_lane_ids(LaneQueryResult const& result);
        void notify_view_changed(InternedString view_id);

    public:
        LaneViewService() = default;
        explicit LaneViewService(LaneViewQueryProvider query_provider);

        void set_query_provider(LaneViewQueryProvider query_provider);
        void set_update_sink(LaneViewUpdateSink update_sink);

        LaneViewResult open_view(LaneViewRequest request);
        LaneViewResult update_view(LaneViewRequest request);
        void close_view(InternedString view_id);
        std::vector<ActiveViewSnapshot> active_views() const;

        void mark_lane_set_changed();
        void mark_lane_changed(LaneId lane_id);
    };
}
