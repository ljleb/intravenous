#include <intravenous/graph_jit/bundle_migration.h>

#include <intravenous/graph/configured_graph.hpp>

#include <algorithm>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace iv::graph_jit {
namespace {

struct Membership {
    std::string_view record_id;
    std::string_view type;
    std::size_t member;
    bool operator==(Membership const&) const = default;
};

struct MembershipHash {
    std::size_t operator()(Membership const& membership) const noexcept
    {
        auto const hash = std::hash<std::string_view>{};
        return hash(membership.record_id) ^ (hash(membership.type) << 1)
            ^ (std::hash<std::size_t>{}(membership.member) << 2);
    }
};

using OrderedMemberships = std::vector<std::vector<Membership>>;
constexpr auto no_bundle = std::numeric_limits<NodeBundleHandle>::max();

OrderedMemberships ordered_memberships(ConfiguredGraph const& graph)
{
    struct RecordMember { SymbolicNodeHandle record; std::size_t index; };
    auto const& records = graph.symbolic_nodes.records();
    std::vector<std::vector<RecordMember>> by_bundle(graph.node_bundles.size());
    for (SymbolicNodeHandle record = 0; record < records.size(); ++record) {
        auto const& members = records[record].node_bundle_handles;
        for (std::size_t member = 0; member < members.size(); ++member) {
            if (members[member] < by_bundle.size())
                by_bundle[members[member]].push_back({record, member});
        }
    }

    OrderedMemberships result(by_bundle.size());
    std::vector<std::optional<std::size_t>> position(records.size());
    for (NodeBundleHandle bundle = 0; bundle < result.size(); ++bundle) {
        if (graph.project_leaf_instances.contains(bundle)) continue;
        auto const& value = graph.node_bundles.bundle(bundle);
        auto const type = value.type_identity();
        for (auto const [record, index] : by_bundle[bundle]) position[record] = index;
        for (auto const handle : value.symbolic_node_handles()) {
            auto const& record = records.at(handle);
            if (position[handle] && record.type_identity == type)
                result[bundle].push_back({record.id, record.type_identity, *position[handle]});
        }
        for (auto const& member : by_bundle[bundle]) position[member.record].reset();
    }
    return result;
}

struct RankedBundle {
    NodeBundleHandle bundle = no_bundle;
    std::size_t rank = 0;
};

auto index_memberships(OrderedMemberships const& memberships)
{
    std::unordered_map<Membership, RankedBundle, MembershipHash> index;
    for (NodeBundleHandle bundle = 0; bundle < memberships.size(); ++bundle) {
        auto const& aliases = memberships[bundle];
        for (std::size_t rank = 0; rank < aliases.size(); ++rank) {
            auto [entry, inserted] = index.try_emplace(aliases[rank], RankedBundle{bundle, rank});
            if (!inserted && entry->second.bundle != bundle) entry->second.bundle = no_bundle;
        }
    }
    return index;
}

// One proposal per counterpart. The old bundle uses its earliest shared
// membership to order proposals. The new bundle ranks that same pair using
// its own earliest shared membership, which may be a different alias.
std::vector<std::vector<RankedBundle>> index_proposals(
    OrderedMemberships const& before, OrderedMemberships const& after)
{
    auto const old_index = index_memberships(before);
    auto const new_index = index_memberships(after);
    std::vector<std::vector<RankedBundle>> proposals(before.size());
    std::vector<std::size_t> destination_position(after.size(), no_bundle);
    for (NodeBundleHandle old = 0; old < before.size(); ++old) {
        auto& choices = proposals[old];
        for (auto const& alias : before[old]) {
            if (old_index.at(alias).bundle != old) continue;
            auto const target = new_index.find(alias);
            if (target == new_index.end() || target->second.bundle == no_bundle) continue;
            auto const [bundle, rank] = target->second;
            auto& position = destination_position[bundle];
            if (position == no_bundle) {
                position = choices.size();
                choices.push_back({bundle, rank});
            } else {
                choices[position].rank = std::min(choices[position].rank, rank);
            }
        }
        for (auto const choice : choices) destination_position[choice.bundle] = no_bundle;
    }
    return proposals;
}

std::vector<RankedBundle> match_old_first(
    OrderedMemberships const& before, OrderedMemberships const& after)
{
    auto const proposals = index_proposals(before, after);
    std::vector<std::size_t> next_proposal(before.size());
    std::vector<RankedBundle> accepted(after.size());
    std::vector<NodeBundleHandle> pending;
    for (NodeBundleHandle old = 0; old < before.size(); ++old)
        if (!proposals[old].empty()) pending.push_back(old);

    while (!pending.empty()) {
        auto const old = pending.back();
        pending.pop_back();
        auto& next = next_proposal[old];
        while (next < proposals[old].size()) {
            auto const [destination, rank] = proposals[old][next++];
            auto& current = accepted[destination];
            if (current.bundle != no_bundle && current.rank <= rank) continue;
            if (current.bundle != no_bundle) pending.push_back(current.bundle);
            current = {old, rank};
            break;
        }
    }
    return accepted;
}

StableConcreteNodeId fresh_symbolic_identity(ConfiguredGraph const& graph,
    ConfiguredBundleInventory inventory, NodeBundleHandle bundle,
    std::unordered_set<std::string>& occupied)
{
    StableConcreteNodeId identity{
        .graph = "symbolic-birth:" + graph.identity.value,
        .direct_member = bundle, .configuration_snapshot = std::max<std::uint64_t>(inventory.snapshot, 1),
    };
    while (!occupied.insert(migration_identity_fields(identity)).second) ++identity.configuration_snapshot;
    return identity;
}

} // namespace

BundleMigrationIdentities assign_bundle_migration_identities(
    ConfiguredGraph const& graph, ConfiguredBundleInventory inventory,
    ConfiguredGraph const* predecessor, BundleMigrationIdentities const* predecessor_identities)
{
    auto memberships = ordered_memberships(graph);
    BundleMigrationIdentities result(memberships.size());
    std::unordered_set<std::string> occupied;
    if (predecessor && predecessor_identities && predecessor->identity.value == graph.identity.value) {
        auto old_memberships = ordered_memberships(*predecessor);
        // A reused frozen invocation has one correspondence per local bundle,
        // regardless of where the project builder placed it in either root.
        NodeAddress const* matched_invocation = nullptr;
        for (auto const& [address, current] : graph.project_invocations) {
            // Address order visits parents before descendants. A matched
            // parent already maps every bundle in those descendants.
            if (matched_invocation && address.instance_id == matched_invocation->instance_id
                && address.path.size() >= matched_invocation->path.size()
                && std::equal(matched_invocation->path.begin(), matched_invocation->path.end(), address.path.begin()))
                continue;
            auto const found = predecessor->project_invocations.find(address);
            if (found == predecessor->project_invocations.end()) continue;
            auto const& previous = found->second;
            if (!current.birth_snapshot || current.birth_snapshot != previous.birth_snapshot
                || !current.frozen_invocation_id || current.frozen_invocation_id != previous.frozen_invocation_id
                || current.count != previous.count
                || current.first > result.size() || current.count > result.size() - current.first
                || previous.first > old_memberships.size() || previous.count > old_memberships.size() - previous.first
                || previous.first > predecessor_identities->size()
                || previous.count > predecessor_identities->size() - previous.first)
                continue;
            matched_invocation = &address;
            for (std::size_t local = 0; local < current.count; ++local) {
                auto const old = previous.first + local;
                auto const next = current.first + local;
                if (auto const& identity = (*predecessor_identities)[old]) {
                    result[next] = identity;
                    old_memberships[old].clear();
                    memberships[next].clear();
                }
            }
        }
        auto const matched = match_old_first(old_memberships, memberships);
        for (auto const& identity : *predecessor_identities)
            if (identity) occupied.insert(migration_identity_fields(*identity));
        for (NodeBundleHandle bundle = 0; bundle < result.size(); ++bundle) {
            auto const old = matched[bundle].bundle;
            if (!result[bundle] && old < predecessor_identities->size()) result[bundle] = (*predecessor_identities)[old];
        }
    }

    for (NodeBundleHandle bundle = 0; bundle < result.size(); ++bundle) {
        if (result[bundle]) continue;
        if (auto const leaf = graph.project_leaf_instances.find(bundle); leaf != graph.project_leaf_instances.end()) {
            result[bundle] = StableConcreteNodeId{
                .graph = graph.identity.value, .project_instance = leaf->second,
                .project_leaf_type = std::string(graph.node_bundles.bundle(bundle).type_identity()),
            };
        } else if (!memberships[bundle].empty()) {
            auto const& first = memberships[bundle].front();
            auto identity = StableConcreteNodeId{
                .graph = graph.identity.value, .symbolic_node = std::string(first.record_id), .direct_member = first.member,
            };
            // A retained identity can follow another alias. A newly created
            // bundle at its former name must start a separate lifetime.
            if (!occupied.insert(migration_identity_fields(identity)).second)
                identity = fresh_symbolic_identity(graph, inventory, bundle, occupied);
            result[bundle] = std::move(identity);
        } else if (inventory.snapshot && bundle < inventory.size) {
            result[bundle] = StableConcreteNodeId{.direct_member = bundle, .configuration_snapshot = inventory.snapshot};
        }
    }
    return result;
}

} // namespace iv::graph_jit
