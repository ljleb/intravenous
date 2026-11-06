#include <intravenous/runtime/project_node_path_resolver.h>

#include <algorithm>
#include <limits>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace iv {
namespace {
bool subgraph_contains(
    ProjectNodePathGraph const& graph,
    NodeBundleHandle subgraph,
    NodeBundleHandle candidate)
{
    auto const& bundle = graph.node_bundles.bundle(subgraph);
    if (!bundle.is_subgraph()) return false;
    auto const begin = bundle.subgraph_child_begin();
    auto const end = begin + bundle.subgraph_child_count();
    return candidate >= begin && candidate < end;
}

std::optional<NodeBundleHandle> immediate_parent_subgraph(
    ProjectNodePathGraph const& graph,
    NodeBundleHandle candidate)
{
    std::optional<NodeBundleHandle> result;
    std::size_t result_extent = std::numeric_limits<std::size_t>::max();
    for (NodeBundleHandle handle = 0; handle < graph.node_bundles.size(); ++handle) {
        if (handle == candidate) continue;
        auto const& bundle = graph.node_bundles.bundle(handle);
        if (!bundle.is_subgraph() || !subgraph_contains(graph, handle, candidate))
            continue;
        if (bundle.subgraph_child_count() < result_extent) {
            result = handle;
            result_extent = bundle.subgraph_child_count();
        }
    }
    return result;
}

void append_subject_scopes(
    ProjectNodePathGraph const& graph,
    NodeBundleHandle subject,
    std::vector<NodeBundleHandle>& output)
{
    auto const& bundle = graph.node_bundles.bundle(subject);
    if (!bundle.is_tiled()) {
        if (!std::ranges::contains(output, subject)) output.push_back(subject);
        return;
    }
    for (auto const member : bundle.tiled_members()) {
        if (!std::ranges::contains(output, member)) output.push_back(member);
    }
}

std::vector<NodeBundleHandle> subject_scopes(
    ProjectNodePathGraph const& graph,
    ProjectNodePathMatch const& match)
{
    std::vector<NodeBundleHandle> result;
    if (match.kind == ProjectNodePathMatch::Kind::bundle) {
        append_subject_scopes(graph, match.bundle, result);
        return result;
    }
    auto const& record = graph.virtual_nodes.record(match.virtual_node);
    for (auto const index : match.virtual_member_indices) {
        if (index >= record.node_bundle_handles.size()) continue;
        auto const member = record.node_bundle_handles[index];
        auto const& bundle = graph.node_bundles.bundle(member);
        if (bundle.is_subgraph()) {
            result.push_back(member);
        } else if (bundle.is_tiled()) {
            for (auto const child : bundle.tiled_members()) {
                if (graph.node_bundles.bundle(child).is_subgraph()
                    && !std::ranges::contains(result, child)) {
                    result.push_back(child);
                }
            }
        }
    }
    return result;
}

bool direct_member_of_scope(
    ProjectNodePathGraph const& graph,
    NodeBundleHandle member,
    NodeBundleHandle scope)
{
    if (member == scope) return true;
    return immediate_parent_subgraph(graph, member) == scope;
}

bool virtual_selector_matches(
    VirtualNodeRecord const& record,
    ProjectVirtualNodeSelector const& selector)
{
    return record.source_identity == selector.source_identity
        && (!selector.type_identity
            || record.type_identity == *selector.type_identity);
}

void append_unique(
    std::vector<ProjectNodePathMatch>& output,
    ProjectNodePathMatch value)
{
    if (!std::ranges::contains(output, value)) output.push_back(std::move(value));
}

std::vector<ProjectNodePathMatch> apply_selector(
    ProjectNodePathGraph const& graph,
    std::span<ProjectNodePathMatch const> current,
    ProjectNodePathSelector const& selector)
{
    std::vector<ProjectNodePathMatch> result;
    std::visit([&](auto const& step) {
        using Step = std::remove_cvref_t<decltype(step)>;
        if constexpr (std::same_as<Step, ProjectVirtualNodeSelector>) {
            for (auto const& subject : current) {
                auto const scopes = subject_scopes(graph, subject);
                for (VirtualNodeHandle handle = 0;
                     handle < graph.virtual_nodes.records().size(); ++handle) {
                    if (subject.kind == ProjectNodePathMatch::Kind::virtual_node
                        && subject.virtual_node == handle) {
                        continue;
                    }
                    auto const& record = graph.virtual_nodes.record(handle);
                    if (!virtual_selector_matches(record, step)) continue;
                    std::vector<std::size_t> direct_members;
                    for (std::size_t index = 0;
                         index < record.node_bundle_handles.size(); ++index) {
                        auto const member = record.node_bundle_handles[index];
                        if (std::ranges::any_of(
                                scopes,
                                [&](NodeBundleHandle scope) {
                                    return direct_member_of_scope(
                                        graph, member, scope);
                                })) {
                            direct_members.push_back(index);
                        }
                    }
                    if (!direct_members.empty()) append_unique(result, ProjectNodePathMatch{
                        .kind = ProjectNodePathMatch::Kind::virtual_node,
                        .virtual_node = handle,
                        .virtual_member_indices = std::move(direct_members),
                    });
                }
            }
        } else if constexpr (std::same_as<Step, ProjectVirtualMemberSelector>) {
            for (auto const& subject : current) {
                if (subject.kind != ProjectNodePathMatch::Kind::virtual_node) continue;
                auto const& members = graph.virtual_nodes
                    .record(subject.virtual_node).node_bundle_handles;
                if (step.index >= subject.virtual_member_indices.size()) continue;
                auto const member_index =
                    subject.virtual_member_indices[step.index];
                if (member_index >= members.size()) continue;
                append_unique(result, ProjectNodePathMatch{
                    .kind = ProjectNodePathMatch::Kind::bundle,
                    .bundle = members[member_index],
                });
            }
        } else if constexpr (std::same_as<Step, ProjectTiledChildSelector>) {
            for (auto const& subject : current) {
                if (subject.kind != ProjectNodePathMatch::Kind::bundle) continue;
                auto const& bundle = graph.node_bundles.bundle(subject.bundle);
                if (!bundle.is_tiled()) continue;
                auto const& type = channel_type_info(bundle.tiled_channel_type());
                auto const found = std::ranges::find(type.channel_names, step.name);
                if (found == type.channel_names.end()) continue;
                auto const index = static_cast<std::size_t>(
                    found - type.channel_names.begin());
                auto const members = bundle.tiled_members();
                if (index >= members.size()) continue;
                append_unique(result, ProjectNodePathMatch{
                    .kind = ProjectNodePathMatch::Kind::bundle,
                    .bundle = members[index],
                });
            }
        }
    }, selector);
    return result;
}
} // namespace

std::vector<ProjectNodePathMatch> resolve_project_node_path(
    ConfiguredGraph const& graph,
    NodeBundleHandle subject,
    std::span<ProjectNodePathSelector const> path)
{
    return resolve_project_node_path(
        ProjectNodePathGraph{graph.node_bundles, graph.virtual_nodes}, subject, path);
}

std::vector<ProjectNodePathMatch> resolve_project_node_path(
    ProjectNodePathGraph graph,
    NodeBundleHandle subject,
    std::span<ProjectNodePathSelector const> path)
{
    std::vector<ProjectNodePathMatch> result{{
        .kind = ProjectNodePathMatch::Kind::bundle,
        .bundle = subject,
    }};
    for (auto const& selector : path) {
        result = apply_selector(graph, result, selector);
        if (result.empty()) break;
    }
    return result;
}

std::vector<ProjectNodePathMatch> resolve_project_node_path(
    ConfiguredGraph const& graph,
    std::unordered_map<std::string, NodeInstancePlacement> const& placements,
    std::string_view instance_id,
    std::span<ProjectNodePathSelector const> path)
{
    auto const placement = placements.find(std::string(instance_id));
    if (placement == placements.end()) return {};
    return resolve_project_node_path(graph, placement->second.root, path);
}

std::vector<ProjectNodePathMatch> resolve_node_address(
    ConfiguredGraph const& graph,
    std::unordered_map<std::string, NodeInstancePlacement> const& placements,
    NodeAddress const& address)
{
    return resolve_project_node_path(
        graph, placements, address.instance_id, address.path);
}

std::vector<NodeBundleHandle> concrete_node_address_matches(
    ConfiguredGraph const& graph,
    std::span<ProjectNodePathMatch const> matches)
{
    std::vector<NodeBundleHandle> result;
    for (auto const& match : matches) {
        if (match.kind == ProjectNodePathMatch::Kind::bundle) {
            auto const& bundle = graph.node_bundles.bundle(match.bundle);
            if (bundle.is_tiled()) {
                for (auto const member : bundle.tiled_members()) {
                    if (!std::ranges::contains(result, member)) {
                        result.push_back(member);
                    }
                }
            } else if (!std::ranges::contains(result, match.bundle)) {
                result.push_back(match.bundle);
            }
            continue;
        }
        auto const& members = graph.virtual_nodes
            .record(match.virtual_node).node_bundle_handles;
        for (auto const member_index : match.virtual_member_indices) {
            if (member_index >= members.size()) continue;
            auto const member = members[member_index];
            auto const& bundle = graph.node_bundles.bundle(member);
            if (bundle.is_tiled()) {
                for (auto const child : bundle.tiled_members()) {
                    if (!std::ranges::contains(result, child)) {
                        result.push_back(child);
                    }
                }
            } else if (!std::ranges::contains(result, member)) {
                result.push_back(member);
            }
        }
    }
    return result;
}

ProjectNodePathMatch require_single_project_node_path_match(
    std::span<ProjectNodePathMatch const> matches,
    std::string_view description)
{
    if (matches.size() != 1) {
        throw std::invalid_argument(
            std::string(description) + " resolved to "
            + std::to_string(matches.size())
            + " nodes; exactly one node is required");
    }
    return matches.front();
}
} // namespace iv
