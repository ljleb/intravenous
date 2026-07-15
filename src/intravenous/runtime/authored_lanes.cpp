#include <intravenous/runtime/authored_lanes.h>

#include <algorithm>
#include <stdexcept>
#include <tuple>

namespace iv {
TypeErasedLaneNode BeatTriggerLaneNode::from_lane_ui_state(
    std::string_view serialized_state,
    LaneCreationContext const& context)
{
    BeatTriggerLaneNode node(context.sample_rate);
    auto const result = node.apply_lane_ui_state(LaneUiStateWrite{
        .serialized_state = serialized_state,
    });
    if (!result.accepted) {
        throw std::runtime_error("invalid beat-trigger authored state: " + result.error_message);
    }
    return TypeErasedLaneNode(std::move(node));
}

TypeErasedLaneNode AudioFileCaptureLaneNode::from_lane_ui_state(
    std::string_view serialized_state,
    LaneCreationContext const& context)
{
    try {
        auto const json = nlohmann::json::parse(serialized_state);
        auto const path = json.at("path").get<std::string>();
        if (path.empty()) throw std::runtime_error("audio-file-capture path must not be empty");
        return TypeErasedLaneNode(AudioFileCaptureLaneNode(path, context.sample_rate));
    } catch (std::exception const& error) {
        throw std::runtime_error("invalid audio-file-capture authored state: " + std::string(error.what()));
    }
}

namespace {
template<CreatableLane T>
CreatableLaneDescriptor descriptor_for()
{
    return {
        .type_id = std::string(T::lane_model_type_id()),
        .category = std::string(T::lane_creation_category()),
        .label = std::string(T::lane_creation_label()),
        .description = std::string(T::lane_creation_description()),
    };
}
} // namespace

std::vector<CreatableLaneDescriptor> AuthoredLanes::creatable_lane_types()
{
    return {descriptor_for<BeatTriggerLaneNode>(), descriptor_for<AudioFileCaptureLaneNode>()};
}

TypeErasedLaneNode AuthoredLanes::make_node(
    std::string_view type_id,
    std::string_view serialized_state,
    LaneCreationContext const& context)
{
    if (type_id == BeatTriggerLaneNode::lane_model_type_id()) {
        return BeatTriggerLaneNode::from_lane_ui_state(serialized_state, context);
    }
    if (type_id == AudioFileCaptureLaneNode::lane_model_type_id()) {
        return AudioFileCaptureLaneNode::from_lane_ui_state(serialized_state, context);
    }
    throw std::runtime_error("unknown authored lane type: " + std::string(type_id));
}

TimelineLaneBatchUpdate AuthoredLanes::create(std::string_view type_id, InternedString public_id)
{
    std::string state;
    if (type_id == BeatTriggerLaneNode::lane_model_type_id()) {
        state = BeatTriggerLaneNode::default_lane_ui_state();
    } else if (type_id == AudioFileCaptureLaneNode::lane_model_type_id()) {
        state = AudioFileCaptureLaneNode::default_lane_ui_state();
    } else {
        throw std::runtime_error("unknown creatable lane type: " + std::string(type_id));
    }
    if (public_id.empty()) public_id = generate_uuid_v4();
    if (lanes_.contains(public_id)) throw std::runtime_error("duplicate authored lane id: " + public_id.str());

    auto const lane = LaneId{next_runtime_lane_id_++};
    auto record = AuthoredLaneRecord{public_id, std::string(type_id), std::move(state)};
    lanes_.emplace(public_id, StoredLane{lane, record});
    return TimelineLaneBatchUpdate{.upserts = {TimelineLaneUpsert{
        .lane = lane,
        .external_id = public_id,
        .make_node = [record, context = context_] {
            return make_node(record.type_id, record.serialized_state, context);
        },
    }}};
}

TimelineLaneBatchUpdate AuthoredLanes::reload(AuthoredLaneRecord record)
{
    if (record.lane_id.empty()) throw std::runtime_error("authored lane record is missing lane id");
    if (lanes_.contains(record.lane_id)) throw std::runtime_error("duplicate authored lane id: " + record.lane_id.str());
    // Validate now, so an unknown type is reported and never silently mapped.
    (void)make_node(record.type_id, record.serialized_state, context_);
    auto const lane = LaneId{next_runtime_lane_id_++};
    auto const id = record.lane_id;
    lanes_.emplace(id, StoredLane{lane, record});
    return TimelineLaneBatchUpdate{.upserts = {TimelineLaneUpsert{
        .lane = lane, .external_id = id,
        .make_node = [record = std::move(record), context = context_] {
            return make_node(record.type_id, record.serialized_state, context);
        },
    }}};
}

void AuthoredLanes::update_canonical_state(InternedString lane_id, std::string serialized_state)
{
    auto const it = lanes_.find(lane_id);
    if (it == lanes_.end()) throw std::runtime_error("authored timeline lane not found");
    it->second.record.serialized_state = std::move(serialized_state);
}

std::vector<AuthoredLaneRecord> AuthoredLanes::records() const
{
    std::vector<AuthoredLaneRecord> result;
    result.reserve(lanes_.size());
    for (auto const& [_, lane] : lanes_) result.push_back(lane.record);
    return result;
}

bool AuthoredLanes::contains(InternedString lane_id) const { return lanes_.contains(lane_id); }

void AuthoredLanes::record_connection(AuthoredLaneConnection connection)
{
    if (!contains_connection(connection)) connections_.push_back(std::move(connection));
}

void AuthoredLanes::remove_connection(AuthoredLaneConnection const& connection)
{
    std::erase_if(connections_, [&](AuthoredLaneConnection const& existing) {
        return existing.source_lane_id == connection.source_lane_id
            && existing.target_lane_id == connection.target_lane_id
            && existing.input == connection.input;
    });
}

bool AuthoredLanes::contains_connection(AuthoredLaneConnection const& connection) const
{
    return std::ranges::any_of(connections_, [&](AuthoredLaneConnection const& existing) {
        return existing.source_lane_id == connection.source_lane_id
            && existing.target_lane_id == connection.target_lane_id
            && existing.input == connection.input;
    });
}

std::vector<AuthoredLaneConnection> AuthoredLanes::connections() const { return connections_; }
} // namespace iv
