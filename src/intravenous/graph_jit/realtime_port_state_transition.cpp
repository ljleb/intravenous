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

void initialize_zero_raw_region(
    std::span<std::byte> storage,
    std::span<std::byte const>)
{
    std::ranges::fill(storage, std::byte{});
}

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
    if (source.storage
        == SampleRealtimePortStateStorage::invocation_playback) {
        return source.invocation_slot.has_value()
            && !source.node_storage_offset
            && !source.callback_arena_offset;
    }
    return source.node_storage_offset.has_value()
        && !source.callback_arena_offset;
}

bool event_storage_is_invocation_playback(
    EventRealtimePortStateStorageView const& storage) noexcept
{
    return storage.storage == EventRealtimePortStateStorage::invocation_playback
        && storage.invocation_slot.has_value()
        && !storage.node_storage_offset
        && !storage.callback_arena_offset;
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

std::expected<InheritedRealtimePortStateRealization, std::string>
select_sample_inherited_state_realization(
    SampleRealtimePortStateRealization const& previous,
    SampleRealtimePortStateRealization const& current)
{
    // Background playback re-reads an invocation-supplied window from the
    // published data on every invocation, so an input supplied that way on
    // both sides of the splice inherits nothing. A delivery-kind change
    // preserves the history the consumer observed: a background-fed
    // predecessor's history is its final playback window. A background-fed
    // successor must present inherited realtime history through a private
    // transition ring until that history expires.
    if (current.retained_access
        == SampleRealtimePortStateAccess::invocation_supplied) {
        if (previous.retained_access
            == SampleRealtimePortStateAccess::invocation_supplied) {
            return InheritedRealtimePortStateRealization::already_represented;
        }
        return InheritedRealtimePortStateRealization::transition_only_storage;
    }
    auto recoverable_realtime_derivation = [](
        SampleRealtimePortStateRealization const& realization,
        SampleRealtimePortStateAccess access) {
        switch (access) {
        case SampleRealtimePortStateAccess::immutable_constant:
            return realization.authored_storage.constant_value.has_value()
                || (realization.retained_sources.size() == 1
                    && realization.retained_sources.front().constant_value
                        .has_value());
        case SampleRealtimePortStateAccess::direct:
            return realization.retained_sources.size() == 1
                && sample_storage_source_is_recoverable(
                    realization.retained_sources.front());
        case SampleRealtimePortStateAccess::materialized:
            return realization.materialization.has_value()
                && !realization.retained_sources.empty()
                && std::ranges::all_of(
                    realization.retained_sources,
                    sample_storage_source_is_recoverable);
        case SampleRealtimePortStateAccess::composed:
            return !realization.composition_sources.empty()
                && std::ranges::all_of(
                    realization.composition_sources,
                    [](auto const& source) {
                        return source.coefficient != 0.0f
                            && sample_storage_source_is_recoverable(
                                source.storage);
                    });
        default:
            return false;
        }
    };
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
    case SampleRealtimePortStateAccess::invocation_supplied:
        predecessor_recoverable = previous.retained_sources.size() == 1
            && previous.retained_sources.front().storage
                == SampleRealtimePortStateStorage::invocation_playback
            && sample_storage_source_is_recoverable(
                previous.retained_sources.front());
        break;
    case SampleRealtimePortStateAccess::combined_invocation_supplied:
        predecessor_recoverable = previous.invocation_combination.has_value()
            && previous.invocation_combination->combine != nullptr
            && previous.invocation_combination->invocation_storage.storage
                == SampleRealtimePortStateStorage::invocation_playback
            && sample_storage_source_is_recoverable(
                previous.invocation_combination->invocation_storage)
            && recoverable_realtime_derivation(
                previous,
                previous.invocation_combination->realtime_access);
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
                == SampleRealtimePortStateAccess::combined_invocation_supplied
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

    if (current.retained_access
        == SampleRealtimePortStateAccess::combined_invocation_supplied) {
        if (!current.invocation_combination) {
            return std::unexpected(
                "successor mixed sample state lost its invocation combination");
        }
        return InheritedRealtimePortStateRealization::transition_only_storage;
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
            || previous.retained_access
                == SampleRealtimePortStateAccess::invocation_supplied
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
    return InheritedRealtimePortStateRealization::transition_only_storage;
}

std::expected<InheritedRealtimePortStateRealization, std::string>
select_event_inherited_state_realization(
    EventRealtimePortStateRealization const& previous,
    EventRealtimePortStateRealization const& current)
{
    // See select_sample_inherited_state_realization().
    if (current.retained_access
        == EventRealtimePortStateAccess::invocation_supplied) {
        if (previous.retained_access
            == EventRealtimePortStateAccess::invocation_supplied) {
            return InheritedRealtimePortStateRealization::already_represented;
        }
        return InheritedRealtimePortStateRealization::transition_only_storage;
    }
    auto recoverable_realtime_derivation = [](
        EventRealtimePortStateRealization const& realization,
        EventRealtimePortStateAccess access) {
        switch (access) {
        case EventRealtimePortStateAccess::immutable_empty:
            return true;
        case EventRealtimePortStateAccess::direct:
            return realization.retained_storage.has_value()
                && event_storage_is_persistent_sequence(
                    *realization.retained_storage);
        case EventRealtimePortStateAccess::materialized:
            return realization.retained_storage.has_value()
                && event_storage_is_persistent_sequence(
                    *realization.retained_storage)
                && realization.materialization_conversion.has_value();
        case EventRealtimePortStateAccess::indexed_merged_source:
            return realization.retained_storage.has_value()
                && event_storage_is_persistent_sequence(
                    *realization.retained_storage)
                && realization.retained_storage->has_source_indices
                && realization.retained_source_index.has_value();
        default:
            return false;
        }
    };
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
    case EventRealtimePortStateAccess::invocation_supplied:
        predecessor_recoverable = previous.retained_storage.has_value()
            && event_storage_is_invocation_playback(
                *previous.retained_storage);
        break;
    case EventRealtimePortStateAccess::combined_invocation_supplied:
        predecessor_recoverable = previous.invocation_combination.has_value()
            && event_storage_is_invocation_playback(
                previous.invocation_combination->invocation_storage)
            && recoverable_realtime_derivation(
                previous,
                previous.invocation_combination->realtime_access);
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
    if (current.retained_access
        == EventRealtimePortStateAccess::combined_invocation_supplied) {
        if (!current.invocation_combination) {
            return std::unexpected(
                "successor mixed event state lost its invocation combination");
        }
        return InheritedRealtimePortStateRealization::transition_only_storage;
    }
    if (previous.retained_access
        == EventRealtimePortStateAccess::combined_invocation_supplied) {
        return InheritedRealtimePortStateRealization::transition_only_storage;
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
        auto const previous_type = previous.retained_access
                == EventRealtimePortStateAccess::combined_invocation_supplied
            ? previous.authored_storage.type
            : previous.retained_storage->type;
        if (previous_type != current.retained_storage->type) {
            try {
                (void)EventConversionRegistry::plan(
                    previous_type,
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
        if (requirement.extent_samples == 0) {
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
    if (current_requirement.role
        == RealtimePortStateRole::tick_output_latency) {
        end = static_cast<std::int64_t>(inherited);
    } else {
        begin = -static_cast<std::int64_t>(inherited);
    }
    return RealtimePortStateTransition{
        .previous_requirement_index = previous.requirement,
        .current_requirement_index = current.requirement,
        .previous_realization_index = previous.realization,
        .current_realization_index = current.realization,
        .inherited_begin = begin,
        .inherited_end = end,
        .inherited_extent_samples = inherited,
        .newly_exposed_extent_samples =
            current_requirement.extent_samples - inherited,
        .discarded_extent_samples =
            previous_requirement.extent_samples - inherited,
    };
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
        std::size_t semantic_users = 0;
        std::vector<std::size_t> transitions{};
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
        ++group.semantic_users;
    }
    for (std::size_t index = 0; index < plan.sample_states.size(); ++index) {
        auto const& transition = plan.sample_states[index];
        auto const& realization = current_realizations.sample_states[
            transition.current_realization_index];
        if (realization.retained_access
                != SampleRealtimePortStateAccess::direct
            || realization.retained_sources.size() != 1
            || !realization.retained_sources.front().node_storage_offset) {
            continue;
        }
        auto const& target = realization.retained_sources.front();
        sample_targets[SampleTargetKey{
            .storage_offset = *target.node_storage_offset,
            .channel = target.representation_channel,
        }].transitions.push_back(index);
    }
    for (auto const& [_, group] : sample_targets) {
        if (group.semantic_users < 2) continue;
        auto const transfers_inherited_state = std::ranges::any_of(
            group.transitions,
            [&](std::size_t index) {
                return plan.sample_states[index].inherited_state_realization
                    == InheritedRealtimePortStateRealization::
                        transfer_to_steady_storage;
            });
        if (!transfers_inherited_state) continue;
        for (auto const index : group.transitions) {
            auto& transition = plan.sample_states[index];
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
        if (target.retained_access != SampleRealtimePortStateAccess::direct
            || target.retained_sources.size() != 1
            || !target.retained_sources.front().node_storage_offset
            || target.retained_sources.front().callback_arena_offset) {
            return std::unexpected(
                "sample state transfer lost its successor steady-storage view");
        }

        auto const source_access = source.invocation_combination
            ? source.invocation_combination->realtime_access
            : source.retained_access;
        SampleRealtimePortStateTransfer transfer{
            .transition_index = index,
            .source_access = source_access,
            .source_materialization = source.materialization,
            .source_materialization_channel =
                source.authored_storage.representation_channel,
            .source_invocation_combination = source.invocation_combination,
            .target_storage = target.retained_sources.front(),
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
        };
        if (source_access
            == SampleRealtimePortStateAccess::immutable_constant) {
            auto const constant = std::ranges::find_if(
                source.retained_sources,
                [](auto const& candidate) {
                    return candidate.constant_value.has_value();
                });
            auto const* view = source.authored_storage.constant_value
                ? &source.authored_storage
                : constant == source.retained_sources.end()
                    ? nullptr
                    : &*constant;
            if (!view) {
                return std::unexpected(
                    "sample state transfer lost its predecessor constant");
            }
            transfer.source_storage.push_back(*view);
        } else if (source_access
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
        auto const source_access = source.invocation_combination
            ? source.invocation_combination->realtime_access
            : source.retained_access;
        auto const ordinary_is_empty = source_access
            == EventRealtimePortStateAccess::immutable_empty;
        auto const source_storage = ordinary_is_empty
            ? source.invocation_combination
                ? std::optional{source.invocation_combination
                                    ->invocation_storage}
                : std::nullopt
            : source.retained_storage;
        if (!source_storage
            || (!event_storage_is_invocation_playback(*source_storage)
                && (!source_storage->node_storage_offset
                    || source_storage->callback_arena_offset))) {
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
        if (source_storage->type
            != target.retained_storage->type) {
            try {
                conversion = EventConversionRegistry::plan(
                    source_storage->type,
                    target.retained_storage->type);
            } catch (std::exception const&) {
                return std::unexpected(
                    "event state transfer lost its type conversion");
            }
        }
        std::optional<EventConversionPlan> invocation_conversion;
        if (source.invocation_combination
            && !ordinary_is_empty
            && source.invocation_combination->invocation_storage.type
                != target.retained_storage->type) {
            try {
                invocation_conversion = EventConversionRegistry::plan(
                    source.invocation_combination->invocation_storage.type,
                    target.retained_storage->type);
            } catch (std::exception const&) {
                return std::unexpected(
                    "event state transfer lost its invocation type conversion");
            }
        }
        plan.event_transfers.push_back(EventRealtimePortStateTransfer{
            .transition_index = index,
            .source_access = ordinary_is_empty
                ? EventRealtimePortStateAccess::invocation_supplied
                : source_access,
            .source_storage = *source_storage,
            .source_index = source.retained_source_index,
            .source_conversion = std::move(conversion),
            .source_history_samples =
                source.materialization_history_samples,
            .source_selects_invocation_window =
                source.materialization_selects_invocation_window,
            .source_invocation_combination = ordinary_is_empty
                ? std::nullopt
                : source.invocation_combination,
            .source_invocation_conversion =
                std::move(invocation_conversion),
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
    if (view.storage == SampleRealtimePortStateStorage::invocation_playback) {
        if (!view.invocation_slot || view.node_storage_offset
            || view.callback_arena_offset || view.constant_value
            || view.geometry.value_size_bytes == 0
            || view.timeline_offset_frames != 0
            || view.representation_channel
                >= channel_count(view.channel_layout)) {
            return std::unexpected(
                std::string{role} + " is not a valid invocation playback window");
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
            std::string{role} + " cannot represent its semantic interval");
    }
    return {};
}

std::expected<void, std::string> validate_event_storage_view(
    EventRealtimePortStateStorageView const& view,
    std::size_t storage_size,
    bool allow_compact_carry,
    std::string_view role)
{
    if (view.storage == EventRealtimePortStateStorage::invocation_playback) {
        if (!event_storage_is_invocation_playback(view)) {
            return std::unexpected(
                std::string{role} + " is not a valid invocation playback window");
        }
        return {};
    }
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

// An invocation_playback source reads the predecessor slot window
// [end - history, end), which every invocation repopulates.
std::expected<void, std::string> validate_sample_playback_source(
    SampleRealtimePortStateTransfer const& transfer,
    RealtimePortStatePlaybackSlots const& playback)
{
    auto const& source = transfer.source_storage.front();
    if (*source.invocation_slot >= playback.sequential_samples.size()) {
        return std::unexpected(
            "sample transition playback source names a missing slot");
    }
    auto const& binding =
        playback.sequential_samples[*source.invocation_slot];
    if (binding.storage.channel_layout != source.channel_layout
        || binding.storage.geometry != source.geometry
        || source.representation_channel
            >= binding.storage.channels.size()) {
        return std::unexpected(
            "sample transition playback source disagrees with its slot geometry");
    }
    auto const& channel =
        binding.storage.channels[source.representation_channel];
    if (!channel.storage || channel.value_capacity == 0
        || !std::has_single_bit(channel.value_capacity)
        || channel.value_stride_bytes < source.geometry.value_size_bytes) {
        return std::unexpected(
            "sample transition playback slot is not a playback ring");
    }
    if (transfer.inherited_end > 0
        || binding.history > channel.value_capacity
        || !std::in_range<std::int64_t>(binding.history)
        || transfer.inherited_begin
            < -static_cast<std::int64_t>(binding.history)) {
        return std::unexpected(
            "sample transition playback window does not hold the inherited history");
    }
    return {};
}

std::expected<void, std::string> validate_event_playback_source(
    EventRealtimePortStateTransfer const& transfer,
    RealtimePortStatePlaybackSlots const& playback)
{
    auto const& source = transfer.source_storage;
    if (*source.invocation_slot >= playback.sequential_events.size()) {
        return std::unexpected(
            "event transition playback source names a missing slot");
    }
    auto const& binding = playback.sequential_events[*source.invocation_slot];
    if (!binding.storage.storage || binding.storage.persistent_ring
        || binding.storage.type != source.type
        || binding.storage.event_capacity == 0) {
        return std::unexpected(
            "event transition playback source disagrees with its slot");
    }
    if (transfer.inherited_end > 0) {
        return std::unexpected(
            "event transition playback window holds no authored future");
    }
    return {};
}

std::expected<void, std::string> validate_prepared_transitions(
    RealtimePortStateTransitionPlan const& plan,
    std::size_t previous_storage_size,
    std::size_t current_storage_size,
    RealtimePortStatePlaybackSlots const& playback)
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
        auto const playback_source = transfer.source_access
            == SampleRealtimePortStateAccess::invocation_supplied;
        auto const direct_source = transfer.source_access
                == SampleRealtimePortStateAccess::direct
            || transfer.source_access
                == SampleRealtimePortStateAccess::immutable_constant
            || playback_source;
        auto const materialized_source = transfer.source_access
            == SampleRealtimePortStateAccess::materialized;
        auto const composed_source = transfer.source_access
            == SampleRealtimePortStateAccess::composed;
        if ((!direct_source && !materialized_source && !composed_source)
            || (direct_source && transfer.source_storage.size() != 1)
            || ((materialized_source || composed_source)
                && transfer.source_storage.empty())
            || transfer.target_begin > transfer.inherited_begin
            || transfer.inherited_begin > transfer.inherited_end
            || transfer.inherited_end > transfer.target_end) {
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
        if (std::ranges::any_of(
                transfer.source_storage,
                [&](auto const& source) {
                    return (source.storage
                            == SampleRealtimePortStateStorage::
                                invocation_playback)
                        != playback_source;
                })
            || transfer.target_storage.storage
                == SampleRealtimePortStateStorage::invocation_playback) {
            return std::unexpected(
                "sample transfer disagrees with its invocation playback source");
        }
        if (playback_source) {
            auto validated =
                validate_sample_playback_source(transfer, playback);
            if (!validated) return validated;
        }
        if (transfer.source_invocation_combination) {
            auto const& combination = *transfer.source_invocation_combination;
            if (combination.realtime_access != transfer.source_access
                || !combination.combine
                || combination.invocation_storage.storage
                    != SampleRealtimePortStateStorage::invocation_playback
                || combination.invocation_storage.geometry
                    != transfer.target_storage.geometry) {
                return std::unexpected(
                    "sample transition has an invalid invocation combination");
            }
            auto invocation_transfer = transfer;
            invocation_transfer.source_access =
                SampleRealtimePortStateAccess::invocation_supplied;
            invocation_transfer.source_storage = {
                combination.invocation_storage};
            invocation_transfer.source_invocation_combination.reset();
            auto invocation_validated = validate_sample_storage_view(
                combination.invocation_storage,
                previous_storage_size,
                false,
                "sample transition invocation source");
            if (!invocation_validated) return invocation_validated;
            invocation_validated = validate_sample_playback_source(
                invocation_transfer, playback);
            if (!invocation_validated) return invocation_validated;
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
        auto const direct_shape_migration = (transfer.source_access
                    == SampleRealtimePortStateAccess::direct
                || playback_source)
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
        auto const playback_source = transfer.source_access
            == EventRealtimePortStateAccess::invocation_supplied;
        if ((transfer.source_access != EventRealtimePortStateAccess::direct
             && transfer.source_access
                != EventRealtimePortStateAccess::materialized
             && transfer.source_access
                != EventRealtimePortStateAccess::indexed_merged_source
             && !playback_source)
            || (transfer.source_storage.storage
                    == EventRealtimePortStateStorage::invocation_playback)
                != playback_source
            || transfer.target_storage.storage
                == EventRealtimePortStateStorage::invocation_playback
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
        if (playback_source) {
            validated = validate_event_playback_source(transfer, playback);
            if (!validated) return validated;
        }
        if (transfer.source_invocation_combination) {
            auto const& combination = *transfer.source_invocation_combination;
            if (combination.realtime_access != transfer.source_access
                || !event_storage_is_invocation_playback(
                    combination.invocation_storage)) {
                return std::unexpected(
                    "event transition has an invalid invocation combination");
            }
            auto invocation_transfer = transfer;
            invocation_transfer.source_access =
                EventRealtimePortStateAccess::invocation_supplied;
            invocation_transfer.source_storage =
                combination.invocation_storage;
            invocation_transfer.source_invocation_combination.reset();
            invocation_transfer.source_invocation_conversion.reset();
            validated = validate_event_storage_view(
                combination.invocation_storage,
                previous_storage_size,
                true,
                "event transition invocation source");
            if (!validated) return validated;
            validated = validate_event_playback_source(
                invocation_transfer, playback);
            if (!validated) return validated;
        } else if (transfer.source_invocation_conversion) {
            return std::unexpected(
                "event transition has an invocation conversion without a source");
        }
        validated = validate_event_storage_view(
            transfer.target_storage,
            current_storage_size,
            true,
            "event transition target");
        if (!validated) return validated;
        if (transfer.target_storage.has_source_indices
            != (transfer.target_index.has_value()
                || transfer.source_invocation_combination.has_value())) {
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
        auto const invocation_converted =
            transfer.source_invocation_conversion.has_value();
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
        if (transfer.source_invocation_combination
            && ((!invocation_converted
                    && transfer.source_invocation_combination
                           ->invocation_storage.type
                        != transfer.target_storage.type)
                || (invocation_converted
                    && (transfer.source_invocation_conversion->source_type
                            != transfer.source_invocation_combination
                                   ->invocation_storage.type
                        || transfer.source_invocation_conversion->target_type
                            != transfer.target_storage.type
                        || transfer.source_invocation_conversion->step_count
                            > EventConversionPlan::max_steps)))) {
            return std::unexpected(
                "event transition has an incompatible invocation conversion");
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
    std::size_t current_storage_size,
    RealtimePortStatePlaybackSlots const& playback)
    : plan_(std::move(plan))
    , previous_storage_size_(previous_storage_size)
    , current_storage_size_(current_storage_size)
    , playback_sample_slot_count_(playback.sequential_samples.size())
    , playback_event_slot_count_(playback.sequential_events.size())
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
            auto realization = select_sample_inherited_state_realization(
                previous_realizations.sample_states[
                    previous->second.realization],
                current_realizations.sample_states[current.realization]);
            if (!realization) {
                return std::unexpected(std::move(realization.error()));
            }
            transition->inherited_state_realization = *realization;
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
        suffix_cursor_targets;
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
    struct SuffixPaceCursor {
        std::int32_t absolute_pace_exponent = 0;
        NodeLayout::RegionHandle region{};
    };
    std::vector<SuffixPaceCursor> suffix_pace_cursors;
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
        auto const source_access = source.invocation_combination
            ? source.invocation_combination->realtime_access
            : source.retained_access;
        if ((source_access
                 != SampleRealtimePortStateAccess::direct
             && source_access
                 != SampleRealtimePortStateAccess::immutable_constant
            && source_access
                 != SampleRealtimePortStateAccess::materialized
             && source_access
                 != SampleRealtimePortStateAccess::composed
             && source_access
                 != SampleRealtimePortStateAccess::invocation_supplied)
            || ((source_access
                        == SampleRealtimePortStateAccess::direct
                    || source_access
                        == SampleRealtimePortStateAccess::invocation_supplied)
                && source.retained_sources.size() != 1)
            || (source_access
                    == SampleRealtimePortStateAccess::materialized
                && (!source.materialization
                    || source.retained_sources.empty()))
            || (source_access
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
                    != SampleRealtimePortStateAccess::composed
                && current.retained_access
                    != SampleRealtimePortStateAccess::invocation_supplied
                && current.retained_access
                    != SampleRealtimePortStateAccess::
                        combined_invocation_supplied)) {
            return std::unexpected(
                "transition suffix requires a recoverable predecessor and supported successor sample view");
        }

        auto source_views = source_access
                == SampleRealtimePortStateAccess::immutable_constant
            ? source.retained_sources.empty()
                ? std::vector<SampleRealtimePortStateStorageView>{
                    source.authored_storage}
                : std::vector<SampleRealtimePortStateStorageView>{
                    source.retained_sources.front()}
            : source.retained_sources;
        std::vector<float> source_composition_coefficients;
        if (source_access
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
        std::optional<std::size_t> steady_target_offset;
        auto target = current.authored_storage;
        if (current.retained_access
            == SampleRealtimePortStateAccess::direct) {
            auto const& steady_target = current.retained_sources.front();
            steady_target_offset = steady_target.node_storage_offset;
            target = steady_target;
        }
        auto const retained_timeline_offset = target.timeline_offset_frames;
        std::optional<NodeLayout::RegionHandle> suffix_cursor_region;
        if (current.retained_access
                == SampleRealtimePortStateAccess::invocation_supplied
            || current.retained_access
                == SampleRealtimePortStateAccess::
                    combined_invocation_supplied) {
            auto const channels = channel_count(target.channel_layout);
            if (target.working_frame_capacity == 0
                || !std::has_single_bit(target.working_frame_capacity)
                || channels == 0 || target.geometry.value_size_bytes == 0
                || target.storage_frame_count
                    != target.working_frame_capacity
                || channels > std::numeric_limits<std::size_t>::max()
                        / target.storage_frame_count
                || channels * target.storage_frame_count
                    > std::numeric_limits<std::size_t>::max()
                        / target.geometry.value_size_bytes
                || target.storage_size_bytes
                    != channels * target.storage_frame_count
                        * target.geometry.value_size_bytes) {
                return std::unexpected(
                    "transition suffix cannot realize an invocation-supplied sample input");
            }
            target.storage = SampleRealtimePortStateStorage::ring;
            target.node_storage_offset.reset();
            target.callback_arena_offset.reset();
            target.invocation_slot.reset();
            if (!target.sequential_cursor_node_storage_offset) {
                auto cursor = std::ranges::find(
                    suffix_pace_cursors,
                    target.absolute_pace_exponent,
                    &SuffixPaceCursor::absolute_pace_exponent);
                if (cursor == suffix_pace_cursors.end()) {
                    std::vector<std::byte> zero(sizeof(std::uint64_t));
                    auto const region = layout_builder.declare_raw_region(
                        sizeof(std::uint64_t),
                        alignof(std::uint64_t),
                        &initialize_zero_raw_region,
                        std::move(zero));
                    suffix_pace_cursors.push_back({
                        .absolute_pace_exponent =
                            target.absolute_pace_exponent,
                        .region = region,
                    });
                    cursor = std::prev(suffix_pace_cursors.end());
                }
                suffix_cursor_region = cursor->region;
            }
        }
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
                        || (source_access
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

        auto const target_begin = transition.inherited_begin < 0
            ? -static_cast<std::int64_t>(target_extent)
            : 0;
        auto const target_end = transition.inherited_begin < 0
            ? 0
            : static_cast<std::int64_t>(target_extent);

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
            .source_access = source_access,
            .source_storage = source_views,
            .source_materialization = source.materialization,
            .source_materialization_channel =
                source.authored_storage.representation_channel,
            .source_composition_coefficients =
                std::move(source_composition_coefficients),
            .source_invocation_combination =
                source.invocation_combination,
            .target_storage = std::move(target),
            .target_begin = target_begin,
            .target_end = target_end,
            .inherited_begin = transition.inherited_begin,
            .inherited_end = transition.inherited_end,
        });
        suffix_targets.emplace_back(
            plan.sample_transfers.size() - 1, group->region);
        if (suffix_cursor_region) {
            suffix_cursor_targets.emplace_back(
                plan.sample_transfers.size() - 1,
                *suffix_cursor_region);
        }
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
        auto const source_access = source.invocation_combination
            ? source.invocation_combination->realtime_access
            : source.retained_access;
        auto const ordinary_is_empty = source_access
            == EventRealtimePortStateAccess::immutable_empty;
        auto const source_storage = ordinary_is_empty
            ? source.invocation_combination
                ? std::optional{source.invocation_combination
                                    ->invocation_storage}
                : std::nullopt
            : source.retained_storage;
        auto const playback_source = ordinary_is_empty
            || (source_access
                == EventRealtimePortStateAccess::invocation_supplied
                && source_storage
                && event_storage_is_invocation_playback(*source_storage));
        if (!source_storage
            || (!playback_source
                && (!event_storage_is_persistent_sequence(
                        *source_storage)
                    || (source_access
                            != EventRealtimePortStateAccess::direct
                        && source_access
                            != EventRealtimePortStateAccess::materialized
                        && source_access
                            != EventRealtimePortStateAccess::
                                indexed_merged_source)))
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
        auto const events_end = *events_offset
            + capacity * sizeof(TimedEvent);
        auto const needs_source_indices = source.invocation_combination
            && !ordinary_is_empty;
        auto const source_indices_offset = needs_source_indices
            ? checked_align_up(events_end, alignof(std::size_t))
            : std::optional<std::size_t>{events_end};
        if (!source_indices_offset
            || (needs_source_indices
                && capacity > (std::numeric_limits<std::size_t>::max()
                        - *source_indices_offset) / sizeof(std::size_t))) {
            return std::unexpected(
                "transition suffix event source-index sidecar overflows size_t");
        }
        auto const size = needs_source_indices
            ? *source_indices_offset + capacity * sizeof(std::size_t)
            : events_end;
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
            .has_source_indices = needs_source_indices,
            .source_indices_relative_offset = *source_indices_offset,
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
        if (source_storage->type != target.type) {
            try {
                conversion = EventConversionRegistry::plan(
                    source_storage->type, target.type);
            } catch (std::exception const&) {
                return std::unexpected(
                    "transition suffix event type has no conversion");
            }
        }
        std::optional<EventConversionPlan> invocation_conversion;
        if (source.invocation_combination
            && !ordinary_is_empty
            && source.invocation_combination->invocation_storage.type
                != target.type) {
            try {
                invocation_conversion = EventConversionRegistry::plan(
                    source.invocation_combination->invocation_storage.type,
                    target.type);
            } catch (std::exception const&) {
                return std::unexpected(
                    "transition suffix invocation event type has no conversion");
            }
        }
        plan.event_transfers.push_back(EventRealtimePortStateTransfer{
            .transition_index = index,
            .target_transition_only = true,
            .source_access = playback_source
                ? EventRealtimePortStateAccess::invocation_supplied
                : source_access,
            .source_storage = *source_storage,
            .source_index = source.retained_source_index,
            .source_conversion = std::move(conversion),
            .source_history_samples = source.materialization_history_samples,
            .source_selects_invocation_window =
                source.materialization_selects_invocation_window,
            .source_invocation_combination = ordinary_is_empty
                ? std::nullopt
                : source.invocation_combination,
            .source_invocation_conversion =
                std::move(invocation_conversion),
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
    for (auto const& [transfer_index, region] : suffix_cursor_targets) {
        if (transfer_index >= plan.sample_transfers.size()
            || region.index >= node_layout.regions.size()
            || !node_layout.regions[region.index].transition_only) {
            return std::unexpected(
                "transition suffix sample cursor lost its layout region");
        }
        plan.sample_transfers[transfer_index].target_storage
            .sequential_cursor_node_storage_offset =
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
    std::span<std::byte> current_storage,
    RealtimePortStatePlaybackSlots const& playback)
{
    auto validated = validate_prepared_transitions(
        plan, previous_storage.size(), current_storage.size(), playback);
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
        std::move(plan),
        previous_storage.size(),
        current_storage.size(),
        playback};
}

void execute_realtime_port_state_transfers(
    PreparedRealtimePortStateTransition const& transition,
    std::span<std::byte const> previous_storage,
    std::span<std::byte> current_storage,
    GlobalIndex sample_index,
    RealtimePortStatePlaybackSlots const& playback,
    std::optional<GlobalIndex> playback_end) noexcept
{
    IV_ASSERT(
        previous_storage.size() == transition.previous_storage_size_,
        "prepared transition predecessor storage size changed");
    IV_ASSERT(
        current_storage.size() == transition.current_storage_size_,
        "prepared transition successor storage size changed");
    IV_ASSERT(
        playback.sequential_samples.size()
                == transition.playback_sample_slot_count_
            && playback.sequential_events.size()
                == transition.playback_event_slot_count_,
        "prepared transition predecessor playback slots changed");
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
    auto shifted_ordinal = [](std::uint64_t origin, std::int64_t relative) {
        return origin + static_cast<std::uint64_t>(relative);
    };
    auto sample_at = [&](auto bytes,
                         SampleRealtimePortStateStorageView const& view,
                         std::uint64_t root_ordinal,
                         std::int64_t relative) -> decltype(bytes.data()) {
        auto* base = storage_base(bytes, view);
        auto const physical = shifted_ordinal(
            shifted_ordinal(root_ordinal, relative),
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
                auto const first = root_ordinal - retained_before;
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
    // The external binding carries the current block's Sequential ordinal.
    // It is the playback counterpart of a generation-owned pace cursor and is
    // independent of the visible GlobalIndex used to select its data.
    auto playback_sample_at = [&](
                                  SampleRealtimePortStateStorageView const& view,
                                  std::int64_t relative) {
        auto const& binding =
            playback.sequential_samples[*view.invocation_slot];
        auto const& channel = binding.storage.channels[
            view.representation_channel];
        auto const ordinal = binding.sequential_begin_ordinal
            + static_cast<std::uint64_t>(relative)
            - binding.read_latency
            - channel.value_delay;
        auto const frame = static_cast<std::size_t>(
            ordinal & (channel.value_capacity - 1));
        return static_cast<std::byte const*>(channel.storage)
            + frame * channel.value_stride_bytes;
    };

    for (auto const& transfer : plan.sample_transfers) {
        auto const target_root_ordinal = sequential_cursor(
            std::span<std::byte>{current_storage}, transfer.target_storage);
        auto const playback_source = transfer.source_access
            == SampleRealtimePortStateAccess::invocation_supplied;
        auto const combined_playback =
            transfer.source_invocation_combination.has_value();
        auto const source_root_ordinal =
            transfer.source_access
                    == SampleRealtimePortStateAccess::immutable_constant
                || playback_source
            ? std::uint64_t{0}
            : sequential_cursor(
                std::span<std::byte const>{previous_storage},
                transfer.source_storage.front());
        for (auto relative = transfer.target_begin;
             relative < transfer.target_end; ++relative) {
            auto* target = sample_at(
                std::span<std::byte>{current_storage},
                transfer.target_storage,
                target_root_ordinal,
                relative);
            IV_ASSERT(
                target != nullptr,
                "prepared sample transition target is out of bounds");
            std::memset(
                target,
                0,
                transfer.target_storage.geometry.value_size_bytes);
        }
        // A predecessor that never invoked observed no history.
        if ((playback_source || combined_playback) && !playback_end) continue;
        for (auto relative = transfer.inherited_begin;
             relative < transfer.inherited_end; ++relative) {
            auto* target = sample_at(
                std::span<std::byte>{current_storage},
                transfer.target_storage,
                target_root_ordinal,
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
                           == SampleRealtimePortStateAccess::direct
                       || playback_source) {
                auto const* address = playback_source
                    ? playback_sample_at(source, relative)
                    : sample_at(
                        std::span<std::byte const>{previous_storage},
                        source,
                        source_root_ordinal,
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
                            source_root_ordinal,
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
                            source_root_ordinal,
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
            if (combined_playback) {
                auto const& combination =
                    *transfer.source_invocation_combination;
                auto const* invocation = playback_sample_at(
                    combination.invocation_storage, relative);
                IV_ASSERT(
                    invocation != nullptr
                        && combination.combine(
                            combination.continuous_input,
                            transfer.target_storage.geometry.value_type,
                            target,
                            invocation,
                            transfer.target_storage.geometry.value_size_bytes,
                            transfer.target_storage.geometry
                                .elements_per_value),
                    "prepared sample invocation combination failed");
            }
        }
    }

    for (std::size_t transfer_index = 0;
         transfer_index < plan.event_transfers.size(); ++transfer_index) {
        auto const& transfer = plan.event_transfers[transfer_index];
        auto const& source = transfer.source_storage;
        auto const& target = transfer.target_storage;
        auto read_size = [](auto const* base, std::size_t offset) {
            std::size_t value = 0;
            std::memcpy(&value, base + offset, sizeof(value));
            return value;
        };
        auto* target_base = storage_base(current_storage, target);
        IV_ASSERT(
            target_base != nullptr,
            "prepared event transition target is out of bounds");
        std::byte const* source_base = nullptr;
        auto source_events_offset = source.events_relative_offset;
        auto source_capacity = source.event_capacity;
        auto const source_is_ring =
            source.storage == EventRealtimePortStateStorage::ring;
        std::size_t read = 0;
        std::size_t write = 0;
        auto seam_displacement = GlobalIndex::extent_type{0};
        if (source.storage
            == EventRealtimePortStateStorage::invocation_playback) {
            // The playback sequence holds exactly the final predecessor
            // invocation window, which ends at playback_end.
            auto const& binding =
                playback.sequential_events[*source.invocation_slot].storage;
            source_base = binding.storage;
            source_events_offset = binding.events_offset;
            source_capacity = binding.event_capacity;
            if (playback_end) {
                write = read_size(source_base, binding.count_offset);
                seam_displacement =
                    playback_end->forward_distance_to(sample_index);
            }
        } else {
            source_base = storage_base(previous_storage, source);
            IV_ASSERT(
                source_base != nullptr,
                "prepared event transition source is out of bounds");
            read = source_is_ring
                ? read_size(source_base, source.read_index_relative_offset)
                : std::size_t{0};
            write = source_is_ring
                ? read_size(source_base, source.write_index_relative_offset)
                : read_size(source_base, source.count_relative_offset);
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
            if (timeline_initialized != 0) {
                seam_displacement =
                    expected_position.forward_distance_to(sample_index);
            }
        }
        IV_ASSERT(
            write >= read && write - read <= source_capacity,
            "prepared event transition source has invalid cursors");
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
        auto append = [&](TimedEvent const& event,
                          std::size_t event_source_index) noexcept {
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
                        && event_source_index < preceding_source_index);
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
                    &event_source_index,
                    sizeof(std::size_t));
            }
            ++target_write;
        };
        for (auto cursor = read; cursor < write; ++cursor) {
            auto event_source_index = transfer.target_index.value_or(
                std::numeric_limits<std::size_t>::max());
            if (target.has_source_indices
                && transfer.source_invocation_combination) {
                if (source.has_source_indices) {
                    std::memcpy(
                        &event_source_index,
                        source_base + source.source_indices_relative_offset
                            + (source_is_ring
                                    ? cursor % source.event_capacity
                                    : cursor)
                                * sizeof(std::size_t),
                        sizeof(event_source_index));
                } else {
                    event_source_index = transfer
                        .source_invocation_combination
                        ->realtime_fixed_source_index.value_or(
                            std::numeric_limits<std::size_t>::max());
                }
            }
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
                source_base + source_events_offset
                    + (source_is_ring
                            ? cursor % source_capacity
                            : cursor)
                        * sizeof(TimedEvent),
                sizeof(event));
            event.time = event.time.advanced_by(seam_displacement);
            if (transfer.source_conversion) {
                EventConversionRegistry::instance().convert(
                    *transfer.source_conversion,
                    event,
                    [&](TimedEvent const& converted) noexcept {
                        append(converted, event_source_index);
                    });
            } else {
                append(event, event_source_index);
            }
        }
        if (transfer.source_invocation_combination && playback_end) {
            auto const& invocation =
                transfer.source_invocation_combination->invocation_storage;
            auto const& binding =
                playback.sequential_events[*invocation.invocation_slot];
            auto const* invocation_base = binding.storage.storage;
            auto const invocation_write = read_size(
                invocation_base, binding.storage.count_offset);
            auto const invocation_displacement =
                playback_end->forward_distance_to(sample_index);
            IV_ASSERT(
                invocation_write <= binding.storage.event_capacity,
                "prepared event invocation source has an invalid count");
            for (std::size_t cursor = 0;
                 cursor < invocation_write;
                 ++cursor) {
                TimedEvent event{};
                std::memcpy(
                    &event,
                    invocation_base + binding.storage.events_offset
                        + cursor * sizeof(TimedEvent),
                    sizeof(event));
                event.time = event.time.advanced_by(
                    invocation_displacement);
                auto const event_source_index =
                    binding.semantic_source_indices
                    ? binding.semantic_source_indices[cursor]
                    : std::numeric_limits<std::size_t>::max();
                if (transfer.source_invocation_conversion) {
                    EventConversionRegistry::instance().convert(
                        *transfer.source_invocation_conversion,
                        event,
                        [&](TimedEvent const& converted) noexcept {
                            append(converted, event_source_index);
                        });
                } else {
                    append(event, event_source_index);
                }
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
