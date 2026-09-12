#pragma once

#include <intravenous/linker_event.h>
#include <intravenous/runtime/configured_lane_api.h>
#include <intravenous/runtime/timeline_events.h>

#include <optional>
#include <string>
#include <vector>

namespace iv {
class TimelineConfiguredLaneConnectionsBuilder {
    std::optional<std::vector<ConfiguredLaneConnection>> result_ {};

public:
    void succeed(std::vector<ConfiguredLaneConnection> connections)
    {
        result_ = std::move(connections);
    }
    [[nodiscard]] bool has_response() const noexcept { return result_.has_value(); }
    [[nodiscard]] std::vector<ConfiguredLaneConnection> build() const { return *result_; }
};

using ConfiguredLanesTimelineBatchRequestedEvent =
    void (*)(TimelineLaneBatchUpdate const &);
using TimelineConfiguredLaneCanonicalStateUpdatedEvent =
    void (*)(InternedString, std::string const &);
using TimelineConfiguredLaneConnectionRecordedEvent =
    void (*)(ConfiguredLaneConnection const &);
using TimelineConfiguredLaneConnectionRemovedEvent =
    void (*)(ConfiguredLaneConnection const &);
using TimelineConfiguredLaneConnectionsRequestedEvent =
    void (*)(TimelineConfiguredLaneConnectionsBuilder &);

IV_DECLARE_LINKER_EVENT(
    ConfiguredLanesTimelineBatchRequestedEvent,
    iv_runtime_configured_lanes_timeline_batch_requested_event);
IV_DECLARE_LINKER_EVENT(
    TimelineConfiguredLaneCanonicalStateUpdatedEvent,
    iv_runtime_timeline_configured_lane_canonical_state_updated_event);
IV_DECLARE_LINKER_EVENT(
    TimelineConfiguredLaneConnectionRecordedEvent,
    iv_runtime_timeline_configured_lane_connection_recorded_event);
IV_DECLARE_LINKER_EVENT(
    TimelineConfiguredLaneConnectionRemovedEvent,
    iv_runtime_timeline_configured_lane_connection_removed_event);
IV_DECLARE_LINKER_EVENT(
    TimelineConfiguredLaneConnectionsRequestedEvent,
    iv_runtime_timeline_configured_lane_connections_requested_event);
} // namespace iv
