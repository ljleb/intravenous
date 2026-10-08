#include <intravenous/graph_jit/realtime_port_state_transition.h>

#include <algorithm>
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
            result.sample_states.push_back(std::move(*transition));
        } else {
            result.event_states.push_back(std::move(*transition));
        }
    }
    return result;
}

} // namespace iv::graph_jit
