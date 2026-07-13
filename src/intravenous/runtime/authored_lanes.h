#pragma once

#include <intravenous/runtime/authored_lane_api.h>
#include <intravenous/basic_lane_nodes/beat_trigger.h>
#include <intravenous/basic_lane_nodes/type_erased.h>
#include <intravenous/runtime/lane_graph.h>
#include <intravenous/runtime/timeline_events.h>
#include <intravenous/runtime/uuid.h>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>
#include <unordered_map>

namespace iv {

template<typename T>
concept CreatableLane = requires(std::string_view state, LaneCreationContext const& context) {
    { T::lane_model_type_id() } -> std::convertible_to<std::string_view>;
    { T::lane_creation_category() } -> std::convertible_to<std::string_view>;
    { T::lane_creation_label() } -> std::convertible_to<std::string_view>;
    { T::lane_creation_description() } -> std::convertible_to<std::string_view>;
    { T::default_lane_ui_state() } -> std::convertible_to<std::string>;
    { T::from_lane_ui_state(state, context) } -> std::same_as<TypeErasedLaneNode>;
};

// The list contains types only; descriptors are derived from those types.
using AuthoredCreatableLaneTypes = std::tuple<BeatTriggerLaneNode>;
static_assert(CreatableLane<BeatTriggerLaneNode>);

class AuthoredLanes {
    struct StoredLane {
        LaneId runtime_lane;
        AuthoredLaneRecord record;
    };

    // Timeline receives explicit ids from independent graph and visualization
    // producers.  Authored model lanes use a separate high range so their
    // persistent records cannot collide with transient low-id lanes.
    std::uint64_t next_runtime_lane_id_ = std::uint64_t{1} << 62;
    LaneCreationContext context_;
    std::unordered_map<InternedString, StoredLane> lanes_;
    std::vector<AuthoredLaneConnection> connections_;

    static TypeErasedLaneNode make_node(
        std::string_view type_id,
        std::string_view serialized_state,
        LaneCreationContext const& context);

public:
    explicit AuthoredLanes(LaneCreationContext context) : context_(context) {}

    [[nodiscard]] static std::vector<CreatableLaneDescriptor> creatable_lane_types();
    [[nodiscard]] TimelineLaneBatchUpdate create(std::string_view type_id, InternedString public_id = {});
    [[nodiscard]] TimelineLaneBatchUpdate reload(AuthoredLaneRecord record);
    void update_canonical_state(InternedString lane_id, std::string serialized_state);
    [[nodiscard]] std::vector<AuthoredLaneRecord> records() const;
    [[nodiscard]] bool contains(InternedString lane_id) const;
    void record_connection(AuthoredLaneConnection connection);
    [[nodiscard]] std::vector<AuthoredLaneConnection> connections() const;
};
} // namespace iv
