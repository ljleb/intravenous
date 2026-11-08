#include <intravenous/graph_jit/realtime_port_state_transition.h>

#include <intravenous/compat.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <exception>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace iv::graph_jit {
namespace {

void hash_combine(std::size_t& seed, std::size_t value) noexcept
{
    seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
}

struct StableRealtimePortStateIdHash {
    std::size_t operator()(StableRealtimePortStateId const& id) const noexcept
    {
        std::size_t result = 0;
        auto const hash_string = std::hash<std::string>{};
        hash_combine(result, hash_string(id.node.graph));
        hash_combine(result, hash_string(id.node.virtual_node));
        hash_combine(result, std::hash<std::size_t>{}(id.node.direct_member));
        hash_combine(
            result,
            std::hash<unsigned>{}(static_cast<unsigned>(id.direction)));
        hash_combine(
            result,
            std::hash<unsigned>{}(static_cast<unsigned>(id.kind)));
        hash_combine(result, hash_string(id.port_name));
        hash_combine(result, std::hash<std::size_t>{}(id.port_index));
        hash_combine(
            result,
            id.channel
                ? std::hash<std::size_t>{}(*id.channel) + 1
                : std::size_t{0});
        hash_combine(
            result,
            std::hash<unsigned>{}(static_cast<unsigned>(id.role)));
        return result;
    }
};

struct LocatedState {
    std::size_t requirement = 0;
    std::size_t realization = 0;
    PortKind kind = PortKind::sample;
};

struct SampleTargetKey {
    std::size_t storage_offset = 0;
    std::size_t channel = 0;

    bool operator==(SampleTargetKey const&) const = default;
};

struct SampleTargetKeyHash {
    std::size_t operator()(SampleTargetKey const& key) const noexcept
    {
        std::size_t result = std::hash<std::size_t>{}(key.storage_offset);
        hash_combine(result, std::hash<std::size_t>{}(key.channel));
        return result;
    }
};

bool sample_storage_source_is_recoverable(
    SampleRealtimePortStateStorageView const& source)
{
    if (source.storage
        == SampleRealtimePortStateStorage::immutable_constant) {
        return source.constant_value.has_value()
            && !source.node_storage_offset
            && !source.callback_arena_offset;
    }
    return source.node_storage_offset.has_value()
        && !source.callback_arena_offset;
}

bool event_storage_is_persistent_sequence(
    EventRealtimePortStateStorageView const& storage) noexcept
{
    return (storage.storage == EventRealtimePortStateStorage::compact_carry
            || storage.storage == EventRealtimePortStateStorage::ring)
        && storage.node_storage_offset.has_value()
        && !storage.callback_arena_offset;
}

std::expected<EventConversionPlan, std::string> append_event_conversion(
    EventConversionPlan prefix,
    EventConversionPlan const& suffix)
{
    if (prefix.target_type != suffix.source_type
        || prefix.step_count > EventConversionPlan::max_steps
        || suffix.step_count > EventConversionPlan::max_steps
        || suffix.step_count
            > EventConversionPlan::max_steps - prefix.step_count) {
        return std::unexpected(
            "event state transition has an invalid conversion chain");
    }
    for (std::size_t step = 0; step < suffix.step_count; ++step) {
        prefix.steps[prefix.step_count++] = suffix.steps[step];
    }
    prefix.target_type = suffix.target_type;
    return prefix;
}

bool supports_linear_audio_channel_conversion(
    ContinuousValueStorageGeometry const& geometry) noexcept
{
    return geometry.channel_capability
            == ContinuousValueChannelCapability::audio
        && geometry.element_alignment >= alignof(float)
        && geometry.channel_scalar_lane_count != 0
        && geometry.value_size_bytes
            == geometry.channel_scalar_lane_count * sizeof(float);
}

// A converted or composed input keeps its values in its sources' storage. A
// source view pairs with a successor view when the successor view is persistent
// storage that can receive its values, and either
// - both hold the same producer's channel at the same read delay, or
// - both are feedback timelines, which only this input reads, so its values
//   move to the successor's timeline at the successor's read delay.
bool sample_source_views_pair(
    SampleRealtimePortStateStorageView const& before,
    SampleRealtimePortStateStorageView const& after)
{
    auto const same_producer = before.producer
        && before.producer == after.producer
        && before.producer_channel == after.producer_channel
        && before.timeline_offset_frames == after.timeline_offset_frames;
    auto const feedback_timelines =
        before.feedback_timeline && after.feedback_timeline;
    return (same_producer || feedback_timelines)
        && before.geometry == after.geometry
        && sample_storage_source_is_recoverable(before)
        && after.node_storage_offset && !after.callback_arena_offset
        && (after.storage == SampleRealtimePortStateStorage::ring
            || after.storage == SampleRealtimePortStateStorage::compact_carry);
}

// The predecessor and successor views of a converted or composed input's
// sources, in derivation order.
std::vector<SampleRealtimePortStateStorageView const*> derivation_sources(
    SampleRealtimePortStateRealization const& realization)
{
    std::vector<SampleRealtimePortStateStorageView const*> views;
    if (realization.retained_access
        == SampleRealtimePortStateAccess::composed) {
        for (auto const& source : realization.composition_sources) {
            views.push_back(&source.storage);
        }
    } else {
        for (auto const& source : realization.retained_sources) {
            views.push_back(&source);
        }
    }
    return views;
}

// True when every source of a converted or composed input pairs with the
// predecessor's source in the same position through the same derivation. Each
// source's values then transfer to that source's successor storage.
bool sample_sources_pair(
    SampleRealtimePortStateRealization const& previous,
    SampleRealtimePortStateRealization const& current)
{
    if (previous.retained_access != current.retained_access) return false;
    if (current.retained_access == SampleRealtimePortStateAccess::materialized) {
        if (!previous.materialization || !current.materialization
            || previous.materialization->source_layout
                != current.materialization->source_layout
            || previous.materialization->target_layout
                != current.materialization->target_layout
            || previous.authored_storage.representation_channel
                != current.authored_storage.representation_channel) {
            return false;
        }
    } else if (current.retained_access
               == SampleRealtimePortStateAccess::composed) {
        if (previous.composition_sources.size()
            != current.composition_sources.size()) {
            return false;
        }
        for (std::size_t index = 0;
             index < current.composition_sources.size(); ++index) {
            if (previous.composition_sources[index].coefficient
                != current.composition_sources[index].coefficient) {
                return false;
            }
        }
    } else {
        return false;
    }
    auto const before = derivation_sources(previous);
    auto const after = derivation_sources(current);
    if (after.empty() || before.size() != after.size()) return false;
    for (std::size_t index = 0; index < after.size(); ++index) {
        if (!sample_source_views_pair(*before[index], *after[index])) {
            return false;
        }
    }
    return true;
}

std::expected<InheritedRealtimePortStateRealization, std::string>
select_sample_inherited_state_realization(
    SampleRealtimePortStateRealization const& previous,
    SampleRealtimePortStateRealization const& current)
{
    bool predecessor_recoverable = false;
    switch (previous.retained_access) {
    case SampleRealtimePortStateAccess::immutable_constant:
        predecessor_recoverable =
            previous.authored_storage.constant_value.has_value();
        break;
    case SampleRealtimePortStateAccess::direct:
        predecessor_recoverable = previous.retained_sources.size() == 1
            && sample_storage_source_is_recoverable(
                previous.retained_sources.front());
        break;
    case SampleRealtimePortStateAccess::materialized:
        predecessor_recoverable = previous.materialization.has_value()
            && !previous.retained_sources.empty()
            && std::ranges::all_of(
                previous.retained_sources,
                sample_storage_source_is_recoverable);
        break;
    case SampleRealtimePortStateAccess::composed:
        predecessor_recoverable = !previous.composition_sources.empty()
            && std::ranges::all_of(
                previous.composition_sources,
                [](auto const& source) {
                    return source.coefficient != 0.0f
                        && sample_storage_source_is_recoverable(
                            source.storage);
                });
        break;
    case SampleRealtimePortStateAccess::callback_only:
        break;
    }
    if (!predecessor_recoverable) {
        return InheritedRealtimePortStateRealization::
            predecessor_state_unavailable;
    }

    auto const& previous_geometry = previous.authored_storage.geometry;
    auto const& current_geometry = current.authored_storage.geometry;
    if (previous_geometry.value_type != current_geometry.value_type) {
        return InheritedRealtimePortStateRealization::fresh_state;
    }
    if (previous_geometry != current_geometry) {
        auto const& type = continuous_value_type_info(
            current_geometry.value_type);
        if (type.migrate_value_shape == nullptr
            || previous.retained_access
                == SampleRealtimePortStateAccess::materialized
            || previous.retained_access
                == SampleRealtimePortStateAccess::composed) {
            return InheritedRealtimePortStateRealization::fresh_state;
        }
    }

    if (current.retained_access
        == SampleRealtimePortStateAccess::immutable_constant) {
        if (!current.authored_storage.constant_value) {
            return std::unexpected(
                "successor immutable sample state has no constant value");
        }
        if (previous.retained_access
            == SampleRealtimePortStateAccess::immutable_constant) {
            if (previous.authored_storage.geometry
                    == current.authored_storage.geometry
                && previous.authored_storage.absolute_pace_exponent
                    == current.authored_storage.absolute_pace_exponent
                && previous.authored_storage.constant_value
                    == current.authored_storage.constant_value) {
                return InheritedRealtimePortStateRealization::
                    already_represented;
            }
        }
        return InheritedRealtimePortStateRealization::
            transition_only_storage;
    }

    if (current.retained_access == SampleRealtimePortStateAccess::direct) {
        if (current.retained_sources.size() != 1
            || !current.retained_sources.front().node_storage_offset
            || current.retained_sources.front().callback_arena_offset) {
            return std::unexpected(
                "successor direct sample state has no persistent view");
        }
        auto const& target = current.retained_sources.front();
        auto const sources_are_direct_storage = previous.retained_access
                == SampleRealtimePortStateAccess::immutable_constant
            || (previous.retained_access
                    == SampleRealtimePortStateAccess::composed
                ? std::ranges::all_of(
                    previous.composition_sources,
                    [](auto const& source) {
                        return source.storage.storage
                                == SampleRealtimePortStateStorage::ring
                            || source.storage.storage
                                == SampleRealtimePortStateStorage::compact_carry;
                    })
                : std::ranges::all_of(
                    previous.retained_sources,
                    [](auto const& source) {
                        return source.storage
                                == SampleRealtimePortStateStorage::ring
                            || source.storage
                                == SampleRealtimePortStateStorage::compact_carry;
                    }));
        if ((target.storage != SampleRealtimePortStateStorage::ring
             && target.storage
                 != SampleRealtimePortStateStorage::compact_carry)
            || !sources_are_direct_storage) {
            return InheritedRealtimePortStateRealization::
                transition_only_storage;
        }
        return InheritedRealtimePortStateRealization::
            transfer_to_steady_storage;
    }
    if (sample_sources_pair(previous, current)) {
        return InheritedRealtimePortStateRealization::
            transfer_to_steady_storage;
    }
    return InheritedRealtimePortStateRealization::transition_only_storage;
}

std::expected<InheritedRealtimePortStateRealization, std::string>
select_event_inherited_state_realization(
    EventRealtimePortStateRealization const& previous,
    EventRealtimePortStateRealization const& current)
{
    bool predecessor_recoverable = false;
    switch (previous.retained_access) {
    case EventRealtimePortStateAccess::immutable_empty:
        predecessor_recoverable = true;
        break;
    case EventRealtimePortStateAccess::direct:
        predecessor_recoverable = previous.retained_storage.has_value()
            && event_storage_is_persistent_sequence(
                *previous.retained_storage);
        break;
    case EventRealtimePortStateAccess::materialized:
        predecessor_recoverable = previous.retained_storage.has_value()
            && event_storage_is_persistent_sequence(
                *previous.retained_storage)
            && previous.materialization_conversion.has_value();
        break;
    case EventRealtimePortStateAccess::indexed_merged_source:
        predecessor_recoverable = previous.retained_storage.has_value()
            && event_storage_is_persistent_sequence(
                *previous.retained_storage)
            && previous.retained_storage->has_source_indices
            && previous.retained_source_index.has_value();
        break;
    case EventRealtimePortStateAccess::shared_merged_source:
    case EventRealtimePortStateAccess::callback_only:
        break;
    }
    if (!predecessor_recoverable) {
        return InheritedRealtimePortStateRealization::
            predecessor_state_unavailable;
    }

    if (previous.retained_access
        == EventRealtimePortStateAccess::immutable_empty) {
        return InheritedRealtimePortStateRealization::already_represented;
    }
    if (current.retained_access == EventRealtimePortStateAccess::direct
        || current.retained_access
            == EventRealtimePortStateAccess::indexed_merged_source) {
        if (!current.retained_storage
            || !event_storage_is_persistent_sequence(
                *current.retained_storage)
            || (current.retained_access
                    == EventRealtimePortStateAccess::indexed_merged_source
                && (!current.retained_storage->has_source_indices
                    || !current.retained_source_index))) {
            return std::unexpected(
                "successor event state has no recoverable persistent view");
        }
        if (previous.retained_storage->type
            != current.retained_storage->type) {
            try {
                (void)EventConversionRegistry::plan(
                    previous.retained_storage->type,
                    current.retained_storage->type);
            } catch (std::exception const&) {
                return InheritedRealtimePortStateRealization::fresh_state;
            }
        }
        return InheritedRealtimePortStateRealization::
            transfer_to_steady_storage;
    }
    return InheritedRealtimePortStateRealization::transition_only_storage;
}

using StableStateIndex = std::unordered_map<
    StableRealtimePortStateId,
    LocatedState,
    StableRealtimePortStateIdHash>;

std::expected<std::vector<std::optional<LocatedState>>, std::string>
index_realizations(
    RealtimePortStateRequirements const& requirements,
    RealtimePortStateRealizations const& realizations,
    std::string_view generation)
{
    std::vector<std::optional<LocatedState>> result(
        requirements.states.size());

    auto add = [&](std::size_t realization_index,
                   std::size_t requirement_index,
                   PortKind expected_kind)
        -> std::expected<void, std::string> {
        if (requirement_index >= requirements.states.size()) {
            return std::unexpected(
                std::string{generation}
                + " realtime port-state realization names a missing requirement");
        }
        auto const& requirement = requirements.states[requirement_index];
        if (requirement.configured_port.port_kind != expected_kind) {
            return std::unexpected(
                std::string{generation}
                + " realtime port-state realization has the wrong port kind");
        }
        if (result[requirement_index]) {
            return std::unexpected(
                std::string{generation}
                + " realtime port-state requirement has multiple realizations");
        }
        result[requirement_index] = LocatedState{
            .requirement = requirement_index,
            .realization = realization_index,
            .kind = expected_kind,
        };
        return {};
    };

    for (std::size_t index = 0;
         index < realizations.sample_states.size(); ++index) {
        auto added = add(
            index,
            realizations.sample_states[index].requirement_index,
            PortKind::sample);
        if (!added) return std::unexpected(std::move(added.error()));
    }
    for (std::size_t index = 0;
         index < realizations.event_states.size(); ++index) {
        auto added = add(
            index,
            realizations.event_states[index].requirement_index,
            PortKind::event);
        if (!added) return std::unexpected(std::move(added.error()));
    }

    for (std::size_t index = 0; index < requirements.states.size(); ++index) {
        auto const& requirement = requirements.states[index];
        // A Sequential sample input's record may own no history (see
        // RealtimePortStateRequirement::in_flight_samples).
        if (requirement.extent_samples == 0
            && requirement.role
                != RealtimePortStateRole::sequential_input_history) {
            return std::unexpected(
                std::string{generation}
                + " realtime port-state requirement has an empty extent");
        }
        if (!result[index]) {
            return std::unexpected(
                std::string{generation}
                + " realtime port-state requirement has no realization");
        }
        if (requirement.configured_port.port_kind == PortKind::sample) {
            if (!requirement.channel) {
                return std::unexpected(
                    std::string{generation}
                    + " sample port-state requirement has no channel");
            }
        } else if (requirement.channel) {
            return std::unexpected(
                std::string{generation}
                + " event port-state requirement unexpectedly has a channel");
        }
        if (!requirement.stable_identity) continue;
        auto const& stable = *requirement.stable_identity;
        if (stable.direction != requirement.direction
            || stable.kind != requirement.configured_port.port_kind
            || stable.port_index != requirement.configured_port.port_index
            || stable.channel != requirement.channel
            || stable.role != requirement.role) {
            return std::unexpected(
                std::string{generation}
                + " realtime port-state stable identity disagrees with its requirement");
        }
    }
    return result;
}

std::expected<StableStateIndex, std::string> index_stable_states(
    RealtimePortStateRequirements const& requirements,
    std::vector<std::optional<LocatedState>> const& locations,
    std::string_view generation)
{
    StableStateIndex result;
    result.reserve(requirements.states.size());
    for (std::size_t index = 0; index < requirements.states.size(); ++index) {
        auto const& requirement = requirements.states[index];
        if (!requirement.stable_identity) continue;
        auto [_, inserted] = result.emplace(
            *requirement.stable_identity, *locations[index]);
        if (!inserted) {
            return std::unexpected(
                std::string{generation}
                + " realtime port-state inventory contains a duplicate stable identity");
        }
    }
    return result;
}

std::expected<RealtimePortStateTransition, std::string> make_transition(
    LocatedState const& previous,
    LocatedState const& current,
    RealtimePortStateRequirement const& previous_requirement,
    RealtimePortStateRequirement const& current_requirement)
{
    auto const inherited = std::min(
        previous_requirement.extent_samples,
        current_requirement.extent_samples);
    if (previous_requirement.extent_samples
            > static_cast<std::size_t>(
                std::numeric_limits<std::int64_t>::max())
        || current_requirement.extent_samples
        > static_cast<std::size_t>(
            std::numeric_limits<std::int64_t>::max())) {
        return std::unexpected(
            "realtime port-state extent is not representable");
    }

    std::int64_t begin = 0;
    std::int64_t end = 0;
    std::int64_t target_begin = 0;
    std::int64_t target_end = 0;
    auto const current_extent =
        static_cast<std::int64_t>(current_requirement.extent_samples);
    if (current_requirement.role
        == RealtimePortStateRole::tick_output_latency) {
        end = static_cast<std::int64_t>(inherited);
        target_end = current_extent;
    } else {
        begin = -static_cast<std::int64_t>(inherited);
        target_begin = -current_extent;
    }
    return RealtimePortStateTransition{
        .previous_requirement_index = previous.requirement,
        .current_requirement_index = current.requirement,
        .previous_realization_index = previous.realization,
        .current_realization_index = current.realization,
        .target_begin = target_begin,
        .target_end = target_end,
        .inherited_begin = begin,
        .inherited_end = end,
        .inherited_extent_samples = inherited,
        .newly_exposed_extent_samples =
            current_requirement.extent_samples - inherited,
        .discarded_extent_samples =
            previous_requirement.extent_samples - inherited,
    };
}

// The position T at which a Sequential input's source values resume after its
// in-flight values. A source value enters the input once, when its source
// finalizes it. When the input keeps reading the same producer, it already holds
// that producer's values up to its previous finalization frontier, so the next
// one lands at c_old + d_new - d_old. Another producer's first value finalized
// after the cutover lands at c_new.
std::int64_t input_resume_position(
    RealtimePortStateRequirement const& previous_requirement,
    RealtimePortStateRequirement const& current_requirement,
    SampleRealtimePortStateRealization const& previous,
    SampleRealtimePortStateRealization const& current)
{
    auto const other_producer =
        static_cast<std::int64_t>(current_requirement.in_flight_samples);
    if (previous.retained_access != SampleRealtimePortStateAccess::direct
        || current.retained_access != SampleRealtimePortStateAccess::direct
        || previous.retained_sources.size() != 1
        || current.retained_sources.size() != 1) {
        return other_producer;
    }
    auto const& before = previous.retained_sources.front();
    auto const& after = current.retained_sources.front();
    if (!before.producer || before.producer != after.producer
        || before.producer_channel != after.producer_channel) {
        return other_producer;
    }
    // A view reads physical index P + position + timeline_offset_frames, so
    // d = -timeline_offset_frames.
    auto const resume =
        static_cast<std::int64_t>(previous_requirement.in_flight_samples)
        + before.timeline_offset_frames - after.timeline_offset_frames;
    return std::max<std::int64_t>(resume, 0);
}

// Gives a Sequential input transition its window [-history, T) (see
// RealtimePortStateTransition).
std::expected<void, std::string> apply_input_window(
    RealtimePortStateTransition& transition,
    RealtimePortStateRequirement const& previous_requirement,
    RealtimePortStateRequirement const& current_requirement,
    SampleRealtimePortStateRealization const& previous,
    SampleRealtimePortStateRealization const& current)
{
    constexpr auto limit = static_cast<std::size_t>(
        std::numeric_limits<std::int64_t>::max() / 4);
    if (previous_requirement.extent_samples > limit
        || previous_requirement.in_flight_samples > limit
        || current_requirement.extent_samples > limit
        || current_requirement.in_flight_samples > limit) {
        return std::unexpected(
            "Sequential input state extent is not representable");
    }
    auto const old_history =
        static_cast<std::int64_t>(previous_requirement.extent_samples);
    auto const new_history =
        static_cast<std::int64_t>(current_requirement.extent_samples);
    auto const old_in_flight =
        static_cast<std::int64_t>(previous_requirement.in_flight_samples);
    auto const resume = input_resume_position(
        previous_requirement, current_requirement, previous, current);
    auto const kept_history = std::min(old_history, new_history);
    auto const kept_in_flight = std::min(old_in_flight, resume);
    transition.target_begin = -new_history;
    transition.target_end = resume;
    transition.inherited_begin = -kept_history;
    transition.inherited_end = kept_in_flight;
    auto const inherited = kept_history + kept_in_flight;
    transition.inherited_extent_samples = static_cast<std::size_t>(inherited);
    transition.newly_exposed_extent_samples =
        static_cast<std::size_t>(new_history + resume - inherited);
    transition.discarded_extent_samples =
        static_cast<std::size_t>(old_history + old_in_flight - inherited);
    if (transition.target_begin == transition.target_end) {
        transition.inherited_state_realization =
            InheritedRealtimePortStateRealization::fresh_state;
    } else if (inherited == 0) {
        // Only fresh positions: they must still be initialized in steady
        // storage that other states may share, through a fresh-only transfer.
        transition.inherited_state_realization =
            current.retained_access == SampleRealtimePortStateAccess::direct
            ? InheritedRealtimePortStateRealization::transfer_to_steady_storage
            : InheritedRealtimePortStateRealization::fresh_state;
    }
    return {};
}

bool same_sample_target_geometry(
    SampleRealtimePortStateStorageView const& lhs,
    SampleRealtimePortStateStorageView const& rhs)
{
    return lhs.storage == rhs.storage
        && lhs.channel_layout == rhs.channel_layout
        && lhs.geometry == rhs.geometry
        && lhs.absolute_pace_exponent == rhs.absolute_pace_exponent
        && lhs.representation_channel == rhs.representation_channel
        && lhs.working_frame_capacity == rhs.working_frame_capacity
        && lhs.storage_frame_count == rhs.storage_frame_count
        && lhs.carry_future_frames == rhs.carry_future_frames
        && lhs.storage_size_bytes == rhs.storage_size_bytes;
}

bool same_event_target_geometry(
    EventRealtimePortStateStorageView const& lhs,
    EventRealtimePortStateStorageView const& rhs)
{
    return lhs.storage == rhs.storage
        && lhs.type == rhs.type
        && lhs.event_capacity == rhs.event_capacity
        && lhs.size_bytes == rhs.size_bytes
        && lhs.alignment == rhs.alignment
        && lhs.count_relative_offset == rhs.count_relative_offset
        && lhs.read_index_relative_offset == rhs.read_index_relative_offset
        && lhs.write_index_relative_offset == rhs.write_index_relative_offset
        && lhs.has_sequential_timeline == rhs.has_sequential_timeline
        && lhs.timeline_initialized_relative_offset
            == rhs.timeline_initialized_relative_offset
        && lhs.expected_next_time_relative_offset
            == rhs.expected_next_time_relative_offset
        && lhs.events_relative_offset == rhs.events_relative_offset
        && lhs.has_source_indices == rhs.has_source_indices
        && lhs.source_indices_relative_offset
            == rhs.source_indices_relative_offset;
}

std::expected<void, std::string> classify_shared_successor_storage(
    RealtimePortStateTransitionPlan& plan,
    RealtimePortStateRequirements const& current_requirements,
    RealtimePortStateRealizations const& current_realizations)
{
    struct SampleTargetGroup {
        SampleRealtimePortStateStorageView const* geometry = nullptr;
    };
    std::unordered_map<
        SampleTargetKey,
        SampleTargetGroup,
        SampleTargetKeyHash> sample_targets;
    for (auto const& realization : current_realizations.sample_states) {
        if (realization.retained_access
                != SampleRealtimePortStateAccess::direct
            || realization.retained_sources.size() != 1) {
            continue;
        }
        auto const& target = realization.retained_sources.front();
        if (!target.node_storage_offset) continue;
        auto const key = SampleTargetKey{
            .storage_offset = *target.node_storage_offset,
            .channel = target.representation_channel,
        };
        auto& group = sample_targets[key];
        if (group.geometry
            && !same_sample_target_geometry(*group.geometry, target)) {
            return std::unexpected(
                "successor sample states disagree about shared storage geometry");
        }
        group.geometry = &target;
    }
    // Several sample states may share one successor target, such as one
    // producer ring read by several inputs. resolve_sample_transfer_conflicts()
    // decides, once their transfers exist, which of them can share it.

    struct EventTargetGroup {
        EventRealtimePortStateStorageView const* geometry = nullptr;
        std::size_t semantic_users = 0;
        std::vector<std::size_t> transitions{};
    };
    std::unordered_map<std::size_t, EventTargetGroup> event_targets;
    for (auto const& realization : current_realizations.event_states) {
        if ((realization.retained_access
                    != EventRealtimePortStateAccess::direct
                && realization.retained_access
                    != EventRealtimePortStateAccess::indexed_merged_source)
            || !realization.retained_storage
            || !realization.retained_storage->node_storage_offset) {
            continue;
        }
        auto const offset =
            *realization.retained_storage->node_storage_offset;
        auto& group = event_targets[offset];
        if (group.geometry
            && !same_event_target_geometry(
                *group.geometry, *realization.retained_storage)) {
            return std::unexpected(
                "successor event states disagree about shared storage geometry");
        }
        group.geometry = &*realization.retained_storage;
        ++group.semantic_users;
    }
    for (std::size_t index = 0; index < plan.event_states.size(); ++index) {
        auto const& transition = plan.event_states[index];
        auto const& realization = current_realizations.event_states[
            transition.current_realization_index];
        if ((realization.retained_access
                    != EventRealtimePortStateAccess::direct
                && realization.retained_access
                    != EventRealtimePortStateAccess::indexed_merged_source)
            || !realization.retained_storage
            || !realization.retained_storage->node_storage_offset) {
            continue;
        }
        event_targets[*realization.retained_storage->node_storage_offset]
            .transitions.push_back(index);
    }
    for (auto const& [_, group] : event_targets) {
        if (group.semantic_users < 2) continue;
        auto const transfers_inherited_state = std::ranges::any_of(
            group.transitions,
            [&](std::size_t index) {
                return plan.event_states[index].inherited_state_realization
                    == InheritedRealtimePortStateRealization::
                        transfer_to_steady_storage;
            });
        if (!transfers_inherited_state) continue;
        if (group.geometry && group.geometry->has_source_indices) continue;
        auto const output_roles_share_one_timeline = !group.transitions.empty()
            && std::ranges::all_of(
                group.transitions,
                [&](std::size_t index) {
                    auto const& transition = plan.event_states[index];
                    if (transition.current_requirement_index
                        >= current_requirements.states.size()) return false;
                    auto const& first = current_requirements.states[
                        plan.event_states[group.transitions.front()]
                            .current_requirement_index];
                    auto const& candidate = current_requirements.states[
                        transition.current_requirement_index];
                    return first.direction == PortDirection::output
                        && candidate.direction == first.direction
                        && candidate.configured_port == first.configured_port
                        && candidate.channel == first.channel;
                });
        if (output_roles_share_one_timeline) continue;
        for (auto const index : group.transitions) {
            auto& transition = plan.event_states[index];
            if (transition.inherited_state_realization
                != InheritedRealtimePortStateRealization::
                    predecessor_state_unavailable
                && transition.inherited_state_realization
                    != InheritedRealtimePortStateRealization::fresh_state) {
                transition.inherited_state_realization =
                    InheritedRealtimePortStateRealization::
                        transition_only_storage;
            }
        }
    }
    return {};
}

// The value a position of a successor sample window holds when no inherited
// value fills it: the value its target storage starts with when that storage
// declares one (a feedback timeline's detach() initial value, a slot-fed input
// timeline's neutral value), otherwise the successor input's neutral_value.
// A Tick output has no neutral value; new producer storage starts at zero.
std::vector<std::byte> sample_fresh_value(
    SampleRealtimePortStateStorageView const& target,
    SampleRealtimePortStateRealization const& current)
{
    if (target.initial_value) return *target.initial_value;
    if (!current.neutral_value.empty()) return current.neutral_value;
    return std::vector<std::byte>(target.geometry.value_size_bytes);
}

std::expected<void, std::string> build_steady_storage_transfers(
    RealtimePortStateTransitionPlan& plan,
    RealtimePortStateRealizations const& previous_realizations,
    RealtimePortStateRealizations const& current_realizations)
{
    plan.sample_transfers.reserve(plan.sample_states.size());
    for (std::size_t index = 0; index < plan.sample_states.size(); ++index) {
        auto const& transition = plan.sample_states[index];
        if (transition.inherited_state_realization
            != InheritedRealtimePortStateRealization::
                transfer_to_steady_storage) {
            continue;
        }
        auto const& source = previous_realizations.sample_states[
            transition.previous_realization_index];
        auto const& target = current_realizations.sample_states[
            transition.current_realization_index];
        if (transition.inherited_begin == transition.inherited_end) {
            // The window inherits nothing: its fresh positions still have to be
            // initialized in storage that other states may share.
            if (target.retained_access != SampleRealtimePortStateAccess::direct
                || target.retained_sources.size() != 1) {
                return std::unexpected(
                    "fresh-only sample state transfer has no direct successor view");
            }
            plan.sample_transfers.push_back(SampleRealtimePortStateTransfer{
                .transition_index = index,
                .source_access = SampleRealtimePortStateAccess::callback_only,
                .target_storage = target.retained_sources.front(),
                .fresh_value = sample_fresh_value(
                    target.retained_sources.front(), target),
                .target_begin = transition.target_begin,
                .target_end = transition.target_end,
                .inherited_begin = transition.target_begin,
                .inherited_end = transition.target_begin,
            });
            continue;
        }
        if (target.retained_access == SampleRealtimePortStateAccess::materialized
            || target.retained_access == SampleRealtimePortStateAccess::composed) {
            // Each paired source's values move to that source's successor
            // storage at the same read delay.
            auto const before = derivation_sources(source);
            auto const after = derivation_sources(target);
            if (after.empty() || before.size() != after.size()) {
                return std::unexpected(
                    "sample state transfer lost its paired source views");
            }
            for (std::size_t view = 0; view < after.size(); ++view) {
                plan.sample_transfers.push_back(SampleRealtimePortStateTransfer{
                    .transition_index = index,
                    .source_access = SampleRealtimePortStateAccess::direct,
                    .source_storage = {*before[view]},
                    .target_storage = *after[view],
                    .fresh_value = sample_fresh_value(*after[view], target),
                    .target_begin = transition.target_begin,
                    .target_end = transition.target_end,
                    .inherited_begin = transition.inherited_begin,
                    .inherited_end = transition.inherited_end,
                });
            }
            continue;
        }
        if (target.retained_access != SampleRealtimePortStateAccess::direct
            || target.retained_sources.size() != 1
            || !target.retained_sources.front().node_storage_offset
            || target.retained_sources.front().callback_arena_offset) {
            return std::unexpected(
                "sample state transfer lost its successor steady-storage view");
        }

        SampleRealtimePortStateTransfer transfer{
            .transition_index = index,
            .source_access = source.retained_access,
            .source_materialization = source.materialization,
            .source_materialization_channel =
                source.authored_storage.representation_channel,
            .target_storage = target.retained_sources.front(),
            .fresh_value = sample_fresh_value(
                target.retained_sources.front(), target),
            .target_begin = transition.target_begin,
            .target_end = transition.target_end,
            .inherited_begin = transition.inherited_begin,
            .inherited_end = transition.inherited_end,
        };
        if (source.retained_access
            == SampleRealtimePortStateAccess::immutable_constant) {
            if (!source.authored_storage.constant_value) {
                return std::unexpected(
                    "sample state transfer lost its predecessor constant");
            }
            transfer.source_storage.push_back(source.authored_storage);
        } else if (source.retained_access
                   == SampleRealtimePortStateAccess::composed) {
            transfer.source_storage.reserve(source.composition_sources.size());
            transfer.source_composition_coefficients.reserve(
                source.composition_sources.size());
            for (auto const& composition_source : source.composition_sources) {
                transfer.source_storage.push_back(composition_source.storage);
                transfer.source_composition_coefficients.push_back(
                    composition_source.coefficient);
            }
        } else {
            transfer.source_storage = source.retained_sources;
        }
        if (transfer.source_storage.empty()
            || !std::ranges::all_of(
                transfer.source_storage,
                sample_storage_source_is_recoverable)) {
            return std::unexpected(
                "sample state transfer lost its recoverable predecessor storage");
        }
        plan.sample_transfers.push_back(std::move(transfer));
    }

    plan.event_transfers.reserve(plan.event_states.size());
    for (std::size_t index = 0; index < plan.event_states.size(); ++index) {
        auto const& transition = plan.event_states[index];
        if (transition.inherited_state_realization
            != InheritedRealtimePortStateRealization::
                transfer_to_steady_storage) {
            continue;
        }
        auto const& source = previous_realizations.event_states[
            transition.previous_realization_index];
        auto const& target = current_realizations.event_states[
            transition.current_realization_index];
        if (!source.retained_storage
            || !source.retained_storage->node_storage_offset
            || source.retained_storage->callback_arena_offset) {
            return std::unexpected(
                "event state transfer lost its recoverable predecessor storage");
        }
        if ((target.retained_access
                    != EventRealtimePortStateAccess::direct
                && target.retained_access
                    != EventRealtimePortStateAccess::indexed_merged_source)
            || !target.retained_storage
            || !target.retained_storage->node_storage_offset
            || target.retained_storage->callback_arena_offset
            || (target.retained_access
                    == EventRealtimePortStateAccess::indexed_merged_source
                && (!target.retained_storage->has_source_indices
                    || !target.retained_source_index))) {
            return std::unexpected(
                "event state transfer lost its successor steady-storage view");
        }
        std::optional<EventConversionPlan> conversion;
        if (source.retained_storage->type
            != target.retained_storage->type) {
            try {
                conversion = EventConversionRegistry::plan(
                    source.retained_storage->type,
                    target.retained_storage->type);
            } catch (std::exception const&) {
                return std::unexpected(
                    "event state transfer lost its type conversion");
            }
        }
        plan.event_transfers.push_back(EventRealtimePortStateTransfer{
            .transition_index = index,
            .source_access = source.retained_access,
            .source_storage = *source.retained_storage,
            .source_index = source.retained_source_index,
            .source_conversion = std::move(conversion),
            .source_history_samples =
                source.materialization_history_samples,
            .source_selects_invocation_window =
                source.materialization_selects_invocation_window,
            .target_storage = *target.retained_storage,
            .target_index = target.retained_source_index,
            .target_begin = transition.inherited_begin < 0
                ? -static_cast<std::int64_t>(
                    transition.inherited_extent_samples
                    + transition.newly_exposed_extent_samples)
                : 0,
            .target_end = transition.inherited_begin < 0
                ? 0
                : static_cast<std::int64_t>(
                    transition.inherited_extent_samples
                    + transition.newly_exposed_extent_samples),
            .inherited_begin = transition.inherited_begin,
            .inherited_end = transition.inherited_end,
        });
    }

    // An input producer-group carry can contain both the input's negative
    // history interval and one or more source-output future intervals. The
    // steady producer-group merge reads callback-local source sequences, so a
    // cutover must seed those future intervals here exactly once. Ordinary
    // calls continue to use the producer-group carry and callback-local merge.
    for (auto const& target : current_realizations.event_states) {
        if (target.retained_future_sources.empty()) continue;
        if ((target.retained_access
                    != EventRealtimePortStateAccess::direct
                && target.retained_access
                    != EventRealtimePortStateAccess::materialized)
            || !target.retained_storage
            || !event_storage_is_persistent_sequence(
                *target.retained_storage)) {
            return std::unexpected(
                "event input future provenance has no successor steady storage");
        }

        for (auto const& future : target.retained_future_sources) {
            auto const transition = std::ranges::find_if(
                plan.event_states,
                [&](RealtimePortStateTransition const& candidate) {
                    return candidate.current_requirement_index
                        == future.requirement_index;
                });
            // A new output has no predecessor future to preserve. Its first
            // successor callback authors the new future normally.
            if (transition == plan.event_states.end()) continue;
            if (transition->inherited_begin != 0
                || transition->previous_realization_index
                    >= previous_realizations.event_states.size()) {
                return std::unexpected(
                    "event input future provenance names a non-latency transition");
            }
            if (transition->inherited_state_realization
                == InheritedRealtimePortStateRealization::fresh_state) {
                continue;
            }
            auto const& source = previous_realizations.event_states[
                transition->previous_realization_index];
            if (!source.retained_storage
                || !event_storage_is_persistent_sequence(
                    *source.retained_storage)) {
                return std::unexpected(
                    "event input future provenance lost its predecessor output storage");
            }

            EventConversionPlan prefix;
            try {
                prefix = EventConversionRegistry::plan(
                    source.retained_storage->type,
                    future.conversion.source_type);
            } catch (std::exception const&) {
                return std::unexpected(
                    "event input future provenance lost its output type conversion");
            }
            auto conversion = append_event_conversion(
                std::move(prefix), future.conversion);
            if (!conversion) {
                return std::unexpected(std::move(conversion.error()));
            }
            if (conversion->target_type != target.retained_storage->type) {
                return std::unexpected(
                    "event input future provenance changed retained input type");
            }

            auto const transition_index = static_cast<std::size_t>(
                std::distance(plan.event_states.begin(), transition));
            plan.event_transfers.push_back(EventRealtimePortStateTransfer{
                .transition_index = transition_index,
                .supplemental_target = true,
                .source_access = source.retained_access,
                .source_storage = *source.retained_storage,
                .source_index = source.retained_source_index,
                .source_conversion = conversion->size() == 0
                    ? std::optional<EventConversionPlan>{}
                    : std::optional<EventConversionPlan>{
                        std::move(*conversion)},
                .source_history_samples =
                    source.materialization_history_samples,
                .source_selects_invocation_window =
                    source.materialization_selects_invocation_window,
                .target_storage = *target.retained_storage,
                .target_index = future.target_source_index,
                .target_begin = 0,
                .target_end = static_cast<std::int64_t>(
                    transition->inherited_extent_samples
                    + transition->newly_exposed_extent_samples),
                .inherited_begin = transition->inherited_begin,
                .inherited_end = transition->inherited_end,
            });
        }
    }
    return {};
}

// Several sample states may transfer into one successor steady target, such as
// a producer ring read by several inputs. They may share it only where they put
// the same value into every slot they both cover: the same predecessor slot, or
// equal fresh values. A native transfer copies a producer's own values, without
// fresh slots, to the same index of that producer's successor storage, so native
// transfers always agree with each other. Every other transfer, largest first,
// is kept while it agrees with the transfers already kept on its target;
// otherwise its whole semantic state moves to transition-only storage. A state
// the predecessor does not have, or an anonymous one, starts fresh, so a kept
// transfer that puts inherited values inside its window moves too.
std::expected<void, std::string> resolve_sample_transfer_conflicts(
    RealtimePortStateTransitionPlan& plan,
    RealtimePortStateRequirements const& current_requirements,
    RealtimePortStateRealizations const& current_realizations)
{
    struct Claim {
        std::size_t transfer = 0;
        bool native = false;
        // Successor slots relative to the cutover cursor.
        std::int64_t begin = 0;
        std::int64_t end = 0;
        std::int64_t inherited_begin = 0;
        std::int64_t inherited_end = 0;
        // Where an inherited slot comes from: a predecessor view's storage and
        // channel, and the distance from the successor slot to its predecessor
        // slot. A transfer that derives its values is its own provenance.
        bool derived = false;
        std::optional<std::size_t> source_offset{};
        std::size_t source_channel = 0;
        std::int64_t shift = 0;
        // The value of every slot it covers outside its inherited slots.
        std::span<std::byte const> fresh_value{};
    };
    auto const agree = [](Claim const& a, Claim const& b) {
        auto const begin = std::max(a.begin, b.begin);
        auto const end = std::min(a.end, b.end);
        if (begin >= end) return true;
        auto const a_begin = std::max(a.inherited_begin, begin);
        auto const a_end = std::min(a.inherited_end, end);
        auto const b_begin = std::max(b.inherited_begin, begin);
        auto const b_end = std::min(b.inherited_end, end);
        auto const a_inherits = a_begin < a_end;
        auto const b_inherits = b_begin < b_end;
        auto const same_fresh_value =
            std::ranges::equal(a.fresh_value, b.fresh_value);
        if (!a_inherits && !b_inherits) return same_fresh_value;
        if (a_inherits != b_inherits || a_begin != b_begin || a_end != b_end) {
            return false;
        }
        auto const both_fresh_somewhere = a_begin != begin || a_end != end;
        return !a.derived && !b.derived && a.source_offset == b.source_offset
            && a.source_channel == b.source_channel && a.shift == b.shift
            && (!both_fresh_somewhere || same_fresh_value);
    };

    std::unordered_map<SampleTargetKey, std::vector<Claim>, SampleTargetKeyHash>
        targets;
    for (std::size_t index = 0; index < plan.sample_transfers.size(); ++index) {
        auto const& transfer = plan.sample_transfers[index];
        if (transfer.target_transition_only
            || !transfer.target_storage.node_storage_offset) {
            continue;
        }
        auto const& transition = plan.sample_states[transfer.transition_index];
        if (transition.current_requirement_index
            >= current_requirements.states.size()) {
            return std::unexpected(
                "sample state transfer names a missing successor requirement");
        }
        auto const& requirement =
            current_requirements.states[transition.current_requirement_index];
        auto const& target = transfer.target_storage;
        auto const offset = target.timeline_offset_frames;
        Claim claim{
            .transfer = index,
            .begin = transfer.target_begin + offset,
            .end = transfer.target_end + offset,
            .inherited_begin = transfer.inherited_begin + offset,
            .inherited_end = transfer.inherited_end + offset,
            .fresh_value = transfer.fresh_value,
        };
        auto const copies = transfer.source_access
                == SampleRealtimePortStateAccess::direct
            && transfer.source_storage.size() == 1;
        if (copies) {
            auto const& source = transfer.source_storage.front();
            claim.source_offset = source.node_storage_offset;
            claim.source_channel = source.representation_channel;
            claim.shift = source.timeline_offset_frames - offset;
            auto const same_producer = requirement.role
                    == RealtimePortStateRole::tick_output_history
                || (source.producer && source.producer == target.producer
                    && source.producer_channel == target.producer_channel);
            claim.native = same_producer && claim.shift == 0
                && transfer.target_begin == transfer.inherited_begin
                && transfer.target_end == transfer.inherited_end;
        } else {
            claim.derived = true;
        }
        targets[SampleTargetKey{
            .storage_offset = *target.node_storage_offset,
            .channel = target.representation_channel,
        }].push_back(claim);
    }

    std::vector<bool> moved(plan.sample_states.size(), false);
    for (auto& [_, claims] : targets) {
        std::ranges::stable_sort(claims, [](Claim const& a, Claim const& b) {
            if (a.native != b.native) return a.native;
            return a.end - a.begin > b.end - b.begin;
        });
        std::vector<Claim const*> kept;
        for (auto const& claim : claims) {
            if (claim.native) {
                kept.push_back(&claim);
                continue;
            }
            auto const fits = std::ranges::all_of(
                kept, [&](Claim const* other) { return agree(claim, *other); });
            if (fits) {
                kept.push_back(&claim);
            } else {
                moved[plan.sample_transfers[claim.transfer].transition_index] = true;
            }
        }
    }

    std::vector<bool> surviving(current_requirements.states.size(), false);
    for (auto const& transition : plan.sample_states) {
        if (transition.current_requirement_index < surviving.size()) {
            surviving[transition.current_requirement_index] = true;
        }
    }
    for (auto const& realization : current_realizations.sample_states) {
        if (realization.requirement_index >= surviving.size()
            || surviving[realization.requirement_index]
            || realization.retained_access != SampleRealtimePortStateAccess::direct
            || realization.retained_sources.size() != 1
            || !realization.retained_sources.front().node_storage_offset) {
            continue;
        }
        auto const& requirement =
            current_requirements.states[realization.requirement_index];
        std::int64_t begin = 0;
        std::int64_t end = 0;
        if (requirement.role == RealtimePortStateRole::sequential_input_history) {
            begin = -static_cast<std::int64_t>(requirement.extent_samples);
            end = static_cast<std::int64_t>(requirement.in_flight_samples);
        } else if (requirement.role
                   == RealtimePortStateRole::tick_output_history) {
            begin = -static_cast<std::int64_t>(requirement.extent_samples);
        }
        if (begin == end) continue;
        auto const& view = realization.retained_sources.front();
        auto const target = targets.find(SampleTargetKey{
            .storage_offset = *view.node_storage_offset,
            .channel = view.representation_channel,
        });
        if (target == targets.end()) continue;
        auto const offset = view.timeline_offset_frames;
        auto const fresh_value = sample_fresh_value(view, realization);
        Claim const fresh{
            .begin = begin + offset,
            .end = end + offset,
            .inherited_begin = begin + offset,
            .inherited_end = begin + offset,
            .fresh_value = fresh_value,
        };
        for (auto const& claim : target->second) {
            if (!agree(fresh, claim)) {
                moved[plan.sample_transfers[claim.transfer].transition_index] = true;
            }
        }
    }

    std::erase_if(
        plan.sample_transfers,
        [&](SampleRealtimePortStateTransfer const& transfer) {
            return moved[transfer.transition_index];
        });
    for (std::size_t index = 0; index < plan.sample_states.size(); ++index) {
        if (moved[index]) {
            plan.sample_states[index].inherited_state_realization =
                InheritedRealtimePortStateRealization::transition_only_storage;
        }
    }
    return {};
}

bool storage_extent_fits(
    std::size_t storage_size,
    std::size_t offset,
    std::size_t extent) noexcept
{
    return offset <= storage_size && extent <= storage_size - offset;
}

std::optional<std::size_t> checked_align_up(
    std::size_t value,
    std::size_t alignment) noexcept
{
    if (!std::has_single_bit(alignment)) return std::nullopt;
    auto const mask = alignment - 1;
    if (value > std::numeric_limits<std::size_t>::max() - mask) {
        return std::nullopt;
    }
    return (value + mask) & ~mask;
}

std::expected<void, std::string> validate_sample_storage_view(
    SampleRealtimePortStateStorageView const& view,
    std::size_t storage_size,
    bool allow_constant,
    std::string_view role)
{
    if (view.storage == SampleRealtimePortStateStorage::immutable_constant) {
        if (!allow_constant || view.node_storage_offset
            || view.callback_arena_offset || !view.constant_value
            || view.geometry.value_size_bytes == 0
            || view.constant_value->size()
                != view.geometry.value_size_bytes) {
            return std::unexpected(
                std::string{role} + " has an invalid immutable constant");
        }
        return {};
    }
    if ((view.storage != SampleRealtimePortStateStorage::ring
         && view.storage != SampleRealtimePortStateStorage::compact_carry)
        || !view.node_storage_offset
        || !view.sequential_cursor_node_storage_offset
        || view.callback_arena_offset
        || view.constant_value || view.geometry.value_size_bytes == 0
        || view.storage_frame_count == 0) {
        return std::unexpected(
            std::string{role} + " is not persistent sample storage");
    }
    auto const channels = channel_count(view.channel_layout);
    if (channels == 0 || view.representation_channel >= channels
        || channels > std::numeric_limits<std::size_t>::max()
                / view.storage_frame_count) {
        return std::unexpected(
            std::string{role} + " has invalid channel geometry");
    }
    auto const values = channels * view.storage_frame_count;
    if (values > std::numeric_limits<std::size_t>::max()
            / view.geometry.value_size_bytes
        || values * view.geometry.value_size_bytes
            > view.storage_size_bytes
        || !storage_extent_fits(
            storage_size,
            *view.node_storage_offset,
            view.storage_size_bytes)
        || !storage_extent_fits(
            storage_size,
            *view.sequential_cursor_node_storage_offset,
            sizeof(std::uint64_t))) {
        return std::unexpected(
            std::string{role} + " lies outside its NodeStorage");
    }
    if (view.storage == SampleRealtimePortStateStorage::ring) {
        if (view.working_frame_capacity == 0
            || view.storage_frame_count < view.working_frame_capacity) {
            return std::unexpected(
                std::string{role} + " has invalid ring capacity");
        }
    } else if (view.carry_future_frames > view.storage_frame_count) {
        return std::unexpected(
            std::string{role} + " has invalid compact-carry geometry");
    }
    return {};
}

std::optional<std::int64_t> checked_add(
    std::int64_t lhs,
    std::int64_t rhs) noexcept
{
    if ((rhs > 0 && lhs > std::numeric_limits<std::int64_t>::max() - rhs)
        || (rhs < 0
            && lhs < std::numeric_limits<std::int64_t>::min() - rhs)) {
        return std::nullopt;
    }
    return lhs + rhs;
}

std::expected<void, std::string> validate_sample_interval(
    SampleRealtimePortStateStorageView const& view,
    std::int64_t begin,
    std::int64_t end,
    std::string_view role)
{
    if (begin > end) {
        return std::unexpected(
            std::string{role} + " has a reversed semantic interval");
    }
    auto const extent = static_cast<std::uint64_t>(end)
        - static_cast<std::uint64_t>(begin);
    if (view.storage == SampleRealtimePortStateStorage::ring
        && extent > view.working_frame_capacity) {
        return std::unexpected(
            std::string{role} + " exceeds its ring capacity");
    }
    if (view.storage != SampleRealtimePortStateStorage::compact_carry) {
        return {};
    }
    auto const retained_before =
        view.storage_frame_count - view.carry_future_frames;
    if (retained_before
        > static_cast<std::size_t>(
            std::numeric_limits<std::int64_t>::max())) {
        return std::unexpected(
            std::string{role} + " has an unrepresentable compact carry");
    }
    auto first = checked_add(
        static_cast<std::int64_t>(retained_before),
        view.timeline_offset_frames);
    auto last = first ? checked_add(*first, end) : std::nullopt;
    first = first ? checked_add(*first, begin) : std::nullopt;
    if (!first || !last || *first < 0 || *last < *first
        || static_cast<std::uint64_t>(*last)
            > view.storage_frame_count) {
        return std::unexpected(
            std::string{role} + " cannot represent its semantic interval ["
            + std::to_string(begin) + ", " + std::to_string(end)
            + ") in compact-carry storage of "
            + std::to_string(view.storage_frame_count) + " frames ("
            + std::to_string(view.carry_future_frames)
            + " carried future, timeline offset "
            + std::to_string(view.timeline_offset_frames) + ")");
    }
    return {};
}

std::expected<void, std::string> validate_event_storage_view(
    EventRealtimePortStateStorageView const& view,
    std::size_t storage_size,
    bool allow_compact_carry,
    std::string_view role)
{
    auto const ring = view.storage == EventRealtimePortStateStorage::ring;
    auto const compact =
        view.storage == EventRealtimePortStateStorage::compact_carry;
    if ((!ring && (!allow_compact_carry || !compact))
        || !view.node_storage_offset || view.callback_arena_offset
        || view.event_capacity == 0
        || (ring && !std::has_single_bit(view.event_capacity))
        || !view.has_sequential_timeline
        || !std::has_single_bit(view.alignment)
        || !storage_extent_fits(
            storage_size, *view.node_storage_offset, view.size_bytes)) {
        return std::unexpected(
            std::string{role} + " is not valid persistent event storage");
    }
    auto field_fits = [&](std::size_t offset, std::size_t size) {
        return storage_extent_fits(view.size_bytes, offset, size);
    };
    if ((!ring
            && !field_fits(
                view.count_relative_offset, sizeof(std::size_t)))
        || (ring
            && (!field_fits(
                    view.read_index_relative_offset,
                    sizeof(std::size_t))
                || !field_fits(
                    view.write_index_relative_offset,
                    sizeof(std::size_t))))
        || !field_fits(
            view.timeline_initialized_relative_offset,
            sizeof(std::uint8_t))
        || !field_fits(
            view.expected_next_time_relative_offset,
            sizeof(GlobalIndex))
        || view.event_capacity > std::numeric_limits<std::size_t>::max()
                / sizeof(TimedEvent)
        || !field_fits(
            view.events_relative_offset,
            view.event_capacity * sizeof(TimedEvent))
        || (view.has_source_indices
            && (view.event_capacity
                    > std::numeric_limits<std::size_t>::max()
                        / sizeof(std::size_t)
                || !field_fits(
                    view.source_indices_relative_offset,
                    view.event_capacity * sizeof(std::size_t))))) {
        return std::unexpected(
            std::string{role} + " contains an out-of-bounds event field");
    }
    return {};
}

std::expected<void, std::string> validate_prepared_transitions(
    RealtimePortStateTransitionPlan const& plan,
    std::size_t previous_storage_size,
    std::size_t current_storage_size)
{
    std::vector<std::size_t> sample_transfer_counts(
        plan.sample_states.size());
    for (auto const& transfer : plan.sample_transfers) {
        if (transfer.transition_index >= plan.sample_states.size()) {
            return std::unexpected(
                "sample transfer names a missing semantic transition");
        }
        auto const transition_only = plan.sample_states[
            transfer.transition_index].inherited_state_realization
            == InheritedRealtimePortStateRealization::
                transition_only_storage;
        if (transfer.target_transition_only != transition_only) {
            return std::unexpected(
                "sample transfer disagrees with its target storage extent");
        }
        ++sample_transfer_counts[transfer.transition_index];
        auto const direct_source = transfer.source_access
                == SampleRealtimePortStateAccess::direct
            || transfer.source_access
                == SampleRealtimePortStateAccess::immutable_constant;
        auto const materialized_source = transfer.source_access
            == SampleRealtimePortStateAccess::materialized;
        auto const composed_source = transfer.source_access
            == SampleRealtimePortStateAccess::composed;
        // A fresh-only transfer initializes its target without inheriting.
        auto const fresh_only = transfer.source_access
                == SampleRealtimePortStateAccess::callback_only
            && transfer.source_storage.empty()
            && transfer.inherited_begin == transfer.inherited_end;
        if ((!fresh_only && !direct_source && !materialized_source
                && !composed_source)
            || (direct_source && transfer.source_storage.size() != 1)
            || ((materialized_source || composed_source)
                && transfer.source_storage.empty())
            || transfer.target_begin > transfer.inherited_begin
            || transfer.inherited_begin > transfer.inherited_end
            || transfer.inherited_end > transfer.target_end
            || transfer.fresh_value.size()
                != transfer.target_storage.geometry.value_size_bytes) {
            return std::unexpected(
                "sample transfer is not an executable retained-state transfer");
        }
        for (auto const& source : transfer.source_storage) {
            auto validated = validate_sample_storage_view(
                source,
                previous_storage_size,
                transfer.source_access
                    == SampleRealtimePortStateAccess::immutable_constant,
                "sample transition source");
            if (!validated) return validated;
        }
        auto validated = validate_sample_storage_view(
            transfer.target_storage,
            current_storage_size,
            false,
            "sample transition target");
        if (!validated) return validated;
        auto const same_geometry = std::ranges::all_of(
            transfer.source_storage,
            [&](auto const& source) {
                return source.geometry == transfer.target_storage.geometry;
            });
        auto const direct_shape_migration = transfer.source_access
                == SampleRealtimePortStateAccess::direct
            && transfer.source_storage.size() == 1
            && transfer.source_storage.front().geometry.value_type
                == transfer.target_storage.geometry.value_type
            && continuous_value_type_info(
                transfer.target_storage.geometry.value_type)
                    .migrate_value_shape != nullptr;
        if (!same_geometry && !direct_shape_migration) {
            return std::unexpected(
                "sample transition source and target geometry differ");
        }
        if (materialized_source) {
            if (!transfer.source_materialization
                || !supports_linear_audio_channel_conversion(
                    transfer.target_storage.geometry)
                || transfer.source_materialization->source_layout
                    != transfer.source_storage.front().channel_layout
                || transfer.source_materialization->target_layout
                    != transfer.target_storage.channel_layout
                || transfer.source_storage.size()
                    != channel_count(
                        transfer.source_materialization->source_layout)
                || transfer.source_materialization_channel
                    >= channel_count(
                        transfer.source_materialization->target_layout)
                || !is_registered_channel_type(
                    transfer.source_materialization->source_layout.channel_type)
                || !is_registered_channel_type(
                    transfer.source_materialization->target_layout.channel_type)) {
                return std::unexpected(
                    "sample transition materialization has incompatible channel geometry");
            }
            for (std::size_t channel = 0;
                 channel < transfer.source_storage.size(); ++channel) {
                auto const& source = transfer.source_storage[channel];
                if (source.channel_layout
                        != transfer.source_materialization->source_layout
                    || source.representation_channel != channel
                    || source.absolute_pace_exponent
                        != transfer.source_storage.front()
                            .absolute_pace_exponent
                    || source.sequential_cursor_node_storage_offset
                        != transfer.source_storage.front()
                            .sequential_cursor_node_storage_offset) {
                    return std::unexpected(
                        "sample transition materialization lost a source channel");
                }
            }
        } else if (composed_source) {
            if (transfer.source_materialization
                || transfer.source_composition_coefficients.size()
                    != transfer.source_storage.size()
                || !supports_linear_audio_channel_conversion(
                    transfer.target_storage.geometry)
                || std::ranges::any_of(
                    transfer.source_composition_coefficients,
                    [](float coefficient) {
                        return coefficient == 0.0f;
                    })) {
                return std::unexpected(
                    "sample transition composition has incompatible sources");
            }
            for (auto const& source : transfer.source_storage) {
                if (source.absolute_pace_exponent
                        != transfer.source_storage.front()
                            .absolute_pace_exponent
                    || source.sequential_cursor_node_storage_offset
                        != transfer.source_storage.front()
                            .sequential_cursor_node_storage_offset) {
                    return std::unexpected(
                        "sample transition composition lost a source timeline");
                }
            }
        } else if (transfer.source_materialization
                   || !transfer.source_composition_coefficients.empty()) {
            return std::unexpected(
                "direct sample transition unexpectedly carries a derivation");
        }
        if (transfer.source_access
            != SampleRealtimePortStateAccess::immutable_constant) {
            for (auto const& source : transfer.source_storage) {
                validated = validate_sample_interval(
                    source,
                    transfer.inherited_begin,
                    transfer.inherited_end,
                    "sample transition source");
                if (!validated) return validated;
            }
        }
        validated = validate_sample_interval(
            transfer.target_storage,
            transfer.target_begin,
            transfer.target_end,
            "sample transition target");
        if (!validated) return validated;
    }
    for (std::size_t index = 0; index < plan.sample_states.size(); ++index) {
        auto const realization =
            plan.sample_states[index].inherited_state_realization;
        if (realization
            == InheritedRealtimePortStateRealization::
                predecessor_state_unavailable) {
            return std::unexpected(
                "sample state is unavailable from the active predecessor");
        }
        if (realization
            == InheritedRealtimePortStateRealization::fresh_state) {
            if (sample_transfer_counts[index] != 0) {
                return std::unexpected(
                    "fresh sample state unexpectedly has a transfer");
            }
            continue;
        }
        if (realization
                == InheritedRealtimePortStateRealization::
                    transfer_to_steady_storage
            && sample_transfer_counts[index] != 0) {
            // A converted or composed input transfers once per paired source.
            continue;
        }
        auto const expected =
            realization
                    == InheritedRealtimePortStateRealization::
                        transfer_to_steady_storage
                || realization
                    == InheritedRealtimePortStateRealization::
                        transition_only_storage
            ? std::size_t{1}
            : std::size_t{0};
        if (sample_transfer_counts[index] != expected) {
            if (realization
                    == InheritedRealtimePortStateRealization::
                        transition_only_storage
                && sample_transfer_counts[index] == 0) {
                return std::unexpected(
                    "sample state requires a transition-only storage suffix");
            }
            return std::unexpected(
                "sample semantic transition has inconsistent transfers");
        }
    }

    for (std::size_t index = 0;
         index < plan.sample_expiry_cursors.size(); ++index) {
        auto const& expiry = plan.sample_expiry_cursors[index];
        if (expiry.value_count == 0
            || !storage_extent_fits(
                current_storage_size,
                expiry.node_storage_offset,
                sizeof(std::uint64_t))
            || std::ranges::any_of(
                plan.sample_expiry_cursors.begin(),
                plan.sample_expiry_cursors.begin()
                    + static_cast<std::ptrdiff_t>(index),
                [&](auto const& previous) {
                    return previous.node_storage_offset
                        == expiry.node_storage_offset;
                })) {
            return std::unexpected(
                "sample transition has an invalid expiry cursor");
        }
    }
    for (auto const& transfer : plan.sample_transfers) {
        if (!transfer.target_transition_only) continue;
        auto const cursor =
            transfer.target_storage.sequential_cursor_node_storage_offset;
        auto const extent = static_cast<std::uint64_t>(transfer.target_end)
            - static_cast<std::uint64_t>(transfer.target_begin);
        if (!cursor
            || std::ranges::none_of(
                plan.sample_expiry_cursors,
                [&](auto const& expiry) {
                    return expiry.node_storage_offset == *cursor
                        && expiry.value_count >= extent;
                })) {
            return std::unexpected(
                "transition-only sample state has no complete expiry cursor");
        }
    }

    std::vector<std::size_t> event_transfer_counts(plan.event_states.size());
    for (std::size_t transfer_index = 0;
         transfer_index < plan.event_transfers.size(); ++transfer_index) {
        auto const& transfer = plan.event_transfers[transfer_index];
        if (transfer.transition_index >= plan.event_states.size()) {
            return std::unexpected(
                "event transfer names a missing semantic transition");
        }
        auto const transition_only = plan.event_states[
            transfer.transition_index].inherited_state_realization
            == InheritedRealtimePortStateRealization::transition_only_storage;
        if ((!transfer.supplemental_target
                && transfer.target_transition_only != transition_only)
            || (transfer.supplemental_target
                && transfer.target_transition_only)) {
            return std::unexpected(
                "event transfer disagrees with its target storage extent");
        }
        if (!transfer.supplemental_target) {
            ++event_transfer_counts[transfer.transition_index];
        }
        if ((transfer.source_access != EventRealtimePortStateAccess::direct
             && transfer.source_access
                != EventRealtimePortStateAccess::materialized
             && transfer.source_access
                != EventRealtimePortStateAccess::indexed_merged_source)
            || transfer.target_begin > transfer.inherited_begin
            || transfer.inherited_begin > transfer.inherited_end
            || transfer.inherited_end > transfer.target_end) {
            return std::unexpected(
                "event transfer is not an executable direct transfer");
        }
        auto validated = validate_event_storage_view(
            transfer.source_storage,
            previous_storage_size,
            true,
            "event transition source");
        if (!validated) return validated;
        validated = validate_event_storage_view(
            transfer.target_storage,
            current_storage_size,
            true,
            "event transition target");
        if (!validated) return validated;
        if (transfer.target_storage.has_source_indices
            != transfer.target_index.has_value()) {
            return std::unexpected(
                "event transition target has inconsistent source identity");
        }
        for (std::size_t previous_index = 0;
             previous_index < transfer_index; ++previous_index) {
            auto const& previous = plan.event_transfers[previous_index];
            if (previous.target_storage.node_storage_offset
                != transfer.target_storage.node_storage_offset) {
                continue;
            }
            if (!same_event_target_geometry(
                    previous.target_storage, transfer.target_storage)
                || previous.target_transition_only
                    != transfer.target_transition_only) {
                return std::unexpected(
                    "event transitions disagree about shared target storage");
            }
            if (!transfer.supplemental_target
                && !previous.supplemental_target
                && transfer.target_index == previous.target_index
                && transfer.inherited_begin < previous.inherited_end
                && previous.inherited_begin < transfer.inherited_end) {
                return std::unexpected(
                    "event transitions overlap in shared target storage");
            }
        }
        auto const converted = transfer.source_conversion.has_value();
        if ((!converted
                && transfer.source_storage.type
                    != transfer.target_storage.type)
            || (converted
                && (transfer.source_conversion->source_type
                        != transfer.source_storage.type
                    || transfer.source_conversion->target_type
                        != transfer.target_storage.type
                    || transfer.source_conversion->step_count
                        > EventConversionPlan::max_steps))
            || (transfer.source_access
                    == EventRealtimePortStateAccess::indexed_merged_source
                && (!transfer.source_index
                    || !transfer.source_storage.has_source_indices))
            || (transfer.source_access
                    != EventRealtimePortStateAccess::indexed_merged_source
                && transfer.source_index)) {
            return std::unexpected(
                "event transition has incompatible selection or conversion");
        }
    }
    for (std::size_t index = 0; index < plan.event_states.size(); ++index) {
        auto const realization =
            plan.event_states[index].inherited_state_realization;
        if (realization
            == InheritedRealtimePortStateRealization::
                predecessor_state_unavailable) {
            return std::unexpected(
                "event state is unavailable from the active predecessor");
        }
        auto const expected = realization
                == InheritedRealtimePortStateRealization::
                    transfer_to_steady_storage
                || realization
                    == InheritedRealtimePortStateRealization::
                        transition_only_storage
            ? std::size_t{1}
            : std::size_t{0};
        if (event_transfer_counts[index] != expected) {
            return std::unexpected(
                "event semantic transition has inconsistent transfers");
        }
    }
    return {};
}

} // namespace

PreparedRealtimePortStateTransition::PreparedRealtimePortStateTransition(
    RealtimePortStateTransitionPlan plan,
    std::size_t previous_storage_size,
    std::size_t current_storage_size)
    : plan_(std::move(plan))
    , previous_storage_size_(previous_storage_size)
    , current_storage_size_(current_storage_size)
{}

std::expected<RealtimePortStateTransitionPlan, std::string>
plan_realtime_port_state_transition(
    RealtimePortStateRequirements const& previous_requirements,
    RealtimePortStateRealizations const& previous_realizations,
    RealtimePortStateRequirements const& current_requirements,
    RealtimePortStateRealizations const& current_realizations)
{
    auto previous_locations = index_realizations(
        previous_requirements, previous_realizations, "previous");
    if (!previous_locations) {
        return std::unexpected(std::move(previous_locations.error()));
    }
    auto current_locations = index_realizations(
        current_requirements, current_realizations, "current");
    if (!current_locations) {
        return std::unexpected(std::move(current_locations.error()));
    }
    auto previous_index = index_stable_states(
        previous_requirements, *previous_locations, "previous");
    if (!previous_index) {
        return std::unexpected(std::move(previous_index.error()));
    }
    auto current_index = index_stable_states(
        current_requirements, *current_locations, "current");
    if (!current_index) {
        return std::unexpected(std::move(current_index.error()));
    }

    RealtimePortStateTransitionPlan result;
    result.sample_states.reserve(current_realizations.sample_states.size());
    result.event_states.reserve(current_realizations.event_states.size());
    for (std::size_t index = 0;
         index < current_requirements.states.size(); ++index) {
        auto const& current_requirement = current_requirements.states[index];
        if (!current_requirement.stable_identity) continue;
        auto const previous = previous_index->find(
            *current_requirement.stable_identity);
        if (previous == previous_index->end()) continue;
        auto const& current = *(*current_locations)[index];
        if (previous->second.kind != current.kind) {
            return std::unexpected(
                "surviving realtime port state changed port kind");
        }
        auto transition = make_transition(
            previous->second,
            current,
            previous_requirements.states[previous->second.requirement],
            current_requirement);
        if (!transition) {
            return std::unexpected(std::move(transition.error()));
        }
        if (current.kind == PortKind::sample) {
            auto const& previous_realization =
                previous_realizations.sample_states[
                    previous->second.realization];
            auto const& current_realization =
                current_realizations.sample_states[current.realization];
            auto realization = select_sample_inherited_state_realization(
                previous_realization, current_realization);
            if (!realization) {
                return std::unexpected(std::move(realization.error()));
            }
            transition->inherited_state_realization = *realization;
            if (current_requirement.role
                == RealtimePortStateRole::sequential_input_history) {
                auto windowed = apply_input_window(
                    *transition,
                    previous_requirements.states[previous->second.requirement],
                    current_requirement,
                    previous_realization,
                    current_realization);
                if (!windowed) return std::unexpected(std::move(windowed.error()));
            }
            result.sample_states.push_back(std::move(*transition));
        } else {
            auto realization = select_event_inherited_state_realization(
                previous_realizations.event_states[
                    previous->second.realization],
                current_realizations.event_states[current.realization]);
            if (!realization) {
                return std::unexpected(std::move(realization.error()));
            }
            transition->inherited_state_realization = *realization;
            result.event_states.push_back(std::move(*transition));
        }
    }
    auto classified = classify_shared_successor_storage(
        result, current_requirements, current_realizations);
    if (!classified) {
        return std::unexpected(std::move(classified.error()));
    }
    auto transfers = build_steady_storage_transfers(
        result, previous_realizations, current_realizations);
    if (!transfers) {
        return std::unexpected(std::move(transfers.error()));
    }
    auto resolved = resolve_sample_transfer_conflicts(
        result, current_requirements, current_realizations);
    if (!resolved) return std::unexpected(std::move(resolved.error()));
    return result;
}

std::expected<RealtimePortStateTransitionStorage, std::string>
realize_realtime_port_state_transition_storage(
    NodeLayout steady_layout,
    RealtimePortStateTransitionPlan plan,
    RealtimePortStateRealizations const& previous_realizations,
    RealtimePortStateRequirements const& current_requirements,
    RealtimePortStateRealizations const& current_realizations)
{
    if (!plan.sample_expiry_cursors.empty()
        || plan.event_expiry_samples != 0) {
        return std::unexpected(
            "transition storage already has an expiry horizon");
    }
    NodeLayoutTransitionBuilder layout_builder(std::move(steady_layout));
    std::vector<std::pair<std::size_t, NodeLayout::RegionHandle>>
        suffix_targets;
    std::vector<std::pair<std::size_t, NodeLayout::RegionHandle>>
        event_suffix_targets;
    struct SuffixTargetGroup {
        NodeBundlePortId configured_port{};
        PortDirection direction = PortDirection::input;
        std::optional<std::size_t> steady_storage_offset{};
        SampleRealtimePortStateStorageView geometry{};
        NodeLayout::RegionHandle region{};
        struct Occupant {
            std::size_t channel = 0;
            std::int64_t begin = 0;
            std::int64_t end = 0;
        };
        std::vector<Occupant> occupants{};
    };
    std::vector<SuffixTargetGroup> suffix_groups;
    struct EventSuffixTargetGroup {
        NodeBundlePortId configured_port{};
        PortDirection direction = PortDirection::input;
        EventRealtimePortStateStorageView geometry{};
        NodeLayout::RegionHandle region{};
        struct Occupant {
            std::int64_t begin = 0;
            std::int64_t end = 0;
        };
        std::vector<Occupant> occupants{};
    };
    std::vector<EventSuffixTargetGroup> event_suffix_groups;

    for (std::size_t index = 0; index < plan.sample_states.size(); ++index) {
        auto const& transition = plan.sample_states[index];
        if (transition.inherited_state_realization
            != InheritedRealtimePortStateRealization::
                transition_only_storage) {
            continue;
        }
        if (transition.previous_realization_index
                >= previous_realizations.sample_states.size()
            || transition.current_realization_index
                >= current_realizations.sample_states.size()
            || transition.current_requirement_index
                >= current_requirements.states.size()) {
            return std::unexpected(
                "sample transition-only state names a missing realization");
        }
        if (std::ranges::any_of(
                plan.sample_transfers,
                [index](auto const& transfer) {
                    return transfer.transition_index == index;
                })) {
            return std::unexpected(
                "sample transition-only state already has a physical transfer");
        }

        auto const& source = previous_realizations.sample_states[
            transition.previous_realization_index];
        auto const& current = current_realizations.sample_states[
            transition.current_realization_index];
        auto const& requirement = current_requirements.states[
            transition.current_requirement_index];
        // A window that inherits nothing needs no predecessor view.
        auto const fresh_only =
            transition.inherited_begin == transition.inherited_end;
        if ((!fresh_only
             && source.retained_access
                 != SampleRealtimePortStateAccess::direct
             && source.retained_access
                 != SampleRealtimePortStateAccess::immutable_constant
            && source.retained_access
                 != SampleRealtimePortStateAccess::materialized
             && source.retained_access
                 != SampleRealtimePortStateAccess::composed)
            || (source.retained_access
                    == SampleRealtimePortStateAccess::direct
                && source.retained_sources.size() != 1)
            || (source.retained_access
                    == SampleRealtimePortStateAccess::materialized
                && (!source.materialization
                    || source.retained_sources.empty()))
            || (source.retained_access
                    == SampleRealtimePortStateAccess::composed
                && source.composition_sources.empty())
            || (current.retained_access
                    == SampleRealtimePortStateAccess::direct
                && current.retained_sources.size() != 1)
            || (current.retained_access
                    != SampleRealtimePortStateAccess::direct
                && current.retained_access
                    != SampleRealtimePortStateAccess::materialized
                && current.retained_access
                    != SampleRealtimePortStateAccess::composed)) {
            return std::unexpected(
                "transition suffix requires a recoverable predecessor and supported successor sample view");
        }

        auto source_views = source.retained_access
                == SampleRealtimePortStateAccess::immutable_constant
            ? std::vector<SampleRealtimePortStateStorageView>{
                source.authored_storage}
            : source.retained_sources;
        std::vector<float> source_composition_coefficients;
        if (source.retained_access
            == SampleRealtimePortStateAccess::composed) {
            source_views.clear();
            source_views.reserve(source.composition_sources.size());
            source_composition_coefficients.reserve(
                source.composition_sources.size());
            for (auto const& composition_source :
                 source.composition_sources) {
                source_views.push_back(composition_source.storage);
                source_composition_coefficients.push_back(
                    composition_source.coefficient);
            }
        }
        if (fresh_only) {
            source_views.clear();
            source_composition_coefficients.clear();
        }
        std::optional<std::size_t> steady_target_offset;
        // The suffix stands in for the storage the steady successor reads.
        auto fresh_value = sample_fresh_value(
            current.retained_access == SampleRealtimePortStateAccess::direct
                ? current.retained_sources.front()
                : current.authored_storage,
            current);
        auto target = current.authored_storage;
        if (current.retained_access
            == SampleRealtimePortStateAccess::direct) {
            auto const& steady_target = current.retained_sources.front();
            steady_target_offset = steady_target.node_storage_offset;
            target = steady_target;
        }
        auto const retained_timeline_offset = target.timeline_offset_frames;
        if (target.storage
                == SampleRealtimePortStateStorage::compact_carry
            || target.storage
                == SampleRealtimePortStateStorage::callback_transient) {
            // Expansion needs the authored working-ring geometry, but the
            // transfer still addresses the retained semantic timeline.
            target = current.authored_storage;
            target.timeline_offset_frames = retained_timeline_offset;
            auto const channels = channel_count(target.channel_layout);
            if (target.storage
                    != SampleRealtimePortStateStorage::callback_transient
                || target.working_frame_capacity == 0
                || channels == 0
                || target.geometry.value_size_bytes == 0
                || channels > std::numeric_limits<std::size_t>::max()
                        / target.working_frame_capacity
                || channels * target.working_frame_capacity
                        > std::numeric_limits<std::size_t>::max()
                            / target.geometry.value_size_bytes) {
                return std::unexpected(
                    "transition suffix cannot expand the successor compact carry");
            }
            target.storage = SampleRealtimePortStateStorage::ring;
            target.storage_frame_count = target.working_frame_capacity;
            target.storage_size_bytes = channels
                * target.storage_frame_count
                * target.geometry.value_size_bytes;
            target.node_storage_offset.reset();
            target.callback_arena_offset.reset();
        }
        if (!std::ranges::all_of(
                source_views,
                sample_storage_source_is_recoverable)
            || target.storage != SampleRealtimePortStateStorage::ring
            || target.callback_arena_offset
            || target.constant_value
            || target.storage_size_bytes == 0
            || target.geometry.value_size_bytes == 0
            || !std::ranges::all_of(
                source_views,
                [&](auto const& view) {
                    return view.geometry == target.geometry
                        || (source.retained_access
                                != SampleRealtimePortStateAccess::materialized
                            && view.geometry.value_type
                                == target.geometry.value_type
                            && continuous_value_type_info(
                                target.geometry.value_type)
                                    .migrate_value_shape != nullptr);
                })) {
            return std::unexpected(
                "transition suffix sample state has incompatible source or target geometry");
        }
        if (transition.inherited_extent_samples
            > std::numeric_limits<std::size_t>::max()
                - transition.newly_exposed_extent_samples) {
            return std::unexpected(
                "transition suffix sample extent overflows size_t");
        }
        auto const target_extent = transition.inherited_extent_samples
            + transition.newly_exposed_extent_samples;
        if (!std::in_range<std::int64_t>(target_extent)) {
            return std::unexpected(
                "transition suffix sample extent is not representable");
        }

        auto const target_begin = transition.target_begin;
        auto const target_end = transition.target_end;
        if (target_end - target_begin
            != static_cast<std::int64_t>(target_extent)) {
            return std::unexpected(
                "transition suffix sample interval disagrees with its extent");
        }

        auto const same_physical_target = [&](SuffixTargetGroup const& group) {
            auto const& candidate = group.geometry;
            return group.steady_storage_offset == steady_target_offset
                && group.configured_port == requirement.configured_port
                && group.direction == requirement.direction
                && candidate.storage == target.storage
                && candidate.channel_layout == target.channel_layout
                && candidate.geometry == target.geometry
                && candidate.absolute_pace_exponent
                    == target.absolute_pace_exponent
                && candidate.working_frame_capacity
                    == target.working_frame_capacity
                && candidate.storage_frame_count == target.storage_frame_count
                && candidate.storage_size_bytes == target.storage_size_bytes;
        };
        auto group = std::ranges::find_if(
            suffix_groups,
            [&](SuffixTargetGroup const& candidate) {
                if (!same_physical_target(candidate)) return false;
                return std::ranges::none_of(
                    candidate.occupants,
                    [&](SuffixTargetGroup::Occupant const& occupant) {
                        return occupant.channel
                                == target.representation_channel
                            && target_begin < occupant.end
                            && occupant.begin < target_end;
                    });
            });
        if (group == suffix_groups.end()) {
            auto const region = layout_builder.declare_raw_region(
                target.storage_size_bytes,
                target.geometry.element_alignment);
            suffix_groups.push_back(SuffixTargetGroup{
                .configured_port = requirement.configured_port,
                .direction = requirement.direction,
                .steady_storage_offset = steady_target_offset,
                .geometry = target,
                .region = region,
            });
            group = std::prev(suffix_groups.end());
        }
        group->occupants.push_back(SuffixTargetGroup::Occupant{
            .channel = target.representation_channel,
            .begin = target_begin,
            .end = target_end,
        });

        target.node_storage_offset.reset();
        target.callback_arena_offset.reset();
        plan.sample_transfers.push_back(SampleRealtimePortStateTransfer{
            .transition_index = index,
            .target_transition_only = true,
            .source_access = fresh_only
                ? SampleRealtimePortStateAccess::callback_only
                : source.retained_access,
            .source_storage = source_views,
            .source_materialization = fresh_only
                ? std::nullopt
                : source.materialization,
            .source_materialization_channel =
                source.authored_storage.representation_channel,
            .source_composition_coefficients =
                std::move(source_composition_coefficients),
            .target_storage = std::move(target),
            .fresh_value = std::move(fresh_value),
            .target_begin = target_begin,
            .target_end = target_end,
            .inherited_begin = transition.inherited_begin,
            .inherited_end = transition.inherited_end,
        });
        suffix_targets.emplace_back(
            plan.sample_transfers.size() - 1, group->region);
    }

    for (std::size_t index = 0; index < plan.event_states.size(); ++index) {
        auto const& transition = plan.event_states[index];
        if (transition.inherited_state_realization
            != InheritedRealtimePortStateRealization::
                transition_only_storage) continue;
        if (transition.previous_realization_index
                >= previous_realizations.event_states.size()
            || transition.current_realization_index
                >= current_realizations.event_states.size()
            || transition.current_requirement_index
                >= current_requirements.states.size()) {
            return std::unexpected(
                "event transition-only state names a missing realization");
        }
        auto const& source = previous_realizations.event_states[
            transition.previous_realization_index];
        auto const& current = current_realizations.event_states[
            transition.current_realization_index];
        auto const& requirement = current_requirements.states[
            transition.current_requirement_index];
        if (!source.retained_storage
            || !event_storage_is_persistent_sequence(
                *source.retained_storage)
            || (source.retained_access
                    != EventRealtimePortStateAccess::direct
                && source.retained_access
                    != EventRealtimePortStateAccess::materialized
                && source.retained_access
                    != EventRealtimePortStateAccess::indexed_merged_source)
            || current.authored_storage.event_capacity == 0) {
            return std::unexpected(
                "transition suffix requires recoverable predecessor event storage");
        }

        auto const requested_capacity = std::max(
            current.authored_storage.event_capacity,
            current.retained_storage
                ? current.retained_storage->event_capacity
                : std::size_t{0});
        constexpr auto maximum_ring_capacity =
            std::size_t{1}
            << (std::numeric_limits<std::size_t>::digits - 1);
        if (requested_capacity > maximum_ring_capacity) {
            return std::unexpected(
                "transition suffix event ring capacity overflows size_t");
        }
        auto const capacity = std::bit_ceil(requested_capacity);
        auto const expected_offset = checked_align_up(
            2 * sizeof(std::size_t) + sizeof(std::uint8_t),
            alignof(GlobalIndex));
        auto const events_offset = expected_offset
            ? checked_align_up(
                *expected_offset + sizeof(GlobalIndex),
                alignof(TimedEvent))
            : std::nullopt;
        if (!events_offset
            || capacity > (std::numeric_limits<std::size_t>::max()
                    - *events_offset) / sizeof(TimedEvent)) {
            return std::unexpected(
                "transition suffix event ring size overflows size_t");
        }
        auto const size = *events_offset + capacity * sizeof(TimedEvent);
        auto const alignment = std::max(
            {alignof(std::size_t), alignof(GlobalIndex), alignof(TimedEvent)});
        auto target = EventRealtimePortStateStorageView{
            .storage = EventRealtimePortStateStorage::ring,
            .type = current.authored_storage.type,
            .event_capacity = capacity,
            .size_bytes = size,
            .alignment = alignment,
            .read_index_relative_offset = 0,
            .write_index_relative_offset = sizeof(std::size_t),
            .has_sequential_timeline = true,
            .timeline_initialized_relative_offset = 2 * sizeof(std::size_t),
            .expected_next_time_relative_offset = *expected_offset,
            .events_relative_offset = *events_offset,
        };
        if (transition.inherited_extent_samples
                > std::numeric_limits<std::size_t>::max()
                    - transition.newly_exposed_extent_samples) {
            return std::unexpected(
                "transition suffix event extent overflows size_t");
        }
        auto const target_extent = transition.inherited_extent_samples
            + transition.newly_exposed_extent_samples;
        if (!std::in_range<std::int64_t>(target_extent)) {
            return std::unexpected(
                "transition suffix event extent is not representable");
        }
        auto const target_begin = transition.inherited_begin < 0
            ? -static_cast<std::int64_t>(target_extent)
            : 0;
        auto const target_end = transition.inherited_begin < 0
            ? 0
            : static_cast<std::int64_t>(target_extent);
        auto group = std::ranges::find_if(
            event_suffix_groups,
            [&](EventSuffixTargetGroup const& candidate) {
                if (candidate.configured_port != requirement.configured_port
                    || candidate.direction != requirement.direction
                    || !same_event_target_geometry(
                        candidate.geometry, target)) {
                    return false;
                }
                return std::ranges::none_of(
                    candidate.occupants,
                    [&](EventSuffixTargetGroup::Occupant const& occupant) {
                        return target_begin < occupant.end
                            && occupant.begin < target_end;
                    });
            });
        if (group == event_suffix_groups.end()) {
            auto const region = layout_builder.declare_raw_region(
                size, alignment);
            event_suffix_groups.push_back(EventSuffixTargetGroup{
                .configured_port = requirement.configured_port,
                .direction = requirement.direction,
                .geometry = target,
                .region = region,
            });
            group = std::prev(event_suffix_groups.end());
        }
        group->occupants.push_back({
            .begin = target_begin,
            .end = target_end,
        });
        std::optional<EventConversionPlan> conversion;
        if (source.retained_storage->type != target.type) {
            try {
                conversion = EventConversionRegistry::plan(
                    source.retained_storage->type, target.type);
            } catch (std::exception const&) {
                return std::unexpected(
                    "transition suffix event type has no conversion");
            }
        }
        plan.event_transfers.push_back(EventRealtimePortStateTransfer{
            .transition_index = index,
            .target_transition_only = true,
            .source_access = source.retained_access,
            .source_storage = *source.retained_storage,
            .source_index = source.retained_source_index,
            .source_conversion = std::move(conversion),
            .source_history_samples = source.materialization_history_samples,
            .source_selects_invocation_window =
                source.materialization_selects_invocation_window,
            .target_storage = std::move(target),
            .target_begin = target_begin,
            .target_end = target_end,
            .inherited_begin = transition.inherited_begin,
            .inherited_end = transition.inherited_end,
        });
        event_suffix_targets.emplace_back(
            plan.event_transfers.size() - 1, group->region);
        plan.event_expiry_samples = std::max<std::uint64_t>(
            plan.event_expiry_samples,
            transition.inherited_extent_samples
                + transition.newly_exposed_extent_samples);
    }

    auto node_layout = std::move(layout_builder).build();
    for (auto const& [transfer_index, region] : suffix_targets) {
        if (transfer_index >= plan.sample_transfers.size()
            || region.index >= node_layout.regions.size()
            || !node_layout.regions[region.index].transition_only) {
            return std::unexpected(
                "transition suffix sample target lost its layout region");
        }
        plan.sample_transfers[transfer_index].target_storage
            .node_storage_offset =
                node_layout.regions[region.index].storage_offset;
    }
    for (auto const& [transfer_index, region] : event_suffix_targets) {
        if (transfer_index >= plan.event_transfers.size()
            || region.index >= node_layout.regions.size()
            || !node_layout.regions[region.index].transition_only) {
            return std::unexpected(
                "transition suffix event target lost its layout region");
        }
        plan.event_transfers[transfer_index].target_storage
            .node_storage_offset =
                node_layout.regions[region.index].storage_offset;
    }

    for (auto const& transfer : plan.sample_transfers) {
        if (!transfer.target_transition_only) continue;
        if (!transfer.target_storage.sequential_cursor_node_storage_offset) {
            return std::unexpected(
                "transition suffix sample target lost its Sequential pace cursor");
        }
        auto const extent = static_cast<std::uint64_t>(transfer.target_end)
            - static_cast<std::uint64_t>(transfer.target_begin);
        if (extent == 0) {
            return std::unexpected(
                "transition suffix sample target has no expiry horizon");
        }
        auto const offset =
            *transfer.target_storage.sequential_cursor_node_storage_offset;
        auto expiry = std::ranges::find(
            plan.sample_expiry_cursors,
            offset,
            &SampleRealtimePortStateExpiryCursor::node_storage_offset);
        if (expiry == plan.sample_expiry_cursors.end()) {
            plan.sample_expiry_cursors.push_back({
                .node_storage_offset = offset,
                .value_count = extent,
            });
        } else {
            expiry->value_count = std::max(expiry->value_count, extent);
        }
    }

    return RealtimePortStateTransitionStorage{
        .node_layout = std::move(node_layout),
        .plan = std::move(plan),
    };
}

std::expected<PreparedRealtimePortStateTransition, std::string>
prepare_realtime_port_state_transfers(
    RealtimePortStateTransitionPlan plan,
    std::span<std::byte const> previous_storage,
    std::span<std::byte> current_storage)
{
    auto validated = validate_prepared_transitions(
        plan, previous_storage.size(), current_storage.size());
    if (!validated) {
        return std::unexpected(std::move(validated.error()));
    }
    std::unordered_map<std::size_t, std::int64_t> event_target_order_begins;
    event_target_order_begins.reserve(plan.event_transfers.size());
    for (auto const& transfer : plan.event_transfers) {
        IV_ASSERT(
            transfer.target_storage.node_storage_offset.has_value(),
            "validated event transition target has no storage offset");
        auto const offset = *transfer.target_storage.node_storage_offset;
        auto [position, inserted] = event_target_order_begins.emplace(
            offset, transfer.target_begin);
        if (!inserted) {
            position->second = std::min(
                position->second, transfer.target_begin);
        }
    }
    for (auto& transfer : plan.event_transfers) {
        transfer.target_order_begin = event_target_order_begins.at(
            *transfer.target_storage.node_storage_offset);
    }
    return PreparedRealtimePortStateTransition{
        std::move(plan), previous_storage.size(), current_storage.size()};
}

void execute_realtime_port_state_transfers(
    PreparedRealtimePortStateTransition const& transition,
    std::span<std::byte const> previous_storage,
    std::span<std::byte> current_storage,
    GlobalIndex sample_index) noexcept
{
    IV_ASSERT(
        previous_storage.size() == transition.previous_storage_size_,
        "prepared transition predecessor storage size changed");
    IV_ASSERT(
        current_storage.size() == transition.current_storage_size_,
        "prepared transition successor storage size changed");
    auto const& plan = transition.plan_;
    auto storage_base = [](auto bytes, auto const& view)
        -> decltype(bytes.data()) {
        auto const size = [&] {
            if constexpr (requires { view.storage_size_bytes; }) {
                return view.storage_size_bytes;
            } else {
                return view.size_bytes;
            }
        }();
        if (!view.node_storage_offset
            || *view.node_storage_offset > bytes.size()
            || size
                > bytes.size() - *view.node_storage_offset) {
            return nullptr;
        }
        return bytes.data() + *view.node_storage_offset;
    };
    auto relative_sample = [](GlobalIndex origin, std::int64_t relative) {
        return origin.shifted_by(relative);
    };
    auto sequential_cursor = [](auto bytes,
                                SampleRealtimePortStateStorageView const& view) {
        std::uint64_t result = 0;
        IV_ASSERT(
            view.sequential_cursor_node_storage_offset
                && *view.sequential_cursor_node_storage_offset
                    <= bytes.size()
                && sizeof(result)
                    <= bytes.size()
                        - *view.sequential_cursor_node_storage_offset,
            "prepared sample transition pace cursor is out of bounds");
        std::memcpy(
            &result,
            bytes.data() + *view.sequential_cursor_node_storage_offset,
            sizeof(result));
        return result;
    };
    auto shifted_index = [](std::uint64_t origin, std::int64_t relative) {
        return origin + static_cast<std::uint64_t>(relative);
    };
    auto sample_at = [&](auto bytes,
                         SampleRealtimePortStateStorageView const& view,
                         std::uint64_t root_index,
                         std::int64_t relative) -> decltype(bytes.data()) {
        auto* base = storage_base(bytes, view);
        auto const physical = shifted_index(
            shifted_index(root_index, relative),
            view.timeline_offset_frames);
        std::optional<std::size_t> frame;
        if (view.storage == SampleRealtimePortStateStorage::ring) {
            if (view.working_frame_capacity != 0) {
                frame = physical % view.working_frame_capacity;
            }
        } else if (view.storage
                   == SampleRealtimePortStateStorage::compact_carry) {
            if (view.storage_frame_count >= view.carry_future_frames) {
                auto const retained_before =
                    view.storage_frame_count - view.carry_future_frames;
                auto const first = root_index - retained_before;
                auto const candidate = physical - first;
                if (candidate < view.storage_frame_count) frame = candidate;
            }
        }
        auto const channels = channel_count(view.channel_layout);
        if (!base || !frame || channels == 0
            || view.representation_channel >= channels) {
            return nullptr;
        }
        auto const sample = view.channel_layout.sample_layout
                == SampleStreamLayout::planar
            ? view.representation_channel * view.storage_frame_count + *frame
            : *frame * channels + view.representation_channel;
        if (view.geometry.value_size_bytes == 0
            || sample >= view.storage_size_bytes / view.geometry.value_size_bytes) {
            return nullptr;
        }
        return reinterpret_cast<decltype(bytes.data())>(base)
            + sample * view.geometry.value_size_bytes;
    };

    for (auto const& transfer : plan.sample_transfers) {
        auto const target_root_index = sequential_cursor(
            std::span<std::byte>{current_storage}, transfer.target_storage);
        auto const source_root_index =
            transfer.source_access
                    == SampleRealtimePortStateAccess::immutable_constant
                || transfer.source_storage.empty()
            ? std::uint64_t{0}
            : sequential_cursor(
                std::span<std::byte const>{previous_storage},
                transfer.source_storage.front());
        for (auto relative = transfer.target_begin;
             relative < transfer.target_end; ++relative) {
            auto* target = sample_at(
                std::span<std::byte>{current_storage},
                transfer.target_storage,
                target_root_index,
                relative);
            IV_ASSERT(
                target != nullptr,
                "prepared sample transition target is out of bounds");
            std::memcpy(
                target,
                transfer.fresh_value.data(),
                transfer.fresh_value.size());
        }
        for (auto relative = transfer.inherited_begin;
             relative < transfer.inherited_end; ++relative) {
            auto* target = sample_at(
                std::span<std::byte>{current_storage},
                transfer.target_storage,
                target_root_index,
                relative);
            IV_ASSERT(
                target != nullptr,
                "prepared sample transition target is out of bounds");
            auto const& source = transfer.source_storage.front();
            if (transfer.source_access
                == SampleRealtimePortStateAccess::immutable_constant) {
                std::memcpy(
                    target,
                    source.constant_value->data(),
                    source.geometry.value_size_bytes);
            } else if (transfer.source_access
                       == SampleRealtimePortStateAccess::direct) {
                auto const* address = sample_at(
                    std::span<std::byte const>{previous_storage},
                    source,
                    source_root_index,
                    relative);
                IV_ASSERT(
                    address != nullptr,
                    "prepared sample transition source is out of bounds");
                if (source.geometry == transfer.target_storage.geometry) {
                    std::memcpy(
                        target, address, source.geometry.value_size_bytes);
                } else {
                    auto const migrate = continuous_value_type_info(
                        transfer.target_storage.geometry.value_type)
                                             .migrate_value_shape;
                    IV_ASSERT(
                        migrate != nullptr
                            && migrate(
                                target,
                                transfer.target_storage.geometry
                                    .elements_per_value,
                                address,
                                source.geometry.elements_per_value),
                        "prepared sample shape migration failed");
                }
            } else if (transfer.source_access
                       == SampleRealtimePortStateAccess::materialized) {
                IV_ASSERT(
                    transfer.source_materialization,
                    "prepared sample transition has an invalid materialized source");
                auto const& materialization =
                    *transfer.source_materialization;
                for (std::size_t lane = 0;
                     lane < source.geometry.channel_scalar_lane_count;
                     ++lane) {
                    float converted = 0.0f;
                    for (std::size_t source_channel = 0;
                         source_channel < transfer.source_storage.size();
                         ++source_channel) {
                        auto const& source_view =
                            transfer.source_storage[source_channel];
                        auto const* address = sample_at(
                            std::span<std::byte const>{previous_storage},
                            source_view,
                            source_root_index,
                            relative);
                        IV_ASSERT(
                            address != nullptr,
                            "prepared materialized sample source is out of bounds");
                        float value = 0.0f;
                        std::memcpy(
                            &value,
                            address + lane * sizeof(float),
                            sizeof(value));
                        converted += value * channel_conversion_coefficient(
                            materialization.source_layout.channel_type,
                            materialization.target_layout.channel_type,
                            source_channel,
                            transfer.source_materialization_channel);
                    }
                    std::memcpy(
                        target + lane * sizeof(float),
                        &converted,
                        sizeof(converted));
                }
            } else {
                IV_ASSERT(
                    transfer.source_access
                            == SampleRealtimePortStateAccess::composed
                        && transfer.source_composition_coefficients.size()
                            == transfer.source_storage.size(),
                    "prepared sample transition has an invalid composed source");
                for (std::size_t lane = 0;
                     lane < source.geometry.channel_scalar_lane_count;
                     ++lane) {
                    float composed = 0.0f;
                    for (std::size_t source_index = 0;
                         source_index < transfer.source_storage.size();
                         ++source_index) {
                        auto const& source_view =
                            transfer.source_storage[source_index];
                        auto const* address = sample_at(
                            std::span<std::byte const>{previous_storage},
                            source_view,
                            source_root_index,
                            relative);
                        IV_ASSERT(
                            address != nullptr,
                            "prepared composed sample source is out of bounds");
                        float value = 0.0f;
                        std::memcpy(
                            &value,
                            address + lane * sizeof(float),
                            sizeof(value));
                        composed += value
                            * transfer.source_composition_coefficients[
                                source_index];
                    }
                    std::memcpy(
                        target + lane * sizeof(float),
                        &composed,
                        sizeof(composed));
                }
            }
        }
    }

    for (std::size_t transfer_index = 0;
         transfer_index < plan.event_transfers.size(); ++transfer_index) {
        auto const& transfer = plan.event_transfers[transfer_index];
        auto const& source = transfer.source_storage;
        auto const& target = transfer.target_storage;
        auto const* source_base = storage_base(previous_storage, source);
        auto* target_base = storage_base(current_storage, target);
        IV_ASSERT(
            source_base != nullptr && target_base != nullptr,
            "prepared event transition ring is out of bounds");
        auto read_size = [](auto const* base, std::size_t offset) {
            std::size_t value = 0;
            std::memcpy(&value, base + offset, sizeof(value));
            return value;
        };
        auto const source_is_ring =
            source.storage == EventRealtimePortStateStorage::ring;
        auto const read = source_is_ring
            ? read_size(source_base, source.read_index_relative_offset)
            : std::size_t{0};
        auto const write = source_is_ring
            ? read_size(source_base, source.write_index_relative_offset)
            : read_size(source_base, source.count_relative_offset);
        IV_ASSERT(
            write >= read && write - read <= source.event_capacity,
            "prepared event transition source has invalid cursors");
        std::uint8_t timeline_initialized = 0;
        std::memcpy(
            &timeline_initialized,
            source_base + source.timeline_initialized_relative_offset,
            sizeof(timeline_initialized));
        GlobalIndex expected_position{};
        std::memcpy(
            &expected_position,
            source_base + source.expected_next_time_relative_offset,
            sizeof(expected_position));
        auto const seam_displacement = timeline_initialized != 0
            ? expected_position.forward_distance_to(sample_index)
            : GlobalIndex::extent_type{0};
        auto const target_is_ring =
            target.storage == EventRealtimePortStateStorage::ring;
        auto const continues_shared_target = std::ranges::any_of(
            plan.event_transfers.begin(),
            plan.event_transfers.begin()
                + static_cast<std::ptrdiff_t>(transfer_index),
            [&](auto const& previous) {
                return previous.target_storage.node_storage_offset
                    == target.node_storage_offset;
            });
        std::size_t target_write = continues_shared_target
            ? read_size(
                target_base,
                target_is_ring
                    ? target.write_index_relative_offset
                    : target.count_relative_offset)
            : std::size_t{0};
        IV_ASSERT(
            target_write <= target.event_capacity,
            "prepared event transition target has an invalid count");
        auto const inherited_extent =
            static_cast<std::uint64_t>(transfer.inherited_end)
            - static_cast<std::uint64_t>(transfer.inherited_begin);
        auto const inherited = GlobalIndexRange{
            .begin = relative_sample(sample_index, transfer.inherited_begin),
            .extent = inherited_extent,
        };
        auto append = [&](TimedEvent const& event) noexcept {
            if (!inherited.contains(event.time)) return;
            if (target_write >= target.event_capacity) return;
            auto const target_slot = [&](std::size_t cursor) {
                return target_is_ring
                    ? cursor % target.event_capacity
                    : cursor;
            };
            auto const order = RangeRelativeIndexOrder{relative_sample(
                sample_index, transfer.target_order_begin)};
            auto position = target_write;
            while (position != 0) {
                TimedEvent preceding{};
                std::size_t preceding_source_index = 0;
                std::memcpy(
                    &preceding,
                    target_base + target.events_relative_offset
                        + target_slot(position - 1) * sizeof(TimedEvent),
                    sizeof(preceding));
                if (target.has_source_indices) {
                    std::memcpy(
                        &preceding_source_index,
                        target_base + target.source_indices_relative_offset
                            + target_slot(position - 1)
                                * sizeof(std::size_t),
                        sizeof(preceding_source_index));
                }
                auto const precedes = order(event.time, preceding.time)
                    || (target.has_source_indices
                        && event.time == preceding.time
                        && *transfer.target_index
                            < preceding_source_index);
                if (!precedes) break;
                std::memcpy(
                    target_base + target.events_relative_offset
                        + target_slot(position) * sizeof(TimedEvent),
                    &preceding,
                    sizeof(preceding));
                if (target.has_source_indices) {
                    std::memcpy(
                        target_base + target.source_indices_relative_offset
                            + target_slot(position) * sizeof(std::size_t),
                        &preceding_source_index,
                        sizeof(preceding_source_index));
                }
                --position;
            }
            std::memcpy(
                target_base + target.events_relative_offset
                    + target_slot(position) * sizeof(TimedEvent),
                &event,
                sizeof(event));
            if (target.has_source_indices) {
                std::memcpy(
                    target_base + target.source_indices_relative_offset
                        + target_slot(position) * sizeof(std::size_t),
                    &*transfer.target_index,
                    sizeof(std::size_t));
            }
            ++target_write;
        };
        for (auto cursor = read; cursor < write; ++cursor) {
            if (transfer.source_access
                == EventRealtimePortStateAccess::indexed_merged_source) {
                std::size_t source_index = 0;
                std::memcpy(
                    &source_index,
                    source_base + source.source_indices_relative_offset
                        + (source_is_ring
                                ? cursor % source.event_capacity
                                : cursor)
                            * sizeof(std::size_t),
                    sizeof(source_index));
                if (source_index != *transfer.source_index) continue;
            }
            TimedEvent event{};
            std::memcpy(
                &event,
                source_base + source.events_relative_offset
                    + (source_is_ring
                            ? cursor % source.event_capacity
                            : cursor)
                        * sizeof(TimedEvent),
                sizeof(event));
            event.time = event.time.advanced_by(seam_displacement);
            if (transfer.source_conversion) {
                EventConversionRegistry::instance().convert(
                    *transfer.source_conversion,
                    event,
                    append);
            } else {
                append(event);
            }
        }
        if (target_is_ring) {
            std::size_t const zero = 0;
            std::memcpy(
                target_base + target.read_index_relative_offset,
                &zero,
                sizeof(zero));
            std::memcpy(
                target_base + target.write_index_relative_offset,
                &target_write,
                sizeof(target_write));
        } else {
            std::memcpy(
                target_base + target.count_relative_offset,
                &target_write,
                sizeof(target_write));
        }
        std::uint8_t const initialized = 1;
        std::memcpy(
            target_base + target.timeline_initialized_relative_offset,
            &initialized,
            sizeof(initialized));
        std::memcpy(
            target_base + target.expected_next_time_relative_offset,
            &sample_index,
            sizeof(sample_index));
    }
}

} // namespace iv::graph_jit
