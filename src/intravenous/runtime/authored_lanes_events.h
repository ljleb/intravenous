#pragma once

#include <intravenous/linker_event.h>
#include <intravenous/runtime/authored_lane_api.h>
#include <intravenous/runtime/timeline_events.h>

#include <optional>
#include <string>
#include <vector>

namespace iv {
class TimelineAuthoredLaneConnectionsBuilder {
    std::optional<std::vector<AuthoredLaneConnection>> result_ {};

public:
    void succeed(std::vector<AuthoredLaneConnection> connections)
    {
        result_ = std::move(connections);
    }
    [[nodiscard]] bool has_response() const noexcept { return result_.has_value(); }
    [[nodiscard]] std::vector<AuthoredLaneConnection> build() const { return *result_; }
};

using AuthoredLanesTimelineBatchRequestedEvent =
    void (*)(TimelineLaneBatchUpdate const &);
using TimelineAuthoredLaneCanonicalStateUpdatedEvent =
    void (*)(InternedString, std::string const &);
using TimelineAuthoredLaneConnectionRecordedEvent =
    void (*)(AuthoredLaneConnection const &);
using TimelineAuthoredLaneConnectionRemovedEvent =
    void (*)(AuthoredLaneConnection const &);
using TimelineAuthoredLaneConnectionsRequestedEvent =
    void (*)(TimelineAuthoredLaneConnectionsBuilder &);

IV_DECLARE_LINKER_EVENT(
    AuthoredLanesTimelineBatchRequestedEvent,
    iv_runtime_authored_lanes_timeline_batch_requested_event);
IV_DECLARE_LINKER_EVENT(
    TimelineAuthoredLaneCanonicalStateUpdatedEvent,
    iv_runtime_timeline_authored_lane_canonical_state_updated_event);
IV_DECLARE_LINKER_EVENT(
    TimelineAuthoredLaneConnectionRecordedEvent,
    iv_runtime_timeline_authored_lane_connection_recorded_event);
IV_DECLARE_LINKER_EVENT(
    TimelineAuthoredLaneConnectionRemovedEvent,
    iv_runtime_timeline_authored_lane_connection_removed_event);
IV_DECLARE_LINKER_EVENT(
    TimelineAuthoredLaneConnectionsRequestedEvent,
    iv_runtime_timeline_authored_lane_connections_requested_event);
} // namespace iv
