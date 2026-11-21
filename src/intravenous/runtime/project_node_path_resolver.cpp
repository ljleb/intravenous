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
namespace {
using NodePath = std::vector<ProjectNodePathSelector>;

bool path_selects_bundle(ProjectNodePathGraph graph, NodeBundleHandle subject,
    NodePath const& path, NodeBundleHandle target)
{
    auto const matches = resolve_project_node_path(graph, subject, path);
    return matches.size() == 1 && matches.front().kind == ProjectNodePathMatch::Kind::bundle
        && matches.front().bundle == target;
}

std::optional<NodePath> virtual_member_path(ProjectNodePathGraph graph,
    NodeBundleHandle parent, NodeBundleHandle target)
{
    for (auto const& record : graph.virtual_nodes.records()) {
        auto const found = std::ranges::find(record.node_bundle_handles, target);
        if (found == record.node_bundle_handles.end()) continue;
        auto const index = std::ranges::count_if(
            std::ranges::subrange(record.node_bundle_handles.begin(), found), [&](auto member) {
                return direct_member_of_scope(graph, member, parent);
            });
        NodePath candidate{
            ProjectVirtualNodeSelector{record.source_identity, record.type_identity},
            ProjectVirtualMemberSelector{static_cast<std::size_t>(index)},
        };
        if (path_selects_bundle(graph, parent, candidate, target)) return candidate;
    }
    return std::nullopt;
}

std::optional<NodePath> tiled_child_path(ProjectNodePathGraph graph,
    NodeBundleHandle parent, NodeBundleHandle target)
{
    auto const& bundle = graph.node_bundles.bundle(parent);
    if (!bundle.is_tiled()) return std::nullopt;
    auto const members = bundle.tiled_members();
    auto const member = std::ranges::find(members, target);
    if (member == members.end()) return std::nullopt;
    auto const index = static_cast<std::size_t>(member - members.begin());
    auto const& names = channel_type_info(bundle.tiled_channel_type()).channel_names;
    if (index >= names.size()) return std::nullopt;
    NodePath candidate{ProjectTiledChildSelector{.name = std::string(names[index])}};
    if (path_selects_bundle(graph, parent, candidate, target)) return candidate;
    return std::nullopt;
}
} // namespace

namespace {
std::string virtual_declaration_name(VirtualNodeRecord const& record)
{
    auto const named = std::ranges::find_if(record.source_infos,
        [](auto const& info) { return !info.declaration_name.empty(); });
    if (named != record.source_infos.end()) return named->declaration_name;
    // Explicit builder annotations may already provide an identifier.
    auto const ascii_letter = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    };
    auto const identity = std::string_view(record.source_identity);
    auto const marker = identity.rfind('@');
    auto const name = marker == std::string_view::npos ? identity : identity.substr(marker + 1);
    if (!name.empty() && ascii_letter(name.front())
        && std::ranges::all_of(name, [&](char c) { return ascii_letter(c) || (c >= '0' && c <= '9'); }))
        return std::string(name);
    return "Node";
}

} // namespace

std::vector<ProjectChildNode> project_child_nodes(
    ConfiguredGraph const& configured, ProjectNodePathMatch const& subject)
{
    ProjectNodePathGraph const graph{configured.node_bundles, configured.virtual_nodes};
    std::vector<ProjectChildNode> children;
    if (subject.kind == ProjectNodePathMatch::Kind::virtual_node) {
        auto const& record = graph.virtual_nodes.record(subject.virtual_node);
        auto const name = virtual_declaration_name(record);
        for (std::size_t index = 0; index < subject.virtual_member_indices.size(); ++index) {
            auto const bundle = record.node_bundle_handles[subject.virtual_member_indices[index]];
            children.push_back({bundle, {ProjectVirtualMemberSelector{index}}, name + " " + std::to_string(index + 1)});
        }
        return children;
    }
    auto const& scope = graph.node_bundles.bundle(subject.bundle);
    if (scope.is_tiled()) {
        for (auto const member : scope.tiled_members()) {
            if (auto path = tiled_child_path(graph, subject.bundle, member)) {
                auto name = std::get<ProjectTiledChildSelector>(path->front()).name;
                children.push_back({member, std::move(*path), std::move(name)});
            }
        }
    } else if (scope.is_subgraph()) {
        auto const records = graph.virtual_nodes.records();
        for (VirtualNodeHandle handle = 0; handle < records.size(); ++handle) {
            auto const& record = records[handle];
            if (std::ranges::none_of(record.node_bundle_handles, [&](auto member) {
                    return member != subject.bundle && subgraph_contains(graph, subject.bundle, member);
                })) continue;
            NodePath path{ProjectVirtualNodeSelector{record.source_identity, record.type_identity}};
            auto const matches = resolve_project_node_path(graph, subject.bundle, path);
            if (matches.size() != 1 || matches.front().kind != ProjectNodePathMatch::Kind::virtual_node
                || matches.front().virtual_node != handle) continue;
            auto const& members = matches.front().virtual_member_indices;
            if (members.empty() || std::ranges::any_of(members, [&](auto index) {
                    return record.node_bundle_handles[index] == subject.bundle;
                })) continue;
            children.push_back({record.node_bundle_handles[members.front()], std::move(path),
                virtual_declaration_name(record), handle});
        }
    }
    return children;
}

std::optional<std::vector<ProjectNodePathSelector>> project_member_path(
    ConfiguredGraph const& configured, NodeBundleHandle root, NodeBundleHandle target)
{
    ProjectNodePathGraph const graph{configured.node_bundles, configured.virtual_nodes};
    if (root == target) return NodePath{};
    auto const& root_bundle = graph.node_bundles.bundle(root);
    if (!root_bundle.is_subgraph() && !root_bundle.is_tiled()) return std::nullopt;
    if (auto path = tiled_child_path(graph, root, target)) return path;

    auto append_to_parent = [&](NodeBundleHandle parent, NodePath const& suffix)
        -> std::optional<NodePath> {
        auto path = project_member_path(configured, root, parent);
        if (!path) return std::nullopt;
        path->insert(path->end(), suffix.begin(), suffix.end());
        if (path_selects_bundle(graph, root, *path, target)) return path;
        return std::nullopt;
    };
    if (auto const parent = immediate_parent_subgraph(graph, target)) {
        if (auto suffix = virtual_member_path(graph, *parent, target)) {
            if (auto path = append_to_parent(*parent, *suffix)) return path;
        }
    }
    for (NodeBundleHandle parent = 0; parent < graph.node_bundles.size(); ++parent) {
        if (parent == target || parent == root) continue;
        if (auto suffix = tiled_child_path(graph, parent, target)) {
            if (auto path = append_to_parent(parent, *suffix)) return path;
        }
    }
    return std::nullopt;
}

} // namespace iv
