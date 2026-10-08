#include <intravenous/graph_jit/realtime_port_state_transition.h>

#include <algorithm>
#include <bit>
#include <functional>
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
    case SampleRealtimePortStateAccess::callback_only:
        break;
    }
    if (!predecessor_recoverable) {
        return InheritedRealtimePortStateRealization::
            predecessor_state_unavailable;
    }

    if (current.retained_access
        == SampleRealtimePortStateAccess::immutable_constant) {
        if (!current.authored_storage.constant_value) {
            return std::unexpected(
                "successor immutable sample state has no constant value");
        }
        if (previous.retained_access
            == SampleRealtimePortStateAccess::immutable_constant) {
            auto const previous_bits = std::bit_cast<std::uint32_t>(
                previous.authored_storage.constant_value->value);
            auto const current_bits = std::bit_cast<std::uint32_t>(
                current.authored_storage.constant_value->value);
            if (previous_bits == current_bits) {
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
            && previous.retained_storage->node_storage_offset.has_value()
            && !previous.retained_storage->callback_arena_offset;
        break;
    case EventRealtimePortStateAccess::materialized:
        predecessor_recoverable = previous.retained_storage.has_value()
            && previous.retained_storage->node_storage_offset.has_value()
            && !previous.retained_storage->callback_arena_offset
            && previous.materialization_conversion.has_value();
        break;
    case EventRealtimePortStateAccess::indexed_merged_source:
        predecessor_recoverable = previous.retained_storage.has_value()
            && previous.retained_storage->node_storage_offset.has_value()
            && !previous.retained_storage->callback_arena_offset
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
            == EventRealtimePortStateAccess::immutable_empty
        && (current.retained_access
                == EventRealtimePortStateAccess::immutable_empty
            || current.retained_access
                == EventRealtimePortStateAccess::direct)) {
        return InheritedRealtimePortStateRealization::already_represented;
    }
    if (current.retained_access == EventRealtimePortStateAccess::direct) {
        if (!current.retained_storage
            || !current.retained_storage->node_storage_offset
            || current.retained_storage->callback_arena_offset) {
            return std::unexpected(
                "successor direct event state has no persistent view");
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
    if (inherited
        > static_cast<std::size_t>(
            std::numeric_limits<std::int64_t>::max())) {
        return std::unexpected(
            "realtime port-state inherited extent is not representable");
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
        && lhs.events_relative_offset == rhs.events_relative_offset
        && lhs.has_source_indices == rhs.has_source_indices
        && lhs.source_indices_relative_offset
            == rhs.source_indices_relative_offset;
}

std::expected<void, std::string> classify_shared_successor_storage(
    RealtimePortStateTransitionPlan& plan,
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
                    predecessor_state_unavailable) {
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
        if (realization.retained_access
                != EventRealtimePortStateAccess::direct
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
        if (realization.retained_access
                != EventRealtimePortStateAccess::direct
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
        for (auto const index : group.transitions) {
            auto& transition = plan.event_states[index];
            if (transition.inherited_state_realization
                != InheritedRealtimePortStateRealization::
                    predecessor_state_unavailable) {
                transition.inherited_state_realization =
                    InheritedRealtimePortStateRealization::
                        transition_only_storage;
            }
        }
    }
    return {};
}

} // namespace

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
        result, current_realizations);
    if (!classified) {
        return std::unexpected(std::move(classified.error()));
    }
    return result;
}

} // namespace iv::graph_jit
