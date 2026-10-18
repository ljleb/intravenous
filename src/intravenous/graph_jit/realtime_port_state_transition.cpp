#include <intravenous/graph_jit/realtime_port_state_transition.h>

#include <intravenous/compat.h>

#include <algorithm>
#include <bit>
#include <cstring>
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
            || std::ranges::all_of(
                previous.retained_sources,
                [](auto const& source) {
                    return source.storage
                            == SampleRealtimePortStateStorage::ring
                        || source.storage
                            == SampleRealtimePortStateStorage::compact_carry;
                });
        if ((target.storage != SampleRealtimePortStateStorage::ring
             && target.storage
                 != SampleRealtimePortStateStorage::compact_carry)
            || !sources_are_direct_storage
            || previous.retained_access
                == SampleRealtimePortStateAccess::materialized) {
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
        if (previous.authored_storage.type
            != current.retained_storage->type) {
            return InheritedRealtimePortStateRealization::
                transition_only_storage;
        }
        if (previous.retained_access != EventRealtimePortStateAccess::direct
            || previous.retained_storage->storage
                != EventRealtimePortStateStorage::ring
            || current.retained_storage->storage
                != EventRealtimePortStateStorage::ring
            || current.retained_storage->event_capacity
                < previous.retained_storage->event_capacity) {
            return InheritedRealtimePortStateRealization::
                transition_only_storage;
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

        SampleRealtimePortStateTransfer transfer{
            .transition_index = index,
            .source_access = source.retained_access,
            .source_materialization = source.materialization,
            .source_materialization_channel =
                source.authored_storage.representation_channel,
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
        if (source.retained_access
            == SampleRealtimePortStateAccess::immutable_constant) {
            if (!source.authored_storage.constant_value) {
                return std::unexpected(
                    "sample state transfer lost its predecessor constant");
            }
            transfer.source_storage.push_back(source.authored_storage);
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
        if (target.retained_access != EventRealtimePortStateAccess::direct
            || !target.retained_storage
            || !target.retained_storage->node_storage_offset
            || target.retained_storage->callback_arena_offset) {
            return std::unexpected(
                "event state transfer lost its successor steady-storage view");
        }
        plan.event_transfers.push_back(EventRealtimePortStateTransfer{
            .transition_index = index,
            .source_access = source.retained_access,
            .source_storage = *source.retained_storage,
            .source_index = source.retained_source_index,
            .source_conversion = source.materialization_conversion,
            .source_history_samples =
                source.materialization_history_samples,
            .source_selects_invocation_window =
                source.materialization_selects_invocation_window,
            .target_storage = *target.retained_storage,
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
    return {};
}

bool storage_extent_fits(
    std::size_t storage_size,
    std::size_t offset,
    std::size_t extent) noexcept
{
    return offset <= storage_size && extent <= storage_size - offset;
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
        || !view.node_storage_offset || view.callback_arena_offset
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
            view.storage_size_bytes)) {
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
    std::string_view role)
{
    if (view.storage != EventRealtimePortStateStorage::ring
        || !view.node_storage_offset || view.callback_arena_offset
        || view.event_capacity == 0
        || !std::has_single_bit(view.alignment)
        || !storage_extent_fits(
            storage_size, *view.node_storage_offset, view.size_bytes)) {
        return std::unexpected(
            std::string{role} + " is not valid persistent event-ring storage");
    }
    auto field_fits = [&](std::size_t offset, std::size_t size) {
        return storage_extent_fits(view.size_bytes, offset, size);
    };
    if (!field_fits(view.count_relative_offset, sizeof(std::size_t))
        || !field_fits(view.read_index_relative_offset, sizeof(std::size_t))
        || !field_fits(view.write_index_relative_offset, sizeof(std::size_t))
        || view.event_capacity > std::numeric_limits<std::size_t>::max()
                / sizeof(TimedEvent)
        || !field_fits(
            view.events_relative_offset,
            view.event_capacity * sizeof(TimedEvent))) {
        return std::unexpected(
            std::string{role} + " contains an out-of-bounds event-ring field");
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
        ++sample_transfer_counts[transfer.transition_index];
        if (transfer.source_storage.size() != 1
            || (transfer.source_access
                    != SampleRealtimePortStateAccess::direct
                && transfer.source_access
                    != SampleRealtimePortStateAccess::immutable_constant)
            || transfer.target_storage.absolute_pace_exponent != 0
            || transfer.target_begin > transfer.inherited_begin
            || transfer.inherited_begin > transfer.inherited_end
            || transfer.inherited_end > transfer.target_end) {
            return std::unexpected(
                "sample transfer is not an executable project-rate direct transfer");
        }
        auto const& source = transfer.source_storage.front();
        auto validated = validate_sample_storage_view(
            source,
            previous_storage_size,
            transfer.source_access
                == SampleRealtimePortStateAccess::immutable_constant,
            "sample transition source");
        if (!validated) return validated;
        validated = validate_sample_storage_view(
            transfer.target_storage,
            current_storage_size,
            false,
            "sample transition target");
        if (!validated) return validated;
        if (source.geometry != transfer.target_storage.geometry
            || source.absolute_pace_exponent != 0) {
            return std::unexpected(
                "sample transition source and target geometry differ");
        }
        if (transfer.source_access == SampleRealtimePortStateAccess::direct) {
            validated = validate_sample_interval(
                source,
                transfer.inherited_begin,
                transfer.inherited_end,
                "sample transition source");
            if (!validated) return validated;
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
            == InheritedRealtimePortStateRealization::transition_only_storage) {
            return std::unexpected(
                "sample state requires a transition-only storage suffix");
        }
        if (realization
            == InheritedRealtimePortStateRealization::
                predecessor_state_unavailable) {
            return std::unexpected(
                "sample state is unavailable from the active predecessor");
        }
        auto const expected = realization
                == InheritedRealtimePortStateRealization::
                    transfer_to_steady_storage
            ? std::size_t{1}
            : std::size_t{0};
        if (sample_transfer_counts[index] != expected) {
            return std::unexpected(
                "sample semantic transition has inconsistent transfers");
        }
    }

    std::vector<std::size_t> event_transfer_counts(plan.event_states.size());
    for (auto const& transfer : plan.event_transfers) {
        if (transfer.transition_index >= plan.event_states.size()) {
            return std::unexpected(
                "event transfer names a missing semantic transition");
        }
        ++event_transfer_counts[transfer.transition_index];
        if (transfer.source_access != EventRealtimePortStateAccess::direct
            || transfer.target_begin > transfer.inherited_begin
            || transfer.inherited_begin > transfer.inherited_end
            || transfer.inherited_end > transfer.target_end) {
            return std::unexpected(
                "event transfer is not an executable direct transfer");
        }
        auto validated = validate_event_storage_view(
            transfer.source_storage,
            previous_storage_size,
            "event transition source");
        if (!validated) return validated;
        validated = validate_event_storage_view(
            transfer.target_storage,
            current_storage_size,
            "event transition target");
        if (!validated) return validated;
        if (transfer.source_storage.type != transfer.target_storage.type
            || transfer.target_storage.event_capacity
                < transfer.source_storage.event_capacity) {
            return std::unexpected(
                "event transition target cannot contain its source ring");
        }
    }
    for (std::size_t index = 0; index < plan.event_states.size(); ++index) {
        auto const realization =
            plan.event_states[index].inherited_state_realization;
        if (realization
            == InheritedRealtimePortStateRealization::transition_only_storage) {
            return std::unexpected(
                "event state requires a transition-only storage suffix");
        }
        if (realization
            == InheritedRealtimePortStateRealization::
                predecessor_state_unavailable) {
            return std::unexpected(
                "event state is unavailable from the active predecessor");
        }
        auto const expected = realization
                == InheritedRealtimePortStateRealization::
                    transfer_to_steady_storage
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
    auto transfers = build_steady_storage_transfers(
        result, previous_realizations, current_realizations);
    if (!transfers) {
        return std::unexpected(std::move(transfers.error()));
    }
    return result;
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
    auto sample_at = [&](auto bytes,
                         SampleRealtimePortStateStorageView const& view,
                         GlobalIndex semantic) -> decltype(bytes.data()) {
        auto* base = storage_base(bytes, view);
        auto const physical = semantic.shifted_by(view.timeline_offset_frames);
        std::optional<std::size_t> frame;
        if (view.storage == SampleRealtimePortStateStorage::ring) {
            if (view.working_frame_capacity != 0) {
                frame = physical.bits() % view.working_frame_capacity;
            }
        } else if (view.storage
                   == SampleRealtimePortStateStorage::compact_carry) {
            if (view.storage_frame_count >= view.carry_future_frames) {
                auto const retained_before =
                    view.storage_frame_count - view.carry_future_frames;
                auto const first = sample_index.retreated_by(retained_before);
                auto const candidate = first.forward_distance_to(physical);
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
        for (auto relative = transfer.target_begin;
             relative < transfer.target_end; ++relative) {
            auto const semantic = relative_sample(sample_index, relative);
            auto* target = sample_at(
                std::span<std::byte>{current_storage},
                transfer.target_storage,
                semantic);
            IV_ASSERT(
                target != nullptr,
                "prepared sample transition target is out of bounds");
            std::memset(
                target,
                0,
                transfer.target_storage.geometry.value_size_bytes);
        }
        for (auto relative = transfer.inherited_begin;
             relative < transfer.inherited_end; ++relative) {
            auto const semantic = relative_sample(sample_index, relative);
            auto* target = sample_at(
                std::span<std::byte>{current_storage},
                transfer.target_storage,
                semantic);
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
            } else {
                auto const* address = sample_at(
                    std::span<std::byte const>{previous_storage},
                    source,
                    semantic);
                IV_ASSERT(
                    address != nullptr,
                    "prepared sample transition source is out of bounds");
                std::memcpy(
                    target, address, source.geometry.value_size_bytes);
            }
        }
    }

    for (auto const& transfer : plan.event_transfers) {
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
        auto const read = read_size(source_base, source.read_index_relative_offset);
        auto const write = read_size(source_base, source.write_index_relative_offset);
        IV_ASSERT(
            write >= read && write - read <= source.event_capacity,
            "prepared event transition source ring has invalid cursors");
        std::size_t target_write = 0;
        auto const inherited_extent =
            static_cast<std::uint64_t>(transfer.inherited_end)
            - static_cast<std::uint64_t>(transfer.inherited_begin);
        auto const inherited = GlobalIndexRange{
            .begin = relative_sample(sample_index, transfer.inherited_begin),
            .extent = inherited_extent,
        };
        for (auto cursor = read; cursor < write; ++cursor) {
            TimedEvent event{};
            std::memcpy(
                &event,
                source_base + source.events_relative_offset
                    + (cursor % source.event_capacity) * sizeof(TimedEvent),
                sizeof(event));
            if (!inherited.contains(event.time)) continue;
            IV_ASSERT(
                target_write < target.event_capacity,
                "prepared event transition exceeds successor ring capacity");
            std::memcpy(
                target_base + target.events_relative_offset
                    + target_write * sizeof(TimedEvent),
                &event,
                sizeof(event));
            ++target_write;
        }
        std::size_t const zero = 0;
        std::memcpy(
            target_base + target.read_index_relative_offset,
            &zero,
            sizeof(zero));
        std::memcpy(
            target_base + target.write_index_relative_offset,
            &target_write,
            sizeof(target_write));
    }
}

} // namespace iv::graph_jit
