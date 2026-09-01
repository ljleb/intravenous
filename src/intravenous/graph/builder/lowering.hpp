#pragma once

#include <intravenous/basic_nodes/type_erased.h>
#include <intravenous/graph/builder/node_bundles.hpp>
#include <intravenous/basic_nodes/routing.h>
#include <intravenous/graph/builder/connections.hpp>
#include <intravenous/graph/builder/detach.hpp>
#include <intravenous/graph/builder/public_ports.hpp>
#include <intravenous/graph/builder/virtual_nodes.hpp>
#include <intravenous/graph/authored_graph.hpp>
#include <intravenous/graph/error.h>
#include <intravenous/graph/executable_graph_ir.hpp>
#include <intravenous/graph/hash.hpp>
#include <intravenous/graph/connection_node.hpp>
#include <intravenous/graph/runtime_binding_nodes.hpp>
#include <intravenous/graph/reflected_node.hpp>

#include <algorithm>
#include <flat_map>
#include <flat_set>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace iv {
struct TopologyEdge {
  TopologyPortId source{};
  TopologyPortId target{};
  auto operator<=>(TopologyEdge const&) const = default;
};

struct TopologyEventEdge {
  TopologyPortId source{};
  TopologyPortId target{};
  EventConversionPlan conversion{};
  constexpr bool operator==(TopologyEventEdge const& rhs) const {
    return source == rhs.source && target == rhs.target;
  }
  constexpr auto operator<=>(TopologyEventEdge const& rhs) const {
    if (auto ordering = source <=> rhs.source; ordering != 0)
      return ordering;
    return target <=> rhs.target;
  }
};

struct GraphLoweringOptions {
  bool execution_root = false;
};

// Diagnostic checkpoints for the build benchmark. They retain intermediate
// state only long enough to force the selected constexpr work to complete.
enum class GraphLoweringProfileStage {
  topology,
  materialization,
  normalization,
};

namespace details {
struct GraphLowererTestAccess;

struct LoweredNodeBundleProjection {
  std::vector<std::vector<TopologyPortId>> sample_inputs{};
  std::vector<std::vector<TopologyPortId>> sample_outputs{};
  std::vector<std::vector<TopologyPortId>> event_inputs{};
  std::vector<std::vector<TopologyPortId>> event_outputs{};
  std::optional<size_t> topology_node{};
};

struct DetachedSamplePortInfo {
  size_t detach_id = 0;
  TopologyPortId original_source{};
  size_t writer_node = std::numeric_limits<size_t>::max();
  TopologyPortId reader_output{};
  size_t loop_extra_latency = 1;
};

struct SampleLoweringPassFacts {
  size_t planned_groups = 0;
  size_t connected_bound_targets = 0;
  size_t vacant_bound_targets = 0;
  size_t assigned_subgraph_outputs = 0;
};

// Mechanical workspace used only while converting authored intent into the
// closed ExecutableGraphIR. It is not a third semantic graph representation.
struct LoweringWorkspace {
  std::vector<StoredNode> topology_nodes{};
  std::flat_set<TopologyEdge> topology_edges{};
  std::flat_set<TopologyEventEdge> topology_event_edges{};
  std::vector<TopologyEdge> pending_topology_edges{};
  std::vector<TopologyEventEdge> pending_topology_event_edges{};
  size_t scope_boundary_port_count = 0;
  std::vector<LoweredNodeBundleProjection> bundle_projections{};
  std::vector<std::optional<NodeBundleHandle>> bundle_by_lowered_node{};
  // Scope membership is retained for every topology node once the complete
  // topology exists. Generated nodes inherit memberships from their adjacent
  // authored components in one global closure rather than being rediscovered
  // separately for every scope.
  std::vector<std::vector<size_t>> scope_memberships{};
  details::ConstexprHashMap<TopologyPortId, TopologyPortId,
      iv::TopologyPortIdHash>
      subgraph_input_of_boundary_source{};
  details::ConstexprHashMap<TopologyPortId, TopologyPortId,
      iv::TopologyPortIdHash>
      subgraph_event_input_of_boundary_source{};
  details::ConstexprHashMap<TopologyPortId, DetachedSamplePortInfo,
      iv::TopologyPortIdHash>
      detached_info_by_source{};
  details::ConstexprHashSet<TopologyPortId, iv::TopologyPortIdHash>
      detached_reader_outputs{};
};
} // namespace details

namespace details {
constexpr std::string decimal_string(size_t value) {
  char digits[std::numeric_limits<size_t>::digits10 + 2] {};
  size_t begin = sizeof(digits);
  do {
    digits[--begin] = static_cast<char>('0' + value % 10);
    value /= 10;
  } while (value != 0);
  return std::string(digits + begin, digits + sizeof(digits));
}

struct VirtualConcretePortInfo {
  std::string name;
  std::string type;
  bool connected = false;
  size_t history = 0;
  size_t latency = 0;
  Sample default_value = 0.0f;
  std::optional<Sample> min{};
  std::optional<Sample> max{};
};

struct VirtualConcreteNode {
  std::string id;
  std::string kind;
  std::string type_identity;
  size_t construction_order = 0;
  std::span<SourceInfo const> source_infos{};
  std::vector<VirtualConcretePortInfo> sample_inputs;
  std::vector<VirtualConcretePortInfo> sample_outputs;
  std::vector<VirtualConcretePortInfo> event_inputs;
  std::vector<VirtualConcretePortInfo> event_outputs;
};


constexpr void sort_and_deduplicate_spans(std::vector<SourceSpan>& spans) {
  std::sort(spans.begin(), spans.end(), [](auto const& a, auto const& b) {
    return std::tie(a.file_path, a.begin, a.end) <
           std::tie(b.file_path, b.begin, b.end);
  });
  spans.erase(std::unique(spans.begin(), spans.end()), spans.end());
}

constexpr std::vector<SourceSpan>
source_spans_for(std::span<VirtualConcreteNode const* const> nodes) {
  std::vector<SourceSpan> spans;
  for (auto const* node : nodes) {
    for (auto const& info : node->source_infos) {
      if (info.span.file_path.empty() || info.span.begin > info.span.end) {
        continue;
      }
      spans.push_back(info.span);
    }
  }
  sort_and_deduplicate_spans(spans);
  return spans;
}

constexpr std::vector<IntrospectionPortInfo> aggregate_ports(
    std::span<VirtualConcreteNode const* const> nodes,
    auto VirtualConcreteNode::* member) {
  if (nodes.empty()) return {};

  auto const& first_ports = nodes.front()->*member;
  std::vector<IntrospectionPortInfo> virtual_ports;
  virtual_ports.reserve(first_ports.size());
  for (size_t i = 0; i < first_ports.size(); ++i) {
    bool any_connected = false;
    bool any_disconnected = false;
    for (auto const* node : nodes) {
      auto const connected = (node->*member)[i].connected;
      any_connected = any_connected || connected;
      any_disconnected = any_disconnected || !connected;
    }
    auto const connectivity = any_connected && any_disconnected
        ? VirtualPortConnectivity::mixed
        : any_connected ? VirtualPortConnectivity::connected
                        : VirtualPortConnectivity::disconnected;

    virtual_ports.push_back(IntrospectionPortInfo{
        .name = first_ports[i].name,
        .type = first_ports[i].type,
        .connectivity = connectivity,
        .ordinal = i,
        .default_value = first_ports[i].default_value,
        .min = first_ports[i].min,
        .max = first_ports[i].max,
        .history = first_ports[i].history,
        .latency = first_ports[i].latency,
    });
  }
  return virtual_ports;
}

using VirtualNodeIdsByBackingNodeId =
    std::flat_map<std::string, std::vector<std::string>>;

using VirtualMetadataBuildResult =
    std::pair<std::vector<IntrospectionVirtualNode>,
              VirtualNodeIdsByBackingNodeId>;

constexpr VirtualMetadataBuildResult
build_virtual_metadata(ExecutableGraphData const& g,
                       std::span<LoweredSubgraphSpec const> lowered_scopes) {
  // The authored virtual-node records already hold the port metadata for
  // ordinary nodes. Rebuilding it from every reflected runtime node here used
  // to visit every generated ConnectionNode and then discard it: nodes without
  // an ID never enter a virtual group, while nodes with one are overwritten by
  // apply_virtual_port_metadata below. Preserve only the runtime association
  // needed by the compiler, and reserve graph-derived metadata work for
  // lowered scopes, which have no authored record to reuse.
  VirtualNodeIdsByBackingNodeId virtual_node_ids_by_backing_node_id;
  for (size_t node_i = 0; node_i < g.nodes.size(); ++node_i) {
    if (node_i >= g.node_virtual_ids.size() ||
        g.node_virtual_ids[node_i].empty()) {
      continue;
    }
    auto& ids = virtual_node_ids_by_backing_node_id[g.node_ids[node_i]];
    ids.insert(ids.end(), g.node_virtual_ids[node_i].begin(),
               g.node_virtual_ids[node_i].end());
  }

  std::vector<VirtualConcreteNode> concrete_nodes;
  std::vector<std::vector<std::string>> concrete_node_virtual_ids;
  concrete_nodes.reserve(lowered_scopes.size());
  concrete_node_virtual_ids.reserve(lowered_scopes.size());
  for (size_t scope_i = 0; scope_i < lowered_scopes.size(); ++scope_i) {
    auto const& scope = lowered_scopes[scope_i];
    VirtualConcreteNode concrete;
    concrete.id = scope.backing_node_id;
    concrete.kind = scope.kind;
    concrete.construction_order = g.nodes.size() + scope_i;
    concrete.type_identity = "lowered-subgraph:" + scope.kind;
    concrete.source_infos = std::span<SourceInfo const>(scope.source_infos);

    concrete.sample_inputs.reserve(scope.sample_inputs.size());
    for (size_t input_i = 0; input_i < scope.sample_inputs.size(); ++input_i) {
      concrete.sample_inputs.push_back(VirtualConcretePortInfo{
          .name = scope.sample_inputs[input_i].name,
          .type = "sample",
          .connected = input_i < scope.sample_input_targets.size() &&
                       !scope.sample_input_targets[input_i].empty(),
          .history = scope.sample_inputs[input_i].history,
          .default_value = scope.sample_inputs[input_i].default_value,
          .min = scope.sample_inputs[input_i].min,
          .max = scope.sample_inputs[input_i].max,
      });
    }

    concrete.sample_outputs.reserve(scope.sample_outputs.size());
    for (size_t output_i = 0; output_i < scope.sample_outputs.size();
         ++output_i) {
      concrete.sample_outputs.push_back(VirtualConcretePortInfo{
          .name = scope.sample_outputs[output_i].name,
          .type = "sample",
          .connected = output_i < scope.sample_output_sources.size() &&
                       scope.sample_output_sources[output_i].valid,
          .history = scope.sample_outputs[output_i].history,
          .latency = scope.sample_outputs[output_i].latency,
      });
    }

    concrete.event_inputs.reserve(scope.event_inputs.size());
    for (size_t input_i = 0; input_i < scope.event_inputs.size(); ++input_i) {
      concrete.event_inputs.push_back(VirtualConcretePortInfo{
          .name = scope.event_inputs[input_i].name,
          .type = event_type_name(scope.event_inputs[input_i].type),
          .connected = input_i < scope.event_input_targets.size() &&
                       !scope.event_input_targets[input_i].empty(),
      });
    }

    concrete.event_outputs.reserve(scope.event_outputs.size());
    for (size_t output_i = 0; output_i < scope.event_outputs.size();
         ++output_i) {
      concrete.event_outputs.push_back(VirtualConcretePortInfo{
          .name = scope.event_outputs[output_i].name,
          .type = event_type_name(scope.event_outputs[output_i].type),
          .connected = output_i < scope.event_output_sources.size() &&
                       scope.event_output_sources[output_i].valid,
      });
    }

    concrete_nodes.push_back(std::move(concrete));

    std::vector<std::string> virtual_node_ids;
    for (auto const& info : scope.source_infos) {
      if (info.declaration_identity.empty()) continue;
      auto const virtual_node_id = typed_virtual_node_id(
          info.declaration_identity,
          concrete_nodes.back().type_identity);
      if (!std::ranges::contains(virtual_node_ids, virtual_node_id)) {
        virtual_node_ids.push_back(virtual_node_id);
      }
    }
    concrete_node_virtual_ids.push_back(std::move(virtual_node_ids));
  }

  struct VirtualGroup {
    std::string virtual_node_id;
    std::vector<size_t> member_indices;
  };

  std::vector<VirtualGroup> groups;
  details::ConstexprHashMap<std::string, size_t, details::ConstexprStringHash>
      group_index_by_virtual_node_id;
  for (size_t i = 0; i < concrete_nodes.size(); ++i) {
    if (i >= concrete_node_virtual_ids.size() ||
        concrete_node_virtual_ids[i].empty()) {
      continue;
    }
    for (auto const& virtual_node_id : concrete_node_virtual_ids[i]) {
      auto group = group_index_by_virtual_node_id.try_emplace(
          virtual_node_id, groups.size());
      if (group.inserted) {
        groups.push_back(VirtualGroup{
            .virtual_node_id = virtual_node_id,
            .member_indices = {},
        });
      }
      // concrete_node_virtual_ids already deduplicates IDs per member.
      groups[group.value].member_indices.push_back(i);
    }
  }

  std::vector<IntrospectionVirtualNode> virtual_nodes;
  virtual_nodes.reserve(groups.size());
  for (size_t group_i = 0; group_i < groups.size(); ++group_i) {
    auto const& group = groups[group_i];
    std::vector<VirtualConcreteNode const*> members;
    members.reserve(group.member_indices.size());
    auto member_indices = group.member_indices;
    std::sort(member_indices.begin(), member_indices.end(),
              [&](auto a, auto b) {
                auto const& left = concrete_nodes[a];
                auto const& right = concrete_nodes[b];
                if (left.construction_order != right.construction_order) {
                  return left.construction_order < right.construction_order;
                }
                return left.id < right.id;
              });
    std::vector<std::string> backing_node_ids;
    backing_node_ids.reserve(group.member_indices.size());
    std::vector<IntrospectionVirtualNode::Member> virtual_members;
    virtual_members.reserve(member_indices.size());
    for (auto member_index : member_indices) {
      members.push_back(&concrete_nodes[member_index]);
      auto const& backing_node_id = concrete_nodes[member_index].id;
      if (!std::ranges::contains(backing_node_ids, backing_node_id)) {
        backing_node_ids.push_back(backing_node_id);
      }
      auto const one_member = std::span<VirtualConcreteNode const* const>(
          &members.back(), 1);
      virtual_members.push_back(IntrospectionVirtualNode::Member{
          .ordinal = virtual_members.size(),
          .backing_node_id = backing_node_id,
          .kind = concrete_nodes[member_index].kind,
          .type_identity = concrete_nodes[member_index].type_identity,
          .sample_inputs = aggregate_ports(
              one_member, &VirtualConcreteNode::sample_inputs),
          .sample_outputs = aggregate_ports(
              one_member, &VirtualConcreteNode::sample_outputs),
          .event_inputs = aggregate_ports(
              one_member, &VirtualConcreteNode::event_inputs),
          .event_outputs = aggregate_ports(
              one_member, &VirtualConcreteNode::event_outputs),
      });
    }

    auto source_spans = source_spans_for(members);
    virtual_nodes.push_back(IntrospectionVirtualNode{
        .id = group.virtual_node_id,
        .kind = members.front()->kind,
        .source_identity = group.virtual_node_id,
        .type_identity = members.front()->type_identity,
        .source_spans = std::move(source_spans),
        .sample_inputs =
            aggregate_ports(members, &VirtualConcreteNode::sample_inputs),
        .sample_outputs =
            aggregate_ports(members, &VirtualConcreteNode::sample_outputs),
        .event_inputs =
            aggregate_ports(members, &VirtualConcreteNode::event_inputs),
        .event_outputs =
            aggregate_ports(members, &VirtualConcreteNode::event_outputs),
        .backing_node_ids = std::move(backing_node_ids),
        .members = std::move(virtual_members),
    });
  }

  std::sort(virtual_nodes.begin(), virtual_nodes.end(),
            [](auto const& a, auto const& b) {
              auto const a_file = a.source_spans.empty()
                                      ? std::string{}
                                      : a.source_spans.front().file_path;
              auto const b_file = b.source_spans.empty()
                                      ? std::string{}
                                      : b.source_spans.front().file_path;
              if (a_file != b_file) return a_file < b_file;
              auto const a_begin =
                  a.source_spans.empty() ? 0u : a.source_spans.front().begin;
              auto const b_begin =
                  b.source_spans.empty() ? 0u : b.source_spans.front().begin;
              if (a_begin != b_begin) return a_begin < b_begin;
              auto const a_end =
                  a.source_spans.empty() ? 0u : a.source_spans.front().end;
              auto const b_end =
                  b.source_spans.empty() ? 0u : b.source_spans.front().end;
              if (a_end != b_end) return a_end < b_end;
              if (a.kind != b.kind) return a.kind < b.kind;
              if (a.id != b.id) return a.id < b.id;
              if (a.source_identity != b.source_identity) {
                return a.source_identity < b.source_identity;
              }
              return a.backing_node_ids < b.backing_node_ids;
            });

  for (auto const& virtual_node : virtual_nodes) {
    for (auto const& backing_node_id : virtual_node.backing_node_ids) {
      virtual_node_ids_by_backing_node_id[backing_node_id].push_back(
          virtual_node.id);
    }
  }
  for (auto&& [_, virtual_ids] : virtual_node_ids_by_backing_node_id) {
    std::sort(virtual_ids.begin(), virtual_ids.end());
    virtual_ids.erase(std::unique(virtual_ids.begin(), virtual_ids.end()),
                      virtual_ids.end());
  }

  return std::make_pair(std::move(virtual_nodes),
                        std::move(virtual_node_ids_by_backing_node_id));
}

struct SampleChannelIdHash {
  template <class ChannelId>
  constexpr size_t operator()(ChannelId const& value) const {
    auto result = constexpr_hash_combine(0, value.bundle);
    result = constexpr_hash_combine(result, value.port);
    return constexpr_hash_combine(result, value.channel);
  }
};

struct EventPortIdHash {
  template <class PortId>
  constexpr size_t operator()(PortId const& value) const {
    return constexpr_hash_combine(value.bundle, value.port);
  }
};

// Authored connectivity is queried heavily while projecting introspection.
// Build this lowerer-local index once instead of rescanning all authored
// connections for every virtual/public port.
struct LoweringAuthoredConnectivity {
  ConstexprHashSet<SampleInputChannelId, SampleChannelIdHash> sample_inputs;
  ConstexprHashSet<SampleOutputChannelId, SampleChannelIdHash> sample_outputs;
  ConstexprHashSet<EventInputPortId, EventPortIdHash> event_inputs;
  ConstexprHashSet<EventOutputPortId, EventPortIdHash> event_outputs;
};

constexpr LoweringAuthoredConnectivity build_lowering_authored_connectivity(
    GraphBuilderConnections const& connections) {
  LoweringAuthoredConnectivity result;
  for (auto const& connection : connections.authored_sample_connections()) {
    result.sample_inputs.insert_range(connection.target_channels.begin(),
                                      connection.target_channels.end());
    result.sample_outputs.insert_range(connection.source_channels.begin(),
                                       connection.source_channels.end());
  }
  for (auto const& connection : connections.authored_event_connections()) {
    result.event_inputs.insert_range(connection.targets.begin(),
                                     connection.targets.end());
    result.event_outputs.insert_range(connection.sources.begin(),
                                      connection.sources.end());
  }
  return result;
}

namespace {
constexpr bool sample_channel_is_connected(
    LoweringAuthoredConnectivity const& connectivity,
    SampleInputChannelId channel) {
  return connectivity.sample_inputs.contains(channel);
}

constexpr bool sample_channel_is_connected(
    LoweringAuthoredConnectivity const& connectivity,
    SampleOutputChannelId channel) {
  return connectivity.sample_outputs.contains(channel);
}

template <class Mapping>
constexpr std::vector<IntrospectionPortInfo> project_virtual_sample_ports(
    std::vector<Mapping> const& mappings,
    GraphBuilderNodeBundles const& node_bundles,
    LoweringAuthoredConnectivity const& connectivity, bool inputs) {
  std::vector<IntrospectionPortInfo> result;
  result.reserve(mappings.size());
  for (auto const& mapping : mappings) {
    if (mapping.channels.empty()) continue;

    auto const first_channel = mapping.channels.front();
    NodeBundlePortId const first_address{
        first_channel.bundle, PortKind::sample, first_channel.port};
    Sample default_value = 0.0f;
    std::optional<Sample> min;
    std::optional<Sample> max;
    size_t history = 0;
    size_t latency = 0;
    if (inputs) {
      auto const config = node_bundles.resolve_sample_input(first_address).config;
      default_value = config.default_value;
      min = config.min;
      max = config.max;
      history = config.history;
    } else {
      latency = node_bundles.resolve_sample_output(first_address).config.latency;
    }
    bool any_connected = false;
    bool any_disconnected = false;
    for (auto const channel : mapping.channels) {
      bool const connected = sample_channel_is_connected(connectivity, channel);
      any_connected = any_connected || connected;
      any_disconnected = any_disconnected || !connected;
    }
    result.push_back(IntrospectionPortInfo{
        .name = mapping.name,
        .type = "sample",
        .connectivity = any_connected && any_disconnected
            ? VirtualPortConnectivity::mixed
            : any_connected ? VirtualPortConnectivity::connected
                            : VirtualPortConnectivity::disconnected,
        .ordinal = mapping.ordinal,
        .default_value = default_value,
        .min = min,
        .max = max,
        .history = history,
        .latency = latency,
        .sample_channel_type = mapping.channel_layout.channel_type,
    });
  }
  return result;
}

template <class Mapping>
constexpr std::vector<IntrospectionPortInfo> project_bundle_sample_ports(
    std::vector<Mapping> const& mappings,
    GraphBuilderNodeBundles const& node_bundles,
    LoweringAuthoredConnectivity const& connectivity,
    NodeBundleHandle bundle_handle,
    bool inputs) {
  std::vector<Mapping> bundle_mappings;
  bundle_mappings.reserve(mappings.size());
  for (auto const& mapping : mappings) {
    auto projected = mapping;
    std::erase_if(projected.channels, [&](auto const channel) {
      return channel.bundle != bundle_handle;
    });
    if (projected.channels.empty()) continue;
    bundle_mappings.push_back(std::move(projected));
  }
  return project_virtual_sample_ports(
      bundle_mappings, node_bundles, connectivity, inputs);
}

constexpr std::vector<IntrospectionPortInfo> project_virtual_event_ports(
    std::vector<VirtualEventPortMapping> const& mappings,
    GraphBuilderNodeBundles const&,
    LoweringAuthoredConnectivity const& connectivity, bool inputs) {
  std::vector<IntrospectionPortInfo> result;
  result.reserve(mappings.size());
  for (auto const& mapping : mappings) {
    if (mapping.node_bundle_ports.empty()) continue;
    bool connected = false;
    for (auto const port : mapping.node_bundle_ports) {
      connected = connected || (inputs
          ? connectivity.event_inputs.contains(
                EventInputPortId{port.node_bundle_handle, port.port_ordinal})
          : connectivity.event_outputs.contains(
                EventOutputPortId{port.node_bundle_handle, port.port_ordinal}));
    }
    result.push_back(IntrospectionPortInfo{
        .name = mapping.name,
        .type = event_type_name(mapping.type),
        .connectivity = connected ? VirtualPortConnectivity::connected
                                  : VirtualPortConnectivity::disconnected,
        .ordinal = mapping.ordinal});
  }
  return result;
}

constexpr std::vector<IntrospectionPortInfo> project_bundle_event_ports(
    std::vector<VirtualEventPortMapping> const& mappings,
    GraphBuilderNodeBundles const& node_bundles,
    LoweringAuthoredConnectivity const& connectivity,
    NodeBundleHandle bundle_handle, bool inputs) {
  auto projected = mappings;
  for (auto& mapping : projected) {
    std::erase_if(mapping.node_bundle_ports, [&](auto const port) {
      return port.node_bundle_handle != bundle_handle;
    });
  }
  std::erase_if(projected, [](auto const& mapping) {
    return mapping.node_bundle_ports.empty();
  });
  return project_virtual_event_ports(
      projected, node_bundles, connectivity, inputs);
}

constexpr std::pair<std::string, std::string> bundle_display_type(
    NodeBundle const& bundle) {
  auto const type = std::string(bundle.type_identity());
  return {type, type};
}
} // namespace

constexpr void apply_virtual_port_metadata(
    GraphIntrospectionMetadata& metadata,
    GraphBuilderNodeBundles const& node_bundles,
    GraphBuilderVirtualNodes const& virtual_nodes,
    LoweringAuthoredConnectivity const& connectivity) {
  details::ConstexprHashMap<std::string, size_t, details::ConstexprStringHash>
      virtual_node_index;
  for (size_t i = 0; i < metadata.virtual_nodes.size(); ++i) {
    virtual_node_index.try_emplace(metadata.virtual_nodes[i].id, i);
  }
  for (auto const& record : virtual_nodes.records()) {
    auto const* index = virtual_node_index.find(record.id);
    if (!index) {
      std::vector<SourceSpan> spans;
      spans.reserve(record.source_infos.size());
      for (auto const& info : record.source_infos) spans.push_back(info.span);
      sort_and_deduplicate_spans(spans);
      metadata.virtual_nodes.push_back(IntrospectionVirtualNode {
          .id = record.id,
          .source_identity = record.source_identity,
          .type_identity = record.type_identity,
          .source_spans = std::move(spans),
      });
      virtual_node_index.try_emplace(
          record.id, metadata.virtual_nodes.size() - 1);
      index = virtual_node_index.find(record.id);
    }
    auto& node = metadata.virtual_nodes[*index];
    node.source_identity = record.source_identity;
    node.type_identity = record.type_identity;
    node.sample_inputs = project_virtual_sample_ports(
        record.sample_inputs, node_bundles, connectivity, true);
    node.sample_outputs = project_virtual_sample_ports(
        record.sample_outputs, node_bundles, connectivity, false);
    node.event_inputs = project_virtual_event_ports(
        record.event_inputs, node_bundles, connectivity, true);
    node.event_outputs = project_virtual_event_ports(
        record.event_outputs, node_bundles, connectivity, false);

    node.members.clear();
    node.backing_node_ids.clear();
    if (record.type_identity == "sample-port"
        || record.type_identity == "event-port") {
      node.kind = record.type_identity == "sample-port"
          ? "Sample port" : "Event port";
      auto const backing_id = record.type_identity + ":" + record.id;
      node.backing_node_ids.push_back(backing_id);
      node.members.push_back(IntrospectionVirtualNode::Member{
          .ordinal = 0,
          .backing_node_id = backing_id,
          .kind = node.kind,
          .type_identity = record.type_identity,
          .sample_inputs = node.sample_inputs,
          .sample_outputs = node.sample_outputs,
          .event_inputs = node.event_inputs,
          .event_outputs = node.event_outputs,
      });
      continue;
    }
    for (size_t bundle_ordinal = 0;
         bundle_ordinal < record.node_bundle_handles.size(); ++bundle_ordinal) {
      auto const handle = record.node_bundle_handles[bundle_ordinal];
      auto const& bundle = node_bundles.bundle(handle);
      auto const [kind, type_identity] = bundle_display_type(bundle);
      if (node.kind.empty()) node.kind = kind;
      if (node.type_identity.empty()) node.type_identity = type_identity;
      auto const backing_id = "node-bundle:" + record.id + ":" +
                              decimal_string(bundle_ordinal);
      node.backing_node_ids.push_back(backing_id);
      node.members.push_back(IntrospectionVirtualNode::Member{
          .ordinal = bundle_ordinal,
          .backing_node_id = backing_id,
          .kind = kind,
          .type_identity = type_identity,
          .sample_inputs = project_bundle_sample_ports(
              record.sample_inputs, node_bundles, connectivity, handle, true),
          .sample_outputs = project_bundle_sample_ports(
              record.sample_outputs, node_bundles, connectivity, handle, false),
          .event_inputs = project_bundle_event_ports(
              record.event_inputs, node_bundles, connectivity, handle, true),
          .event_outputs = project_bundle_event_ports(
              record.event_outputs, node_bundles, connectivity, handle, false),
      });
    }
  }
}
} // namespace details

namespace details {
    constexpr std::string lowered_generated_node_id(std::string_view builder_id, size_t generated_index)
    {
        std::string id(builder_id);
        if (!id.empty()) {
            id += ".";
        }
        id += "generated.";
        char digits[std::numeric_limits<size_t>::digits10 + 2] {};
        size_t begin = sizeof(digits);
        do {
            digits[--begin] = static_cast<char>('0' + generated_index % 10);
            generated_index /= 10;
        } while (generated_index != 0);
        id.append(digits + begin, digits + sizeof(digits));
        return id;
    }

    constexpr auto make_lowered_connected_output_sets(ExecutableGraphData const& g)
    {
        ConstexprHashSet<ConcretePortId, ConcretePortIdHash> sample_outputs;
        ConstexprHashSet<ConcretePortId, ConcretePortIdHash> event_outputs;

        for (GraphEdge const& edge : g.edges)
        {
            sample_outputs.insert(edge.source);
        }
        for (GraphEventEdge const& edge : g.event_edges)
        {
            event_outputs.insert(edge.source);
        }

        return std::make_tuple(
            std::move(sample_outputs), std::move(event_outputs)
        );
    }


    constexpr void expand_lowered_hyperedge_ports(
        ExecutableGraphData& g,
        std::string_view builder_id)
    {
        size_t next_construction_order = g.node_construction_order.empty()
            ? 0
            : (*std::ranges::max_element(g.node_construction_order) + 1);
        ConstexprHashSet<ConcretePortId, ConcretePortIdHash> sample_targets;
        for (GraphEdge const& edge : g.edges)
        {
            if (!sample_targets.insert(edge.target))
                error("sample fan-in reached completion after lowering instead of one ConnectionNode");
        }
        ConstexprHashMap<ConcretePortId, std::vector<GraphEventEdge>, ConcretePortIdHash>
            reverse_event_edges_map;
        for (GraphEventEdge const& edge : g.event_edges)
        {
            auto group = reverse_event_edges_map.try_emplace(
                edge.target, std::vector<GraphEventEdge>{});
            group.value.push_back(edge);
        }

        size_t nodes_size = g.nodes.size();
        for (size_t node = 0; node < nodes_size; ++node)
        {
            size_t const num_inputs = get_num_event_inputs(g.nodes[node]);
            for (size_t in_port = 0; in_port < num_inputs; ++in_port)
            {
                auto const* edges = reverse_event_edges_map.find({ node, in_port });
                if (!edges) continue;

                auto const& edges_to_expand = *edges;
                size_t const port_arity = edges_to_expand.size();
                if (port_arity <= 1) continue;

                EventTypeId const concat_type = get_event_inputs(g.nodes[node])[in_port].type;
                g.nodes.push_back(materialize_generated_node_or_forbid(
                    EventConcatenationNodeSpec{
                        .input_count = port_arity, .type = concat_type}));
                g.explicit_ttl_samples.push_back(std::nullopt);
                g.node_ids.push_back(lowered_generated_node_id(builder_id, g.node_ids.size()));
                g.node_virtual_ids.emplace_back();
                g.node_source_infos.emplace_back();
                g.node_construction_order.push_back(next_construction_order++);
                size_t const concat_node = g.nodes.size() - 1;

                for (size_t out_port = 0; out_port < edges_to_expand.size(); ++out_port)
                {
                    GraphEventEdge const& to_rewire = edges_to_expand[out_port];
                    g.event_edges.erase(to_rewire);
                    g.event_edges.insert(GraphEventEdge{ to_rewire.source, { concat_node, out_port }, to_rewire.conversion });
                }
                g.event_edges.insert(GraphEventEdge{
                    { concat_node, 0 },
                    { node, in_port },
                    EventConversionPlan{}
                });
            }
        }

        ConstexprHashMap<ConcretePortId, std::vector<GraphEventEdge>, ConcretePortIdHash>
            event_edges_map;
        for (GraphEventEdge const& edge : g.event_edges)
        {
            auto group = event_edges_map.try_emplace(
                edge.source, std::vector<GraphEventEdge>{});
            group.value.push_back(edge);
        }

        nodes_size = g.nodes.size();
        for (size_t node = 0; node < nodes_size; ++node)
        {
            size_t const num_outputs = get_num_event_outputs(g.nodes[node]);
            for (size_t out_port = 0; out_port < num_outputs; ++out_port)
            {
                auto const* edges = event_edges_map.find({ node, out_port });
                if (!edges) continue;

                auto const& edges_to_expand = *edges;
                size_t const port_arity = edges_to_expand.size();
                if (port_arity <= 1) continue;

                EventTypeId const broadcast_type = get_event_outputs(g.nodes[node])[out_port].type;
                g.nodes.push_back(materialize_generated_node_or_forbid(
                    BroadcastEventNodeSpec{
                        .output_count = port_arity, .type = broadcast_type}));
                g.explicit_ttl_samples.push_back(std::nullopt);
                g.node_ids.push_back(lowered_generated_node_id(builder_id, g.node_ids.size()));
                g.node_virtual_ids.emplace_back();
                g.node_source_infos.emplace_back();
                g.node_construction_order.push_back(next_construction_order++);
                size_t const broadcast_node = g.nodes.size() - 1;

                for (size_t in_port = 0; in_port < edges_to_expand.size(); ++in_port)
                {
                    GraphEventEdge const& to_rewire = edges_to_expand[in_port];
                    g.event_edges.erase(to_rewire);
                    g.event_edges.insert(GraphEventEdge{ { broadcast_node, in_port }, to_rewire.target, to_rewire.conversion });
                }
                g.event_edges.insert(GraphEventEdge{
                    { node, out_port },
                    { broadcast_node, 0 },
                    EventConversionPlan{}
                });
            }
        }
    }

    constexpr void stub_lowered_dangling_ports(ExecutableGraphData& g, size_t num_public_inputs, std::string_view builder_id)
    {
        auto [connected_sample_outputs, connected_event_outputs] =
            make_lowered_connected_output_sets(g);
        size_t next_construction_order = g.node_construction_order.empty()
            ? 0
            : (*std::ranges::max_element(g.node_construction_order) + 1);

        size_t const num_nodes = g.nodes.size();
        for (size_t node_id = 0; node_id < num_nodes + 1; ++node_id)
        {
            size_t const node = (node_id == num_nodes) ? GRAPH_ID : node_id;
            size_t const num_outputs = (node == GRAPH_ID) ? num_public_inputs : get_num_outputs(g.nodes[node]);
            size_t const num_event_outputs = (node == GRAPH_ID) ? 0 : get_num_event_outputs(g.nodes[node]);

            for (size_t output_port = 0; output_port < num_outputs; ++output_port)
            {
                ConcretePortId const this_port{ node, output_port };
                if (!connected_sample_outputs.contains(this_port))
                {
                    g.nodes.push_back(materialize_generated_node_or_forbid(
                        DummySinkNodeSpec{}));
                    g.explicit_ttl_samples.push_back(std::nullopt);
                    g.node_ids.push_back(lowered_generated_node_id(builder_id, g.node_ids.size()));
                    g.node_virtual_ids.emplace_back();
                    g.node_source_infos.emplace_back();
                    g.node_construction_order.push_back(next_construction_order++);
                    size_t const new_node = g.nodes.size() - 1;
                    g.edges.insert(GraphEdge{ this_port, { new_node, 0 } });
                }
            }

            for (size_t output_port = 0; output_port < num_event_outputs; ++output_port)
            {
                ConcretePortId const this_port{ node, output_port };
                if (!connected_event_outputs.contains(this_port))
                {
                    g.nodes.push_back(materialize_generated_node_or_forbid(
                        DummyEventSinkNodeSpec{}));
                    g.explicit_ttl_samples.push_back(std::nullopt);
                    g.node_ids.push_back(lowered_generated_node_id(builder_id, g.node_ids.size()));
                    g.node_virtual_ids.emplace_back();
                    g.node_source_infos.emplace_back();
                    g.node_construction_order.push_back(next_construction_order++);
                    size_t const new_node = g.nodes.size() - 1;
                    EventTypeId const source_type = get_event_outputs(g.nodes[node])[output_port].type;
                    g.event_edges.insert(GraphEventEdge{
                        this_port,
                        { new_node, 0 },
                        EventConversionRegistry::instance().plan(source_type, EventTypeId::empty)
                    });
                }
            }
        }
    }

    constexpr void validate_lowered_graph(ExecutableGraphData const& g, size_t num_public_inputs, size_t num_public_outputs)
    {
        for (auto const& edge : g.edges)
        {
            size_t source_num_outputs = (edge.source.node == GRAPH_ID)
                ? num_public_inputs
                : get_num_outputs(g.nodes[edge.source.node]);

            size_t target_num_inputs = (edge.target.node == GRAPH_ID)
                ? num_public_outputs
                : get_num_inputs(g.nodes[edge.target.node]);

            if (edge.source.port >= source_num_outputs) {
                error("bad connection: source output port out of range");
            }
            if (edge.target.port >= target_num_inputs) {
                error("bad connection: target input port out of range");
            }
        }
    }

    constexpr void resolve_lowered_sample_edge_conversions(
        ExecutableGraphData& g,
        std::span<InputConfig const> public_inputs,
        std::span<OutputConfig const> public_outputs)
    {
        auto output_layout_for = [&](ConcretePortId port) {
            return port.node == GRAPH_ID
                ? effective_channel_layout(public_inputs[port.port])
                : effective_channel_layout(g.nodes[port.node].outputs()[port.port]);
        };
        auto input_layout_for = [&](ConcretePortId port) {
            return port.node == GRAPH_ID
                ? effective_channel_layout(public_outputs[port.port])
                : effective_channel_layout(g.nodes[port.node].inputs()[port.port]);
        };

        std::vector<GraphEdge> resolved_edges;
        for (GraphEdge const& edge : g.edges) {
            resolved_edges.push_back(
                GraphEdge{edge.source, edge.target,
                    ChannelConversionRegistry::plan(
                        output_layout_for(edge.source), input_layout_for(edge.target))});
        }
        std::ranges::sort(resolved_edges);
        resolved_edges.erase(
            std::ranges::unique(resolved_edges).begin(),
            resolved_edges.end());
        g.edges = std::flat_set<GraphEdge>(
            std::sorted_unique, resolved_edges.begin(), resolved_edges.end());
    }

    constexpr bool lowered_has_path(
        std::vector<std::flat_set<size_t>> const& outgoing,
        size_t start,
        size_t goal)
    {
        if (start == goal) {
            return true;
        }

        std::vector<bool> seen(outgoing.size(), false);
        std::vector<size_t> q;
        q.push_back(start);
        seen[start] = true;

        size_t q_index = 0;
        while (q_index < q.size())
        {
            size_t u = q[q_index++];

            for (size_t v : outgoing[u])
            {
                if (seen[v]) continue;
                if (v == goal) return true;
                seen[v] = true;
                q.push_back(v);
            }
        }

        return false;
    }

    constexpr void validate_lowered_detached_edges(ExecutableGraphData const& g, std::string_view builder_id)
    {
        if (g.detached_info_by_source.empty()) return;

        size_t const num_nodes = g.nodes.size();

        std::vector<std::flat_set<size_t>> explicit_outgoing(num_nodes);
        details::ConstexprHashMap<ConcretePortId, std::vector<ConcretePortId>,
            details::ConcretePortIdHash> consumers_of_output;

        for (GraphEdge const& edge : g.edges)
        {
            consumers_of_output[edge.source].push_back(edge.target);

            if (edge.source.node == GRAPH_ID) continue;
            if (edge.target.node == GRAPH_ID) continue;

            explicit_outgoing[edge.source.node].insert(edge.target.node);
        }

        for (auto const& [_, info] : g.detached_info_by_source)
        {
            if (info.original_source.node == GRAPH_ID) {
                continue;
            }

            auto const* it = consumers_of_output.find(info.reader_output);
            if (!it) {
                continue;
            }

            for (ConcretePortId target_port : *it)
            {
                if (target_port.node == GRAPH_ID) {
                    continue;
                }

                size_t const u = info.original_source.node;
                size_t const v = target_port.node;

                if (!lowered_has_path(explicit_outgoing, v, u)) {
                    error(
                        "builder " + std::string(builder_id) + ": detach() on " +
                        "signal " + std::to_string(info.original_source.node) + ":" + std::to_string(info.original_source.port) +
                        " breaks an acyclic dependency"
                    );
                }
            }
        }
    }
} // namespace details

class GraphLowerer {
  friend struct details::GraphLowererTestAccess;

  struct RuntimeSampleBindingRef {
    std::string binding_id{};
    size_t source_channel_offset = 0;
  };

  GraphBuilderIdentity const& identity;
  GraphBuilderNodeBundles const& bundles;
  GraphBuilderConnections const& connections;
  GraphBuilderPublicPorts const& public_ports;
  GraphBuilderVirtualNodes const& virtuals;
  GraphBuilderDetach const& detach;
  bool execution_root = false;
  details::LoweringWorkspace& out;
  GraphBuilderVirtualPorts virtual_ports;
  details::ConstexprHashMap<NodeBundleHandle, size_t,
      details::ConstexprIdentityHash> subgraph_by_boundary;
  details::ConstexprHashMap<NodeBundlePortId, TopologyPortId,
      details::NodeBundlePortIdHash> materialized_event_output_ports;
  ExecutableGraphData graph{
      .nodes = {}, .explicit_ttl_samples = {}, .node_ids = {},
      .node_virtual_ids = {}, .node_source_infos = {},
      .node_construction_order = {}, .node_kinds = {},
      .node_type_identities = {}, .edges = {}, .event_edges = {},
      .detached_info_by_source = {}, .detached_reader_outputs = {},
  };
  std::vector<size_t> runtime_node_indices;
  details::ConstexprHashMap<TopologyPortId, TopologyPortId, TopologyPortIdHash>
      source_of;
  details::ConstexprHashMap<TopologyPortId, TopologyEventEdge, TopologyPortIdHash>
      event_source_of;
  details::ConstexprHashMap<NodeBundlePortId, RuntimeSampleBindingRef,
      details::NodeBundlePortIdHash>
      runtime_binding_by_semantic_target;
  details::ConstexprHashMap<TopologyPortId, RuntimeSampleBindingRef,
      TopologyPortIdHash>
      runtime_binding_by_concrete_target;
  std::vector<GraphEdge> sample_edge_batch;
  std::vector<GraphEventEdge> event_edge_batch;

  constexpr size_t topology_node_count() const {
    return out.topology_nodes.size();
  }

  constexpr ConcreteNode const& topology_generated_node(size_t index) const {
    return std::get<ConcreteNode>(out.topology_nodes.at(index));
  }
  constexpr AuthoredConcreteNodeRef const& topology_authored_node(
      size_t index) const {
    return std::get<AuthoredConcreteNodeRef>(out.topology_nodes.at(index));
  }
  constexpr bool topology_is_authored_concrete_node(size_t index) const {
    return std::holds_alternative<AuthoredConcreteNodeRef>(
        out.topology_nodes.at(index));
  }
  constexpr NodePorts const& topology_concrete_ports(size_t index) const {
    if (topology_is_authored_concrete_node(index)) {
      return bundles.typed_ports(
          topology_authored_node(index).node_bundle_handle);
    }
    return topology_generated_node(index).ports;
  }
  constexpr NodeLifetime const& topology_concrete_lifetime(size_t index) const {
    if (topology_is_authored_concrete_node(index)) {
      return bundles.concrete_lifetime(
          topology_authored_node(index).node_bundle_handle);
    }
    return topology_generated_node(index).lifetime;
  }
  constexpr SubgraphNode& topology_subgraph_node(size_t index) {
    return std::get<SubgraphNode>(out.topology_nodes.at(index));
  }
  constexpr SubgraphNode const& topology_subgraph_node(size_t index) const {
    return std::get<SubgraphNode>(out.topology_nodes.at(index));
  }
  constexpr bool topology_is_subgraph_node(size_t index) const {
    return std::holds_alternative<SubgraphNode>(out.topology_nodes.at(index));
  }
  constexpr size_t append_topology_node(ConcreteNode node) {
    out.topology_nodes.emplace_back(std::move(node));
    return out.topology_nodes.size() - 1;
  }
  constexpr size_t append_topology_node(AuthoredConcreteNodeRef node) {
    out.topology_nodes.emplace_back(node);
    return out.topology_nodes.size() - 1;
  }
  constexpr size_t append_topology_node(SubgraphNode node) {
    out.topology_nodes.emplace_back(std::move(node));
    return out.topology_nodes.size() - 1;
  }
  constexpr TopologyPortId append_scope_sample_input(OutputConfig) {
    return {GRAPH_ID - 1 - out.scope_boundary_port_count++, 0};
  }
  constexpr TopologyPortId append_scope_event_input(EventOutputConfig) {
    return {GRAPH_ID - 1 - out.scope_boundary_port_count++, 0};
  }
  constexpr void normalize_sample_edges() {
    if (!out.pending_topology_edges.empty()) {
      out.topology_edges.insert_range(out.pending_topology_edges);
      out.pending_topology_edges.clear();
    }
  }
  constexpr void normalize_event_edges() {
    if (!out.pending_topology_event_edges.empty()) {
      out.topology_event_edges.insert_range(out.pending_topology_event_edges);
      out.pending_topology_event_edges.clear();
    }
  }
  constexpr void normalize_topology_edges() {
    normalize_sample_edges();
    normalize_event_edges();
  }
    constexpr void replace_sample_ports(
        std::span<std::pair<TopologyPortId, TopologyPortId> const> sources,
        std::span<std::pair<TopologyPortId, TopologyPortId> const> targets) {
        normalize_sample_edges();
    using TopologyPortReplacements =
        details::ConstexprHashMap<TopologyPortId, TopologyPortId, TopologyPortIdHash>;
    auto make_replacements = [](auto const& replacements) {
      TopologyPortReplacements result;
      for (auto const& [from, to] : replacements)
        result.try_emplace(from, to);
      return result;
    };
    auto const source_replacements = make_replacements(sources);
    auto const target_replacements = make_replacements(targets);
    auto replacement_for = [](
        TopologyPortReplacements const& replacements, TopologyPortId port) {
      auto const* replacement = replacements.find(port);
      if (!replacement) return std::optional<TopologyPortId>{};
      return std::optional{*replacement};
    };
    auto replaced = std::move(out.topology_edges).extract();
    for (auto& edge : replaced) {
      if (auto source = replacement_for(source_replacements, edge.source))
        edge.source = *source;
      if (auto target = replacement_for(target_replacements, edge.target))
        edge.target = *target;
    }
    std::ranges::sort(replaced);
    replaced.erase(std::ranges::unique(replaced).begin(), replaced.end());
    out.topology_edges.replace(std::move(replaced));
  }
    constexpr void replace_event_ports(
        std::span<std::pair<TopologyPortId, TopologyPortId> const> sources,
        std::span<std::pair<TopologyPortId, TopologyPortId> const> targets) {
        normalize_event_edges();
    using TopologyPortReplacements =
        details::ConstexprHashMap<TopologyPortId, TopologyPortId, TopologyPortIdHash>;
    auto make_replacements = [](auto const& replacements) {
      TopologyPortReplacements result;
      for (auto const& [from, to] : replacements)
        result.try_emplace(from, to);
      return result;
    };
    auto const source_replacements = make_replacements(sources);
    auto const target_replacements = make_replacements(targets);
    auto replacement_for = [](
        TopologyPortReplacements const& replacements, TopologyPortId port) {
      auto const* replacement = replacements.find(port);
      if (!replacement) return std::optional<TopologyPortId>{};
      return std::optional{*replacement};
    };
    auto replaced = std::move(out.topology_event_edges).extract();
    for (auto& edge : replaced) {
      if (auto source = replacement_for(source_replacements, edge.source))
        edge.source = *source;
      if (auto target = replacement_for(target_replacements, edge.target))
        edge.target = *target;
    }
    std::ranges::sort(replaced);
    replaced.erase(std::ranges::unique(replaced).begin(), replaced.end());
    out.topology_event_edges.replace(std::move(replaced));
  }
  template<class Fn>
  constexpr void for_each_sample_edge(Fn&& fn) {
    normalize_sample_edges();
    for (auto const& edge : out.topology_edges) fn(edge);
  }
  template<class Fn>
  constexpr void for_each_sample_edge(Fn&& fn) const {
    for (auto const& edge : out.topology_edges) fn(edge);
  }
  template<class Fn>
  constexpr void for_each_event_edge(Fn&& fn) {
    normalize_event_edges();
    for (auto const& edge : out.topology_event_edges) fn(edge);
  }
  template<class Fn>
  constexpr void for_each_event_edge(Fn&& fn) const {
    for (auto const& edge : out.topology_event_edges) fn(edge);
  }
  constexpr size_t append_lowered_subgraph_node(
      std::string kind, std::vector<InputConfig> inputs,
      std::vector<OutputConfig> outputs,
      std::vector<EventInputConfig> event_inputs,
      std::vector<EventOutputConfig> event_outputs, size_t begin, size_t count,
      std::vector<std::vector<TopologyPortId>> sample_input_targets,
      std::vector<TopologyPortId> sample_output_sources,
      std::vector<std::vector<TopologyPortId>> event_input_targets,
      std::vector<TopologyPortId> event_output_sources) {
    auto const type_identity = "lowered-subgraph:" + kind;
    return append_topology_node(SubgraphNode{
        .ports = NodePorts{.sample_inputs = std::move(inputs),
                           .sample_outputs = std::move(outputs),
                           .event_input_configs = std::move(event_inputs),
                           .event_output_configs = std::move(event_outputs)},
        .lifetime = NodeLifetime{.ttl_samples = std::nullopt},
        .lowered_subgraph = LoweredSubgraphBinding{
            .begin = begin, .count = count,
            .sample_input_targets = std::move(sample_input_targets),
            .sample_output_sources = std::move(sample_output_sources),
            .event_input_targets = std::move(event_input_targets),
            .event_output_sources = std::move(event_output_sources),
            .kind = std::move(kind)},
        .type_identity = NodeTypeIdentity{.value = type_identity},
    });
  }

  constexpr size_t append_generated(ConcreteNode node) {
    auto const index = append_topology_node(std::move(node));
    if (out.bundle_by_lowered_node.size() <= index)
      out.bundle_by_lowered_node.resize(index + 1);
    out.bundle_by_lowered_node[index] = std::nullopt;
    return index;
  }

  constexpr void add_sample_edge(TopologyEdge edge) {
    if (auto const* boundary = out.subgraph_input_of_boundary_source.find(edge.source)) {
      auto& targets = topology_subgraph_node(boundary->node)
                          .lowered_subgraph.sample_input_targets.at(
                              boundary->port);
      if (!std::ranges::contains(targets, edge.target))
        targets.push_back(edge.target);
    }
    out.pending_topology_edges.push_back(std::move(edge));
  }

  constexpr void add_event_edge(TopologyEventEdge edge) {
    if (auto const* boundary = out.subgraph_event_input_of_boundary_source.find(
            edge.source)) {
      auto& targets = topology_subgraph_node(boundary->node)
                          .lowered_subgraph.event_input_targets.at(
                              boundary->port);
      if (!std::ranges::contains(targets, edge.target))
        targets.push_back(edge.target);
    }
    out.pending_topology_event_edges.push_back(std::move(edge));
  }

  constexpr void project_bundles() {
    out.bundle_projections.resize(bundles.size());
    auto const root_boundary = public_ports.boundary_handle();
    for (NodeBundleHandle handle=0; handle<bundles.size(); ++handle) {
      auto const& bundle = bundles.bundle(handle);
      auto& p = out.bundle_projections[handle];
      if (bundle.is_concrete()) {
        auto node = append_topology_node(
            AuthoredConcreteNodeRef{.node_bundle_handle = handle});
        p.topology_node = node;
        if (out.bundle_by_lowered_node.size() <= node) out.bundle_by_lowered_node.resize(node+1);
        out.bundle_by_lowered_node[node] = handle;
        for(size_t i=0;i<bundle.sample_input_count();++i)p.sample_inputs.push_back({{node,i}});
        for(size_t i=0;i<bundle.sample_output_count();++i)p.sample_outputs.push_back({{node,i}});
        for(size_t i=0;i<bundle.event_input_count();++i)p.event_inputs.push_back({{node,i}});
        for(size_t i=0;i<bundle.event_output_count();++i)p.event_outputs.push_back({{node,i}});
      } else if (bundle.is_tiled()) {
        auto members = bundle.tiled_members();
        for (auto const member : members) {
          auto const member_node = out.bundle_projections.at(member).topology_node;
          if (!member_node) details::error("tiled member has no lowered concrete node");
          out.bundle_by_lowered_node.at(*member_node) = handle;
        }
        for(size_t port=0;port<bundle.sample_input_count();++port){
          std::vector<TopologyPortId> endpoints;
          for(auto member:members) endpoints.push_back(out.bundle_projections.at(member).sample_inputs.at(port).at(0));
          p.sample_inputs.push_back(std::move(endpoints));
        }
        for(size_t port=0;port<bundle.sample_output_count();++port){
          std::vector<TopologyPortId> endpoints;
          for(auto member:members) endpoints.push_back(out.bundle_projections.at(member).sample_outputs.at(port).at(0));
          p.sample_outputs.push_back(std::move(endpoints));
        }
        for(size_t port=0;port<bundle.event_input_count();++port){
          std::vector<TopologyPortId> endpoints;
          for(auto member:members) endpoints.push_back(out.bundle_projections.at(member).event_inputs.at(port).at(0));
          p.event_inputs.push_back(std::move(endpoints));
        }
        for(size_t port=0;port<bundle.event_output_count();++port){
          std::vector<TopologyPortId> endpoints;
          for(auto member:members) endpoints.push_back(out.bundle_projections.at(member).event_outputs.at(port).at(0));
          p.event_outputs.push_back(std::move(endpoints));
        }
      } else if (bundle.is_boundary()) {
        if (handle == root_boundary) {
          for(size_t i=0;i<bundle.sample_input_count();++i)p.sample_inputs.push_back({{GRAPH_ID,i}});
          for(size_t i=0;i<bundle.sample_output_count();++i)p.sample_outputs.push_back({{GRAPH_ID,i}});
          for(size_t i=0;i<bundle.event_input_count();++i)p.event_inputs.push_back({{GRAPH_ID,i}});
          for(size_t i=0;i<bundle.event_output_count();++i)p.event_outputs.push_back({{GRAPH_ID,i}});
        } else {
          p.sample_inputs.resize(bundle.sample_input_count());
          p.event_inputs.resize(bundle.event_input_count());
          for(size_t i=0;i<bundle.sample_output_count();++i) {
            auto config=bundles.resolve_sample_output({handle,PortKind::sample,i}).config;
            p.sample_outputs.push_back({append_scope_sample_input(config)});
          }
          for(size_t i=0;i<bundle.event_output_count();++i) {
            auto config=bundles.resolve_event_output({handle,PortKind::event,i}).config;
            p.event_outputs.push_back({append_scope_event_input(config)});
          }
        }
      } else if (bundle.is_subgraph()) {
        auto info=bundles.subgraph_info(handle);
        auto const& boundary=bundles.bundle(info.boundary);
        size_t begin=topology_node_count(), end=begin;
        bool found=false;
        for(size_t child=info.child_begin; child<info.child_begin+info.child_count; ++child) {
          if (auto n=out.bundle_projections[child].topology_node) {
            if(!found){begin=*n;found=true;} end=std::max(end,*n+1);
          }
        }
        if(!found) begin=topology_node_count(), end=begin;
        auto node=append_lowered_subgraph_node(
            info.kind,
            std::vector<InputConfig>(boundary.boundary_sample_inputs().begin(),boundary.boundary_sample_inputs().end()),
            std::vector<OutputConfig>(boundary.boundary_sample_outputs().begin(),boundary.boundary_sample_outputs().end()),
            std::vector<EventInputConfig>(boundary.boundary_event_inputs().begin(),boundary.boundary_event_inputs().end()),
            std::vector<EventOutputConfig>(boundary.boundary_event_outputs().begin(),boundary.boundary_event_outputs().end()),
            begin,end-begin,
            std::vector<std::vector<TopologyPortId>>(boundary.boundary_sample_inputs().size()),
            std::vector<TopologyPortId>(boundary.boundary_sample_outputs().size()),
            std::vector<std::vector<TopologyPortId>>(boundary.boundary_event_inputs().size()),
            std::vector<TopologyPortId>(boundary.boundary_event_outputs().size()));
        topology_subgraph_node(node).lifetime=info.lifetime;
        p.topology_node=node;
        if(out.bundle_by_lowered_node.size()<=node)out.bundle_by_lowered_node.resize(node+1);
        out.bundle_by_lowered_node[node]=handle;
        for(size_t i=0;i<bundle.sample_input_count();++i)p.sample_inputs.push_back({{node,i}});
        for(size_t i=0;i<bundle.sample_output_count();++i)p.sample_outputs.push_back({{node,i}});
        for(size_t i=0;i<bundle.event_input_count();++i)p.event_inputs.push_back({{node,i}});
        for(size_t i=0;i<bundle.event_output_count();++i)p.event_outputs.push_back({{node,i}});
        if (subgraph_by_boundary.contains(info.boundary))
          details::error("boundary belongs to more than one subgraph");
        subgraph_by_boundary.try_emplace(info.boundary, node);
        auto const& bp=out.bundle_projections[info.boundary];
        for(size_t i=0;i<bp.sample_outputs.size();++i)
          if(!bp.sample_outputs[i].empty())out.subgraph_input_of_boundary_source.try_emplace(bp.sample_outputs[i].front(),TopologyPortId{node,i});
        for(size_t i=0;i<bp.event_outputs.size();++i)
          if(!bp.event_outputs[i].empty())out.subgraph_event_input_of_boundary_source.try_emplace(bp.event_outputs[i].front(),TopologyPortId{node,i});
      }
    }
  }

  struct ResolvedSampleSourceChannel {
    TopologyPortId port{};
    OutputConfig config{};
    size_t channel = 0;
  };

  struct ResolvedSampleTargetChannel {
    std::optional<TopologyPortId> port{};
    InputConfig config{};
    size_t channel = 0;
  };

  constexpr ResolvedSampleSourceChannel resolve_sample_source_channel(
      SampleOutputChannelId channel) const {
    NodeBundlePortId logical{channel.bundle, PortKind::sample, channel.port};
    auto const config = bundles.resolve_sample_output(logical).config;
    auto const count = channel_count(config.channel_layout.channel_type);
    auto const& endpoints = out.bundle_projections.at(channel.bundle).sample_outputs.at(channel.port);
    if (channel.channel >= count) details::error("sample source channel out of bounds");
    if (endpoints.size() == 1) return {endpoints.front(), config, channel.channel};
    if (endpoints.size() == count && bundles.bundle(channel.bundle).is_tiled()) {
      auto const member = bundles.tiled_member(channel.bundle, channel.channel);
      auto const member_config = bundles.resolve_sample_output({member, PortKind::sample, channel.port}).config;
      if (channel_count(member_config.channel_layout.channel_type) != 1)
        details::error("tiled sample source member must expose one concrete channel");
      return {endpoints[channel.channel], member_config, 0};
    }
    details::error("sample source has invalid lowered endpoint count");
  }

  constexpr ResolvedSampleTargetChannel resolve_sample_target_channel(
      SampleInputChannelId channel) const {
    NodeBundlePortId logical{channel.bundle, PortKind::sample, channel.port};
    auto const config = bundles.resolve_sample_input(logical).config;
    auto const count = channel_count(config.channel_layout.channel_type);
    auto const& endpoints = out.bundle_projections.at(channel.bundle).sample_inputs.at(channel.port);
    if (channel.channel >= count) details::error("sample target channel out of bounds");
    if (endpoints.empty()) {
      if (!subgraph_by_boundary.contains(channel.bundle)) details::error("sample target has no lowered endpoint");
      return {std::nullopt, config, channel.channel};
    }
    if (endpoints.size() == 1) return {endpoints.front(), config, channel.channel};
    if (endpoints.size() == count && bundles.bundle(channel.bundle).is_tiled()) {
      auto const member = bundles.tiled_member(channel.bundle, channel.channel);
      auto const member_config = bundles.resolve_sample_input({member, PortKind::sample, channel.port}).config;
      if (channel_count(member_config.channel_layout.channel_type) != 1)
        details::error("tiled sample target member must expose one concrete channel");
      return {endpoints[channel.channel], member_config, 0};
    }
    details::error("sample target has invalid lowered endpoint count");
  }

  constexpr std::optional<TopologyPortId> direct_sample_source(
      ChannelTypeId type, std::span<SampleOutputChannelId const> channels) const {
    if (channels.empty()) details::error("sample source has no channels");
    auto const logical = bundles.sample_output_port_for_channels(type, channels);
    if (!logical) return std::nullopt;
    auto const& endpoints = out.bundle_projections.at(
        logical->node_bundle_handle).sample_outputs.at(logical->port_ordinal);
    if (endpoints.size() == 1) return endpoints.front();
    if (type == ChannelTypeId::mono && channels.size() == 1) {
      auto const resolved = resolve_sample_source_channel(channels.front());
      if (resolved.channel == 0 && channel_count(resolved.config.channel_layout.channel_type) == 1)
        return resolved.port;
    }
    return std::nullopt;
  }

  constexpr ConcreteNode make_connection_node(
      std::vector<ConnectionNodeInputConfig> inputs,
      std::vector<ConnectionNodeEphemeralPortConfig> ephemeral_ports,
      OutputConfig output, Sample default_value,
      std::string runtime_binding_id = {},
      size_t runtime_source_channel_offset = 0) const {
    ConnectionNodeSpec spec{
        .input_configs = std::move(inputs),
        .ephemeral_port_configs = std::move(ephemeral_ports),
        .output_config = std::move(output),
        .default_value = default_value,
        .runtime_binding_id = std::move(runtime_binding_id),
        .runtime_source_channel_offset = runtime_source_channel_offset,
    };
    return ConcreteNode{
        .ports = details::generated_node_ports(spec),
        .type_identity = NodeTypeIdentity{.value = "iv::ConnectionNode"},
        .generated_node = std::move(spec),
    };
  }

  template<class Spec>
  constexpr ConcreteNode make_generated_node(Spec spec, std::string type_identity) const {
    return ConcreteNode{
        .ports = details::generated_node_ports(spec),
        .type_identity = NodeTypeIdentity{.value = std::move(type_identity)},
        .generated_node = std::move(spec),
    };
  }

  struct LoweredConnection {
    TopologyPortId output{};
    std::optional<TopologyPortId> target{};
  };

  struct ConnectionNodeTarget {
    std::optional<TopologyPortId> endpoint{};
    InputConfig config{};
    std::optional<size_t> tiled_channel{};
  };

  struct SampleLoweringState {
    size_t authored_topology_node_count = 0;
    details::ConstexprHashSet<NodeBundlePortId, details::NodeBundlePortIdHash>
        assigned_subgraph_outputs;
    // Input ports of authored concrete nodes form a dense domain. A bitmap is
    // both the natural representation of this one-pass marking problem and
    // avoids a flat_set insertion for every authored connection.
    std::vector<size_t> input_slot_offsets;
    std::vector<bool> bound_target_slots;
    // Public and subgraph-boundary targets are outside that dense domain.
    // They are few, but still count as bound targets for the lowering pass.
    details::ConstexprHashSet<TopologyPortId, TopologyPortIdHash>
        non_authored_bound_targets;
    size_t bound_target_count = 0;
  };

  constexpr void index_runtime_sample_bindings() {
    for (auto const& input : virtual_ports.sample_inputs) {
      for (size_t member = 0; member < input.node_bundle_ports.size(); ++member) {
        auto const target = input.node_bundle_ports[member];
        if (target.port_kind != PortKind::sample) continue;
        RuntimeSampleBindingRef const binding{
            .binding_id = runtime_virtual_port_key(
                true, PortKind::sample, input.id.virtual_node_id, member,
                input.id.port_ordinal),
            .source_channel_offset = 0,
        };
        runtime_binding_by_semantic_target.emplace(target, binding);
        auto const& endpoints = out.bundle_projections.at(
            target.node_bundle_handle).sample_inputs.at(target.port_ordinal);
        for (size_t channel = 0; channel < endpoints.size(); ++channel) {
          auto concrete_binding = binding;
          concrete_binding.source_channel_offset = channel;
          runtime_binding_by_concrete_target.emplace(
              endpoints[channel], std::move(concrete_binding));
        }
      }
    }
  }

  constexpr RuntimeSampleBindingRef runtime_sample_binding_for(
      NodeBundlePortId semantic_target,
      std::optional<TopologyPortId> concrete_target) const {
    if (concrete_target) {
      if (auto const* it = runtime_binding_by_concrete_target.find(
              *concrete_target))
        return *it;
    }
    if (auto const* it = runtime_binding_by_semantic_target.find(semantic_target))
      return *it;
    return {};
  }

  constexpr bool has_runtime_sample_capability(
      NodeBundlePortId semantic_target,
      std::optional<TopologyPortId> concrete_target) const {
    return (concrete_target &&
            runtime_binding_by_concrete_target.contains(*concrete_target)) ||
           runtime_binding_by_semantic_target.contains(semantic_target);
  }

  constexpr LoweredConnection lower_connection_node(
      SampleLoweringGroup const& group,
      ConnectionNodeTarget target) {
    std::vector<TopologyPortId> input_sources;
    std::vector<ConnectionNodeInputConfig> input_configs;
    std::vector<ConnectionNodeEphemeralPortConfig> ephemeral_configs;
    auto projection_input = [&](ResolvedSampleSourceChannel const& source) {
      auto it = std::ranges::find(input_sources, source.port);
      if (it != input_sources.end()) return static_cast<size_t>(it - input_sources.begin());
      input_sources.push_back(source.port);
      input_configs.push_back(ConnectionNodeInputConfig{.input = InputConfig{.channel_layout = source.config.channel_layout}});
      return input_sources.size() - 1;
    };
    for (auto const* connection : group.connections) {
      if (connection->source_channels.size() != channel_count(connection->source_type))
        details::error("sample source channel count does not match its semantic type");
      if (connection->target_channels.size() != channel_count(connection->target_type))
        details::error("sample target channel count does not match its semantic type");
      auto const ephemeral = ephemeral_configs.size();
      ConnectionNodeEphemeralPortConfig ephemeral_config{
          .channel_layout = ChannelLayout{.channel_type = connection->source_type, .sample_layout = SampleStreamLayout::planar},
          .conversion = ChannelConversionRegistry::plan(
              ChannelLayout{.channel_type = connection->source_type, .sample_layout = SampleStreamLayout::planar},
              ChannelLayout{.channel_type = connection->target_type, .sample_layout = SampleStreamLayout::planar}),
      };
      for (size_t source_i = 0; source_i < connection->source_channels.size(); ++source_i) {
        auto const source = resolve_sample_source_channel(connection->source_channels[source_i]);
        auto const input = projection_input(source);
        input_configs[input].channel_copies.push_back({.input_channel = source.channel, .ephemeral_port = ephemeral, .ephemeral_channel = source_i});
      }
      for (size_t target_i = 0; target_i < connection->target_channels.size(); ++target_i) {
        auto const target_channel = connection->target_channels[target_i];
        if (target.tiled_channel && target_channel.channel != *target.tiled_channel)
          continue;
        ephemeral_config.output_channel_copies.push_back(
            {.converted_channel = target_i,
             .output_channel = target.tiled_channel ? 0 : target_channel.channel});
      }
      if (!ephemeral_config.output_channel_copies.empty())
        ephemeral_configs.push_back(std::move(ephemeral_config));
      else
        for (auto& input : input_configs)
          std::erase_if(input.channel_copies, [&](auto const& copy) { return copy.ephemeral_port == ephemeral; });
    }
    std::vector<TopologyPortId> used_input_sources;
    std::vector<ConnectionNodeInputConfig> used_input_configs;
    for (size_t input = 0; input < input_configs.size(); ++input) {
      if (input_configs[input].channel_copies.empty()) continue;
      used_input_sources.push_back(input_sources[input]);
      used_input_configs.push_back(std::move(input_configs[input]));
    }
    auto const runtime_binding = runtime_sample_binding_for(
        group.target, target.endpoint);
    auto const node = append_generated(make_connection_node(
        std::move(used_input_configs), std::move(ephemeral_configs),
        OutputConfig{.name = target.config.name,
                     .channel_layout = target.config.channel_layout},
        target.config.default_value, runtime_binding.binding_id,
        runtime_binding.source_channel_offset));
    for (size_t input = 0; input < used_input_sources.size(); ++input)
      add_sample_edge({used_input_sources[input], {node, input}});
    TopologyPortId const source{node, 0};
    if (target.endpoint) add_sample_edge({source, *target.endpoint});
    return {.output = source, .target = target.endpoint};
  }

  constexpr bool lower_tiled_group_direct(SampleLoweringGroup const& group, std::span<TopologyPortId const> endpoints) {
    auto const target_channels = bundles.sample_input_channels(group.target);
    auto const target_type = bundles.resolve_sample_input(group.target).config.channel_layout.channel_type;
    if (group.connections.size() == 1) {
      auto const& connection = *group.connections.front();
      bool const whole_target = connection.target_type == target_type && connection.target_channels == target_channels;
      if (whole_target && connection.source_type == ChannelTypeId::mono) {
        if (auto source = direct_sample_source(connection.source_type, connection.source_channels)) {
          for (auto const target : endpoints) add_sample_edge({*source, target});
          return true;
        }
      }
      if (whole_target && connection.source_type == target_type && connection.source_channels.size() == endpoints.size()) {
        std::vector<TopologyPortId> sources;
        sources.reserve(connection.source_channels.size());
        for (auto const source_channel : connection.source_channels) {
          auto const resolved = resolve_sample_source_channel(source_channel);
          if (resolved.channel != 0 || channel_count(resolved.config.channel_layout.channel_type) != 1) return false;
          sources.push_back(resolved.port);
        }
        for (size_t channel = 0; channel < endpoints.size(); ++channel)
          add_sample_edge({sources[channel], endpoints[channel]});
        return true;
      }
    }
    std::vector<std::pair<TopologyPortId, TopologyPortId>> edges;
    std::vector<bool> claimed(endpoints.size(), false);
    for (auto const* connection : group.connections) {
      if (connection->target_type != ChannelTypeId::mono || connection->target_channels.size() != 1) return false;
      auto it = std::ranges::find(target_channels, connection->target_channels.front());
      if (it == target_channels.end()) details::error("partial target not in tiled input");
      auto const channel = static_cast<size_t>(it - target_channels.begin());
      if (claimed[channel]) details::error("sample target channel has multiple sources");
      auto source = direct_sample_source(connection->source_type, connection->source_channels);
      if (!source) return false;
      claimed[channel] = true;
      edges.push_back({*source, endpoints[channel]});
    }
    for (auto const& [source, target] : edges) add_sample_edge({source, target});
    return !edges.empty();
  }

  constexpr SampleLoweringPlan plan_sample_lowering() const {
    return connections.sample_lowering_plan();
  }

  constexpr SampleLoweringState lower_connected_sample_groups(
      SampleLoweringPlan const& plan) {
    SampleLoweringState state{
        .authored_topology_node_count = topology_node_count(),
        .assigned_subgraph_outputs = {},
        .input_slot_offsets = {},
        .bound_target_slots = {},
        .non_authored_bound_targets = {},
        .bound_target_count = 0,
    };
    state.input_slot_offsets.resize(state.authored_topology_node_count + 1);
    size_t input_slot_count = 0;
    for (size_t node = 0; node < state.authored_topology_node_count; ++node) {
      state.input_slot_offsets[node] = input_slot_count;
      if (!topology_is_subgraph_node(node)) {
        input_slot_count +=
            topology_concrete_ports(node).sample_inputs.size();
      }
    }
    state.input_slot_offsets[state.authored_topology_node_count] =
        input_slot_count;
    state.bound_target_slots.assign(input_slot_count, false);
    auto mark_bound = [&](TopologyPortId target) {
      if (target.node >= state.authored_topology_node_count ||
          topology_is_subgraph_node(target.node)) {
        if (state.non_authored_bound_targets.insert(target))
          ++state.bound_target_count;
        return;
      }
      if (target.port >=
          topology_concrete_ports(target.node).sample_inputs.size()) {
        details::error("sample target is outside its authored input range");
      }
      auto const slot = state.input_slot_offsets[target.node] + target.port;
      if (slot >= state.bound_target_slots.size())
        details::error("sample target is outside its authored input range");
      if (!state.bound_target_slots[slot]) {
        state.bound_target_slots[slot] = true;
        ++state.bound_target_count;
      }
    };
    for (auto const& group : plan.groups) {
      auto const& endpoints = out.bundle_projections.at(group.target.node_bundle_handle).sample_inputs.at(group.target.port_ordinal);
      auto const target_channels = bundles.sample_input_channels(group.target);
      auto const target_config = bundles.resolve_sample_input(group.target).config;
      auto const target_type = target_config.channel_layout.channel_type;
      std::vector<ResolvedSampleTargetChannel> resolved_target_channels;
      resolved_target_channels.reserve(target_channels.size());
      for (auto const channel : target_channels)
        resolved_target_channels.push_back(resolve_sample_target_channel(channel));
      for (auto const* connection : group.connections)
        for (auto const channel : connection->target_channels)
          if (!std::ranges::contains(target_channels, channel))
            details::error("sample connection target does not belong to its group");
      if (auto const* it = subgraph_by_boundary.find(group.target.node_bundle_handle)) {
        auto const subgraph_node = *it;
        if (group.target.port_ordinal >= topology_subgraph_node(subgraph_node).lowered_subgraph.sample_output_sources.size())
          details::error("subgraph sample output out of bounds");
        if (!state.assigned_subgraph_outputs.insert(group.target))
          details::error("subgraph sample output has more than one source");
        std::optional<TopologyPortId> source;
        if (group.connections.size() == 1) {
          auto const& connection = *group.connections.front();
          if (connection.target_type == target_type && connection.target_channels == target_channels)
            source = direct_sample_source(connection.source_type, connection.source_channels);
        }
        if (!source)
          source = lower_connection_node(
              group, {.endpoint = std::nullopt, .config = target_config,
                      .tiled_channel = std::nullopt})
                       .output;
        topology_subgraph_node(subgraph_node).lowered_subgraph.sample_output_sources[group.target.port_ordinal] = *source;
        continue;
      }
      if (endpoints.empty()) details::error("sample target has no lowered endpoint");
      if (endpoints.size() == 1) {
        auto const runtime_capable = has_runtime_sample_capability(group.target, endpoints.front());
        if (group.connections.size() == 1) {
          auto const& connection = *group.connections.front();
          if (connection.target_type == target_type && connection.target_channels == target_channels) {
            if (auto source = direct_sample_source(connection.source_type, connection.source_channels)) {
              auto const source_config = resolve_sample_source_channel(connection.source_channels.front()).config;
              if (!runtime_capable && effective_channel_layout(source_config) == effective_channel_layout(target_config)) {
                add_sample_edge({*source, endpoints.front()});
                mark_bound(endpoints.front());
                continue;
              }
            }
          }
        }
        lower_connection_node(
            group, {.endpoint = endpoints.front(), .config = target_config,
                    .tiled_channel = std::nullopt});
        mark_bound(endpoints.front());
        continue;
      }
      auto const runtime_capable = std::ranges::any_of(endpoints, [&](auto endpoint) { return has_runtime_sample_capability(group.target, endpoint); });
      if (!runtime_capable && lower_tiled_group_direct(group, endpoints)) {
        for (auto const endpoint : endpoints) mark_bound(endpoint);
        continue;
      }
      for (size_t channel = 0; channel < endpoints.size(); ++channel) {
        auto const& resolved = resolved_target_channels.at(channel);
        if (resolved.port != endpoints[channel])
          details::error("tiled sample target does not match its projection");
        lower_connection_node(
            group, {.endpoint = endpoints[channel], .config = resolved.config,
                    .tiled_channel = target_channels[channel].channel});
        mark_bound(endpoints[channel]);
      }
    }
    return state;
  }

  constexpr void lower_vacant_sample_inputs(SampleLoweringState& state) {
    auto mark_bound = [&](TopologyPortId target) {
      auto const slot = state.input_slot_offsets[target.node] + target.port;
      if (slot >= state.bound_target_slots.size())
        details::error("vacant sample target is outside its authored input range");
      if (!state.bound_target_slots[slot]) {
        state.bound_target_slots[slot] = true;
        ++state.bound_target_count;
      }
    };
    for (size_t node = 0; node < state.authored_topology_node_count; ++node) {
      if (topology_is_subgraph_node(node)) continue;
      auto const input_count =
          topology_concrete_ports(node).sample_inputs.size();
      for (size_t input = 0; input < input_count; ++input) {
        TopologyPortId const target{node, input};
        auto const slot = state.input_slot_offsets[node] + input;
        if (state.bound_target_slots[slot]) continue;
        SampleLoweringGroup vacant;
        lower_connection_node(
            vacant,
            {.endpoint = target,
             .config = topology_concrete_ports(node).sample_inputs.at(input),
             .tiled_channel = std::nullopt});
        mark_bound(target);
      }
    }
  }

  constexpr void bind_subgraph_sample_inputs(
      SampleLoweringState const& state) {
    for (auto const& [boundary, node] : subgraph_by_boundary) {
      auto& binding = topology_subgraph_node(node).lowered_subgraph;
      for (size_t output = 0; output < binding.sample_output_sources.size(); ++output)
        if (!state.assigned_subgraph_outputs.contains(
                {boundary, PortKind::sample, output}))
          details::error("subgraph sample output has no authored source");
    }
  }

  constexpr TopologyPortId materialize_event_output_port(
      NodeBundlePortId logical, EventTypeId type) {
        if (auto const* existing = materialized_event_output_ports.find(logical))
      return *existing;
    auto const& endpoints = out.bundle_projections.at(
        logical.node_bundle_handle).event_outputs.at(logical.port_ordinal);
    if (endpoints.empty())
      details::error("virtual event output member has no lowered endpoint");
    if (endpoints.size() == 1) return endpoints.front();
    auto const node = append_generated(make_generated_node(
        EventConcatenationNodeSpec{
            .input_count = endpoints.size(), .type = type},
        "iv::EventConcatenation"));
    for (size_t input = 0; input < endpoints.size(); ++input)
      add_event_edge({
          endpoints[input], {node, input},
          EventConversionRegistry::instance().plan(type, type)});
    return materialized_event_output_ports.try_emplace(
        logical, TopologyPortId{node, 0}).value;
  }

  constexpr TopologyPortId materialize_event_source(AuthoredEventConnection const& c) {
    std::vector<TopologyPortId> endpoints;
    for(auto source:c.sources) {
      NodeBundlePortId const logical{
          source.bundle, PortKind::event, source.port};
      auto const config=bundles.resolve_event_output(logical).config;
      if(config.type!=c.source_type)details::error("event source type changed before lowering");
      endpoints.push_back(materialize_event_output_port(logical, c.source_type));
    }
    if(endpoints.empty())details::error("event source has no lowered endpoint");
    if(endpoints.size()==1)return endpoints.front();
    auto node=append_generated(make_generated_node(
        EventConcatenationNodeSpec{
            .input_count = endpoints.size(), .type = c.source_type},
        "iv::EventConcatenation"));
    for(size_t i=0;i<endpoints.size();++i)
      add_event_edge({endpoints[i], {node, i},
                      EventConversionRegistry::instance().plan(
                          c.source_type, c.source_type)});
    return {node,0};
  }

  constexpr void lower_runtime_event_inputs() {
    for (auto const& input : virtual_ports.event_inputs) {
      for (size_t member = 0; member < input.node_bundle_ports.size(); ++member) {
        auto const target = input.node_bundle_ports[member];
        auto const node = append_generated(make_generated_node(
            RuntimeEventInputNodeSpec{.type = input.config.type, .binding_id = runtime_virtual_port_key(true, PortKind::event, input.id.virtual_node_id, member, input.id.port_ordinal)},
            "iv::RuntimeEventInputNode"));
        auto const& endpoints = out.bundle_projections.at(target.node_bundle_handle).event_inputs.at(target.port_ordinal);
        if (endpoints.empty()) details::error("runtime-capable event target has no lowered endpoint");
        for (auto const endpoint : endpoints)
          add_event_edge({{node, 0}, endpoint,
                          EventConversionRegistry::instance().plan(
                              input.config.type, input.config.type)});
      }
    }
  }

  constexpr void lower_events() {
    details::ConstexprHashSet<NodeBundlePortId, details::NodeBundlePortIdHash>
        assigned_subgraph_outputs;
    for(auto const& c:connections.authored_event_connections()) {
      auto source=materialize_event_source(c);
      for(auto target:c.targets) {
        auto config=bundles.resolve_event_input({target.bundle,PortKind::event,target.port}).config;
        if(config.type!=c.target_type)details::error("event target type changed before lowering");
        if(auto const* it=subgraph_by_boundary.find(target.bundle)) {
          NodeBundlePortId const logical{target.bundle, PortKind::event, target.port};
          if (!assigned_subgraph_outputs.insert(logical))
            details::error("subgraph event output has more than one source");
          topology_subgraph_node(*it).lowered_subgraph.event_output_sources.at(target.port)=source;
          continue;
        }
        auto const& endpoints=out.bundle_projections.at(target.bundle).event_inputs.at(target.port);
        if(endpoints.empty())details::error("event target has no lowered endpoint");
        for(auto endpoint:endpoints)
          add_event_edge({source, endpoint,
                          EventConversionRegistry::instance().plan(
                              c.source_type, c.target_type)});
      }
    }
    lower_runtime_event_inputs();
    for(auto const& [boundary,node]:subgraph_by_boundary) {
      auto& binding=topology_subgraph_node(node).lowered_subgraph;
      for (size_t output = 0; output < binding.event_output_sources.size(); ++output)
        if (!assigned_subgraph_outputs.contains(
                {boundary, PortKind::event, output}))
          details::error("subgraph event output has no authored source");
    }
  }

  constexpr void lower_detach() {
    if (detach.authored_infos().empty()) return;
    // This lookup is meaningful only for detach writers. Building it while
    // every ordinary sample edge is added made no-detach graphs maintain a
    // large, repeatedly shifted flat_map for no consumer.
    auto source_for_target = [&](TopologyPortId target)
        -> std::optional<TopologyPortId> {
      for (auto it = out.pending_topology_edges.rbegin();
           it != out.pending_topology_edges.rend(); ++it) {
        if (it->target == target) return it->source;
      }
      for (auto const& edge : out.topology_edges) {
        if (edge.target == target) return edge.source;
      }
      return std::nullopt;
    };
    for(auto const& info:detach.authored_infos()) {
      auto writer=out.bundle_projections.at(info.writer_bundle).topology_node;
      if(!writer)details::error("detach writer is not a concrete lowered node");
      auto const source = source_for_target({*writer, 0});
      if (!source)
        details::error("detach writer has no lowered source");
      auto const reader_channel = resolve_sample_source_channel(info.reader_channel);
      if (reader_channel.channel != 0 || channel_count(reader_channel.config.channel_layout.channel_type) != 1) details::error("detach reader output must be one concrete channel");
      auto const reader = reader_channel.port;
      out.detached_info_by_source.try_emplace(
          *source,
          details::DetachedSamplePortInfo{
          info.detach_id, *source, *writer, reader,
          info.loop_extra_latency});
      out.detached_reader_outputs.insert(reader);
    }
  }

  constexpr TopologyPortId materialize_sample_output_channels(
      ChannelTypeId semantic_type,
      std::span<SampleOutputChannelId const> semantic_channels,
      OutputConfig output_config) {
    if (semantic_channels.size() != channel_count(semantic_type))
      details::error("virtual sample output member channel count changed");
    if (semantic_type != output_config.channel_layout.channel_type)
      details::error("virtual sample output member channel type changed");
    if (auto logical = bundles.sample_output_port_for_channels(
            semantic_type, semantic_channels)) {
      auto const& endpoints = out.bundle_projections.at(
          logical->node_bundle_handle).sample_outputs.at(logical->port_ordinal);
      auto const resolved_config = bundles.resolve_sample_output(*logical).config;
      if (endpoints.size() == 1
          && effective_channel_layout(resolved_config)
              == effective_channel_layout(output_config))
        return endpoints.front();
    }
    std::vector<TopologyPortId> input_sources;
    std::vector<ConnectionNodeInputConfig> input_configs;
    ConnectionNodeEphemeralPortConfig ephemeral{
        .channel_layout = ChannelLayout{.channel_type = output_config.channel_layout.channel_type, .sample_layout = SampleStreamLayout::planar},
        .conversion = ChannelConversionRegistry::plan(
            ChannelLayout{.channel_type = output_config.channel_layout.channel_type, .sample_layout = SampleStreamLayout::planar},
            output_config.channel_layout),
    };
    for (size_t channel = 0; channel < semantic_channels.size(); ++channel) {
      auto const resolved = resolve_sample_source_channel(semantic_channels[channel]);
      auto it = std::ranges::find(input_sources, resolved.port);
      size_t input = 0;
      if (it == input_sources.end()) {
        input = input_sources.size();
        input_sources.push_back(resolved.port);
        input_configs.push_back(ConnectionNodeInputConfig{.input = InputConfig{.channel_layout = resolved.config.channel_layout}});
      } else input = static_cast<size_t>(it - input_sources.begin());
      input_configs[input].channel_copies.push_back({.input_channel = resolved.channel, .ephemeral_port = 0, .ephemeral_channel = channel});
      ephemeral.output_channel_copies.push_back({.converted_channel = channel, .output_channel = channel});
    }
    auto const node = append_generated(make_connection_node(std::move(input_configs), std::vector<ConnectionNodeEphemeralPortConfig>{std::move(ephemeral)}, std::move(output_config), Sample{0.0f}));
    for (size_t input = 0; input < input_sources.size(); ++input)
      add_sample_edge({input_sources[input], {node, input}});
    return {node, 0};
  }

  constexpr TopologyPortId materialize_sample_output_port(
      NodeBundlePortId logical, OutputConfig output_config) {
    auto const semantic_channels = bundles.sample_output_channels(logical);
    return materialize_sample_output_channels(
        bundles.resolve_sample_output(logical).config.channel_layout.channel_type,
        semantic_channels, std::move(output_config));
  }

  constexpr void lower_runtime_output_observers() {
    for (auto const& output : virtual_ports.sample_outputs) {
      std::vector<TopologyPortId> sources;
      std::vector<InputConfig> inputs;
      std::vector<std::string> member_binding_ids;
      sources.reserve(output.member_channels.size());
      inputs.reserve(output.member_channels.size());
      member_binding_ids.reserve(output.member_channels.size());
      for (size_t member = 0; member < output.member_channels.size(); ++member) {
        sources.push_back(materialize_sample_output_channels(
            output.config.channel_layout.channel_type,
            output.member_channels[member], output.config));
        inputs.push_back(InputConfig{.name = output.config.name, .channel_layout = output.config.channel_layout});
        member_binding_ids.push_back(runtime_virtual_port_key(false, PortKind::sample, output.id.virtual_node_id, member, output.id.port_ordinal));
      }
      auto const node = append_generated(make_generated_node(
          RuntimeSampleOutputFamilyNodeSpec{
              .input_configs = std::move(inputs),
              .member_binding_ids = std::move(member_binding_ids),
              .aggregate_binding_id = runtime_virtual_port_key(false, PortKind::sample, output.id.virtual_node_id, std::nullopt, output.id.port_ordinal),
          },
          "iv::RuntimeSampleOutputFamilyNode"));
      for (size_t input = 0; input < sources.size(); ++input)
        add_sample_edge({sources[input], {node, input}});
    }
    for (auto const& output : virtual_ports.event_outputs) {
      std::vector<TopologyPortId> sources;
      std::vector<std::string> member_binding_ids;
      sources.reserve(output.node_bundle_ports.size());
      member_binding_ids.reserve(output.node_bundle_ports.size());
      for (size_t member = 0; member < output.node_bundle_ports.size(); ++member) {
        sources.push_back(materialize_event_output_port(output.node_bundle_ports[member], output.config.type));
        member_binding_ids.push_back(runtime_virtual_port_key(false, PortKind::event, output.id.virtual_node_id, member, output.id.port_ordinal));
      }
      auto const node = append_generated(make_generated_node(
          RuntimeEventOutputFamilyNodeSpec{
              .type = output.config.type,
              .member_count = sources.size(),
              .member_binding_ids = std::move(member_binding_ids),
              .aggregate_binding_id = runtime_virtual_port_key(false, PortKind::event, output.id.virtual_node_id, std::nullopt, output.id.port_ordinal),
          },
          "iv::RuntimeEventOutputFamilyNode"));
      for (size_t input = 0; input < sources.size(); ++input)
        add_event_edge({sources[input], {node, input},
                        EventConversionRegistry::instance().plan(
                            output.config.type, output.config.type)});
    }
  }

  constexpr void lower_execution_root_ports() {
    if (!execution_root) return;
    std::vector<std::pair<TopologyPortId, TopologyPortId>> sample_sources;
    std::vector<std::pair<TopologyPortId, TopologyPortId>> sample_targets;
    std::vector<std::pair<TopologyPortId, TopologyPortId>> event_sources;
    std::vector<std::pair<TopologyPortId, TopologyPortId>> event_targets;
    auto const sample_inputs = public_ports.sample_inputs(bundles);
    for (size_t port = 0; port < sample_inputs.size(); ++port) {
      auto const& input = sample_inputs[port];
      auto node = append_generated(make_generated_node(
          RuntimeSampleInputNodeSpec{
              .output = OutputConfig{.name = input.name, .channel_layout = input.channel_layout},
              .default_value = input.default_value,
              .binding_id = runtime_public_port_key(true, PortKind::sample, port),
          }, "iv::RuntimeSampleInputNode"));
      sample_sources.emplace_back(TopologyPortId{GRAPH_ID, port},
                                  TopologyPortId{node, 0});
    }
    auto const event_inputs = public_ports.event_inputs(bundles);
    for (size_t port = 0; port < event_inputs.size(); ++port) {
      auto node = append_generated(make_generated_node(
          RuntimeEventInputNodeSpec{.type = event_inputs[port].type, .binding_id = runtime_public_port_key(true, PortKind::event, port)},
          "iv::RuntimeEventInputNode"));
      event_sources.emplace_back(TopologyPortId{GRAPH_ID, port},
                                 TopologyPortId{node, 0});
    }
    auto const sample_outputs = public_ports.sample_outputs(bundles);
    for (size_t port = 0; port < sample_outputs.size(); ++port) {
      auto const& output = sample_outputs[port];
      auto node = append_generated(make_generated_node(
          RuntimeSampleOutputNodeSpec{
              .input = InputConfig{.name = output.name, .channel_layout = output.channel_layout},
              .binding_id = runtime_public_port_key(false, PortKind::sample, port),
          }, "iv::RuntimeSampleOutputNode"));
      sample_targets.emplace_back(TopologyPortId{GRAPH_ID, port},
                                  TopologyPortId{node, 0});
    }
    auto const event_outputs = public_ports.event_outputs(bundles);
    for (size_t port = 0; port < event_outputs.size(); ++port) {
      auto node = append_generated(make_generated_node(
          RuntimeEventOutputNodeSpec{.type = event_outputs[port].type, .binding_id = runtime_public_port_key(false, PortKind::event, port)},
          "iv::RuntimeEventOutputNode"));
      event_targets.emplace_back(TopologyPortId{GRAPH_ID, port},
                                 TopologyPortId{node, 0});
    }
    replace_sample_ports(sample_sources, sample_targets);
    replace_event_ports(event_sources, event_targets);
  }

  constexpr std::optional<NodeBundleHandle>
  bundle_for_lowered_node(size_t node) const {
    if (node >= out.bundle_by_lowered_node.size()) return std::nullopt;
    return out.bundle_by_lowered_node[node];
  }

  constexpr NodeBundleHandle subgraph_bundle_handle(
      size_t topology_node) const {
    auto const handle = bundle_for_lowered_node(topology_node);
    if (!handle || !bundles.bundle(*handle).is_subgraph())
      details::error("lowered subgraph node has no semantic subgraph bundle");
    return *handle;
  }

  constexpr void begin_materialization() {
    runtime_node_indices.assign(topology_node_count(), GRAPH_ID);
    source_of.clear();
    event_source_of.clear();
    // Only lowered-subgraph inputs are followed through source_of. Indexing
    // every ordinary node input creates an O(E²) flat-map build in the common
    // flat graph case, then leaves every entry unread.
    for_each_sample_edge([&](TopologyEdge const& edge) {
      if (edge.target.node < topology_node_count() &&
          topology_is_subgraph_node(edge.target.node)) {
        source_of[edge.target] = edge.source;
      }
    });
    for_each_event_edge([&](TopologyEventEdge const& edge) {
      if (edge.target.node < topology_node_count() &&
          topology_is_subgraph_node(edge.target.node)) {
        event_source_of[edge.target] = edge;
      }
    });
  }

  constexpr void append_reflected_nodes() {
    for (size_t node_i = 0; node_i < topology_node_count(); ++node_i) {
      if (topology_is_subgraph_node(node_i)) continue;
      runtime_node_indices[node_i] = graph.nodes.size();
      ReflectedNodeDescription description;
      if (topology_is_authored_concrete_node(node_i)) {
        description = bundles.materialize_concrete_description(
            topology_authored_node(node_i).node_bundle_handle);
      } else if consteval {
        description = details::materialize_generated_node(
            topology_generated_node(node_i).generated_node);
      } else {
        details::runtime_graph_builder_node_call_is_forbidden();
      }
      auto const kind = std::string(description.type_name);
      graph.nodes.push_back(std::move(description));
      append_node_metadata(
          node_i, topology_concrete_lifetime(node_i), std::move(kind));
    }
  }

  constexpr void append_node_metadata(size_t node_i, NodeLifetime const& lifetime,
                                      std::string kind) {
    graph.explicit_ttl_samples.push_back(lifetime.ttl_samples);
    graph.node_ids.push_back(identity.child_id(node_i));
    std::vector<std::string> virtual_ids;
    std::vector<SourceInfo> source_infos;
    if (auto handle = bundle_for_lowered_node(node_i)) {
      auto const& bundle = bundles.bundle(*handle);
      virtual_ids = virtuals.ids_for_bundle(bundle);
      source_infos = bundle.source_annotations().infos;
      for (auto const virtual_handle : bundle.virtual_node_handles()) {
        for (auto const& info : virtuals.record(virtual_handle).source_infos)
          if (std::find(source_infos.begin(), source_infos.end(), info) ==
              source_infos.end())
            source_infos.push_back(info);
      }
    }
    graph.node_source_infos.push_back(std::move(source_infos));
    graph.node_virtual_ids.push_back(std::move(virtual_ids));
    graph.node_construction_order.push_back(node_i);
    graph.node_kinds.push_back(kind);
    graph.node_type_identities.push_back(std::move(kind));
  }

  constexpr ConcretePortId
  resolve_sample_source(TopologyPortId source) const {
    if (auto const* boundary = out.subgraph_input_of_boundary_source.find(source)) {
      auto const* incoming = source_of.find(*boundary);
      if (!incoming)
        details::error(
            "subgraph boundary sample source has no incoming connection");
      return resolve_sample_source(*incoming);
    }
    if (source.node == GRAPH_ID) return {GRAPH_ID, source.port};
    if (source.node >= topology_node_count())
      details::error("unresolved sample boundary out.topology source");
    if (!topology_is_subgraph_node(source.node))
      return {runtime_node_indices[source.node], source.port};
    auto const& node = topology_subgraph_node(source.node);
    auto passthrough = node.lowered_subgraph.sample_output_sources.at(source.port);
    if (passthrough.node == source.node) {
      auto const* it = source_of.find(passthrough);
      if (!it)
        details::error("subgraph sample passthrough has no source");
      return resolve_sample_source(*it);
    }
    return resolve_sample_source(passthrough);
  }

  constexpr ConcretePortId resolve_event_source(TopologyPortId source) const {
    if (auto const* boundary = out.subgraph_event_input_of_boundary_source.find(source)) {
      auto const* incoming = event_source_of.find(*boundary);
      if (!incoming)
        details::error(
            "subgraph boundary event source has no incoming connection");
      return resolve_event_source(incoming->source);
    }
    if (source.node == GRAPH_ID) return {GRAPH_ID, source.port};
    if (source.node >= topology_node_count())
      details::error("unresolved event boundary out.topology source");
    if (!topology_is_subgraph_node(source.node))
      return {runtime_node_indices[source.node], source.port};
    auto const& node = topology_subgraph_node(source.node);
    auto passthrough = node.lowered_subgraph.event_output_sources.at(source.port);
    if (passthrough.node == source.node) {
      auto const* it = event_source_of.find(passthrough);
      if (!it)
        details::error("subgraph event passthrough has no source");
      return resolve_event_source(it->source);
    }
    return resolve_event_source(passthrough);
  }

  constexpr TopologyPortId resolve_sample_source_topology(TopologyPortId source) const {
    if (auto const* boundary = out.subgraph_input_of_boundary_source.find(source)) {
      auto const* incoming = source_of.find(*boundary);
      if (!incoming)
        details::error(
            "subgraph boundary sample source has no incoming connection");
      return resolve_sample_source_topology(*incoming);
    }
    if (source.node == GRAPH_ID) return source;
    if (source.node >= topology_node_count())
      details::error("unresolved sample boundary out.topology source");
    if (!topology_is_subgraph_node(source.node)) return source;
    auto const& node = topology_subgraph_node(source.node);
    auto passthrough = node.lowered_subgraph.sample_output_sources.at(source.port);
    if (passthrough.node == source.node) {
      auto const* it = source_of.find(passthrough);
      if (!it)
        details::error("subgraph sample passthrough has no source");
      return resolve_sample_source_topology(*it);
    }
    return resolve_sample_source_topology(passthrough);
  }

  constexpr TopologyPortId resolve_event_source_topology(TopologyPortId source) const {
    if (auto const* boundary = out.subgraph_event_input_of_boundary_source.find(source)) {
      auto const* incoming = event_source_of.find(*boundary);
      if (!incoming)
        details::error(
            "subgraph boundary event source has no incoming connection");
      return resolve_event_source_topology(incoming->source);
    }
    if (source.node == GRAPH_ID) return source;
    if (source.node >= topology_node_count())
      details::error("unresolved event boundary out.topology source");
    if (!topology_is_subgraph_node(source.node)) return source;
    auto const& node = topology_subgraph_node(source.node);
    auto passthrough = node.lowered_subgraph.event_output_sources.at(source.port);
    if (passthrough.node == source.node) {
      auto const* it = event_source_of.find(passthrough);
      if (!it)
        details::error("subgraph event passthrough has no source");
      return resolve_event_source_topology(it->source);
    }
    return resolve_event_source_topology(passthrough);
  }

    constexpr void add_sample_target_edges(ConcretePortId source,
                                           TopologyPortId target) {
        if (target.node == GRAPH_ID) {
            sample_edge_batch.push_back(
                GraphEdge{source, {GRAPH_ID, target.port}});
            return;
        }
        if (!topology_is_subgraph_node(target.node)) {
            sample_edge_batch.push_back(GraphEdge{
                source, {runtime_node_indices[target.node], target.port}});
            return;
        }
        auto const& node = topology_subgraph_node(target.node);
        for (auto child : node.lowered_subgraph.sample_input_targets.at(target.port))
            add_sample_target_edges(source, child);
    }

    constexpr void canonicalize_sample_edges() {
        std::ranges::sort(sample_edge_batch);
        sample_edge_batch.erase(
            std::ranges::unique(sample_edge_batch).begin(),
            sample_edge_batch.end());
      graph.edges.replace(std::move(sample_edge_batch));
      sample_edge_batch.clear();
    }

  constexpr void add_event_target_edges(GraphEventEdge edge, TopologyPortId target) {
    if (target.node == GRAPH_ID) {
      event_edge_batch.push_back(GraphEventEdge{
          edge.source, {GRAPH_ID, target.port}, std::move(edge.conversion)});
      return;
    }
    if (!topology_is_subgraph_node(target.node)) {
      event_edge_batch.push_back(GraphEventEdge{
          edge.source, {runtime_node_indices[target.node], target.port},
          std::move(edge.conversion)});
      return;
    }
    auto const& node = topology_subgraph_node(target.node);
    for (auto child : node.lowered_subgraph.event_input_targets.at(target.port))
      add_event_target_edges(
          GraphEventEdge{edge.source, {}, edge.conversion}, child);
  }

  constexpr void canonicalize_event_edges() {
    std::ranges::sort(event_edge_batch);
    event_edge_batch.erase(
        std::ranges::unique(event_edge_batch).begin(),
        event_edge_batch.end());
    graph.event_edges.replace(std::move(event_edge_batch));
    event_edge_batch.clear();
  }

  constexpr void lower_edges() {
    for_each_sample_edge([&](TopologyEdge const& edge) {
      if (out.subgraph_input_of_boundary_source.contains(edge.source)) return;
      add_sample_target_edges(resolve_sample_source(edge.source), edge.target);
    });
    for_each_event_edge([&](TopologyEventEdge const& edge) {
      if (out.subgraph_event_input_of_boundary_source.contains(edge.source))
        return;
      add_event_target_edges(
          GraphEventEdge{resolve_event_source(edge.source), {}, edge.conversion},
          edge.target);
    });
  }

  constexpr ConcretePortId materialize_subgraph_default(
      size_t subgraph_node, size_t input_port) {
    runtime_node_indices.push_back(graph.nodes.size());
    if consteval {
      graph.nodes.push_back(details::materialize_generated_node(
          ConstantNodeSpec{
              .value = topology_subgraph_node(subgraph_node)
                           .inputs()[input_port]
                           .default_value}));
    } else {
      details::runtime_graph_builder_node_call_is_forbidden();
    }
    graph.explicit_ttl_samples.push_back(std::nullopt);
    graph.node_ids.push_back(identity.child_id(subgraph_node) + ".default." +
                             details::decimal_string(input_port));
    graph.node_virtual_ids.emplace_back();
    graph.node_source_infos.emplace_back();
    graph.node_construction_order.push_back(subgraph_node);
    return {graph.nodes.size() - 1, 0};
  }

  constexpr void add_subgraph_default_edges() {
    for (size_t node_i = 0; node_i < topology_node_count(); ++node_i) {
      if (!topology_is_subgraph_node(node_i)) continue;
      auto const& node = topology_subgraph_node(node_i);
      for (size_t input = 0; input < node.inputs().size(); ++input) {
        TopologyPortId subgraph_input{node_i, input};
        if (source_of.contains(subgraph_input)) continue;
        auto source = materialize_subgraph_default(node_i, input);
        for (auto target : node.lowered_subgraph.sample_input_targets[input])
            add_sample_target_edges(source, target);
    }
    }
  }

  constexpr void copy_detach_info() {
    for (auto const& [source, info] : out.detached_info_by_source) {
      auto remapped = resolve_sample_source(source);
      graph.detached_info_by_source.emplace(
          remapped,
          DetachedInfo{
              .detach_id = info.detach_id,
              .original_source = remapped,
              .writer_node = runtime_node_indices[info.writer_node],
              .reader_output = resolve_sample_source(info.reader_output),
              .loop_extra_latency = info.loop_extra_latency,
          });
    }
    for (auto reader : out.detached_reader_outputs)
      graph.detached_reader_outputs.insert(resolve_sample_source(reader));
  }

  constexpr iv::LoweredSubgraphSpec::PortRef make_scope_port_ref(
      TopologyPortId port) const {
    if (port.node == GRAPH_ID)
      return {.port = {GRAPH_ID, port.port}, .valid = true};
    if (port.node >= topology_node_count() || topology_is_subgraph_node(port.node))
      details::error("scope port did not resolve to a concrete out.topology node");
    return {.port = {runtime_node_indices[port.node], port.port}, .valid = true};
  }

  constexpr void collect_scope_targets(
      TopologyPortId target,
      std::vector<iv::LoweredSubgraphSpec::PortRef>& out_refs) const {
    if (target.node == GRAPH_ID || !topology_is_subgraph_node(target.node)) {
      out_refs.push_back(make_scope_port_ref(target));
      return;
    }
    for (auto child : topology_subgraph_node(target.node)
                          .lowered_subgraph.sample_input_targets[target.port])
      collect_scope_targets(child, out_refs);
  }

  constexpr void collect_scope_event_targets(
      TopologyPortId target,
      std::vector<iv::LoweredSubgraphSpec::PortRef>& out_refs) const {
    if (target.node == GRAPH_ID || !topology_is_subgraph_node(target.node)) {
      out_refs.push_back(make_scope_port_ref(target));
      return;
    }
    for (auto child : topology_subgraph_node(target.node)
                          .lowered_subgraph.event_input_targets[target.port])
      collect_scope_event_targets(child, out_refs);
  }

  constexpr std::vector<std::vector<size_t>>
  build_topology_scope_memberships(std::span<size_t const> subgraphs) {
    auto const node_count = topology_node_count();
    out.scope_memberships.assign(node_count, {});
    std::vector<std::vector<size_t>> topology_nodes_by_bundle(bundles.size());
    for (size_t node = 0; node < node_count; ++node) {
      if (auto const handle = bundle_for_lowered_node(node)) {
        topology_nodes_by_bundle.at(*handle).push_back(node);
      }
    }
    for (size_t scope : subgraphs) {
      auto const info = bundles.subgraph_info(subgraph_bundle_handle(scope));
      for (size_t handle = info.child_begin;
           handle < info.child_begin + info.child_count;
           ++handle) {
        for (size_t node : topology_nodes_by_bundle.at(handle)) {
          out.scope_memberships[node].push_back(scope);
        }
      }
    }

    std::vector<bool> generated(node_count, false);
    std::vector<std::vector<size_t>> generated_neighbors(node_count);
    std::vector<std::vector<size_t>> adjacent_scope_memberships(node_count);
    for (size_t node = 0; node < node_count; ++node) {
      generated[node] = !topology_is_subgraph_node(node)
          && !bundle_for_lowered_node(node).has_value();
    }
    auto inspect_edge = [&](TopologyPortId left, TopologyPortId right) {
      if (left.node >= node_count || right.node >= node_count) return;
      auto connect_generated = [&](size_t generated_node, size_t other) {
        if (!generated[generated_node]) return;
        if (generated[other]) {
          generated_neighbors[generated_node].push_back(other);
        } else {
          auto const& scopes = out.scope_memberships[other];
          adjacent_scope_memberships[generated_node].insert(
              adjacent_scope_memberships[generated_node].end(),
              scopes.begin(), scopes.end());
        }
      };
      connect_generated(left.node, right.node);
      connect_generated(right.node, left.node);
    };
    for_each_sample_edge([&](TopologyEdge const& edge) {
      inspect_edge(edge.source, edge.target);
    });
    for_each_event_edge([&](TopologyEventEdge const& edge) {
      inspect_edge(edge.source, edge.target);
    });

    std::vector<bool> visited(node_count, false);
    for (size_t start = 0; start < node_count; ++start) {
      if (!generated[start] || visited[start]) continue;
      std::vector<size_t> component;
      std::vector<size_t> membership;
      std::vector<size_t> pending{start};
      visited[start] = true;
      for (size_t pending_i = 0; pending_i < pending.size(); ++pending_i) {
        auto const node = pending[pending_i];
        component.push_back(node);
        auto const& adjacent = adjacent_scope_memberships[node];
        membership.insert(membership.end(), adjacent.begin(), adjacent.end());
        for (size_t neighbor : generated_neighbors[node]) {
          if (!visited[neighbor]) {
            visited[neighbor] = true;
            pending.push_back(neighbor);
          }
        }
      }
      std::sort(membership.begin(), membership.end());
      membership.erase(std::unique(membership.begin(), membership.end()),
                       membership.end());
      for (size_t node : component)
        out.scope_memberships[node] = membership;
    }

    details::ConstexprHashMap<size_t, size_t, details::ConstexprIdentityHash>
        subgraph_order;
    for (size_t i = 0; i < subgraphs.size(); ++i)
      subgraph_order.try_emplace(subgraphs[i], i);
    std::vector<std::vector<size_t>> member_nodes(subgraphs.size());
    for (size_t node = 0; node < topology_node_count(); ++node) {
      if (topology_is_subgraph_node(node)) continue;
      for (size_t scope : out.scope_memberships.at(node)) {
        if (auto const* order = subgraph_order.find(scope))
          member_nodes[*order].push_back(node);
      }
    }
    return member_nodes;
  }

  constexpr std::vector<iv::LoweredSubgraphSpec>
  build_lowered_scopes() {
    std::vector<size_t> subgraphs;
    for (size_t i = 0; i < topology_node_count(); ++i)
      if (topology_is_subgraph_node(i)) subgraphs.push_back(i);
    if (subgraphs.empty()) return {};

    auto const scope_member_topology_nodes =
        build_topology_scope_memberships(subgraphs);

    details::ConstexprHashMap<size_t, size_t, details::ConstexprIdentityHash>
        scope_index;
    std::vector<iv::LoweredSubgraphSpec> scopes;
    for (size_t subgraph_order = 0;
         subgraph_order < subgraphs.size();
         ++subgraph_order) {
      auto const subgraph_i = subgraphs[subgraph_order];
      auto const& subgraph = topology_subgraph_node(subgraph_i);
      iv::LoweredSubgraphSpec scope;
      scope.kind = subgraph.lowered_subgraph.kind;
      scope.backing_node_id = identity.child_id(subgraph_i);

      auto const handle = subgraph_bundle_handle(subgraph_i);
      for (auto const& info : bundles.bundle(handle).source_annotations().infos) {
        scope.source_infos.push_back(info);
        if (!info.span.file_path.empty() && info.span.begin <= info.span.end &&
            std::find(scope.source_spans.begin(), scope.source_spans.end(),
                      info.span) == scope.source_spans.end())
          scope.source_spans.push_back(info.span);
      }

      scope.sample_inputs = subgraph.ports.sample_inputs;
      scope.sample_outputs = subgraph.ports.sample_outputs;
      scope.event_inputs = subgraph.ports.event_input_configs;
      scope.event_outputs = subgraph.ports.event_output_configs;
      scope.ttl_samples = subgraph.lifetime.ttl_samples;

      for (auto node : scope_member_topology_nodes[subgraph_order])
        scope.member_nodes.push_back(runtime_node_indices[node]);

      for (auto const& targets : subgraph.lowered_subgraph.sample_input_targets) {
        std::vector<iv::LoweredSubgraphSpec::PortRef> flat;
        for (auto target : targets) collect_scope_targets(target, flat);
        scope.sample_input_targets.push_back(std::move(flat));
      }
      for (auto source : subgraph.lowered_subgraph.sample_output_sources)
        scope.sample_output_sources.push_back(make_scope_port_ref(
            resolve_sample_source_topology(source)));
      for (auto const& targets : subgraph.lowered_subgraph.event_input_targets) {
        std::vector<iv::LoweredSubgraphSpec::PortRef> flat;
        for (auto target : targets) collect_scope_event_targets(target, flat);
        scope.event_input_targets.push_back(std::move(flat));
      }
      for (auto source : subgraph.lowered_subgraph.event_output_sources)
        scope.event_output_sources.push_back(
            make_scope_port_ref(resolve_event_source_topology(source)));

      if (scope.member_nodes.empty()) continue;
      std::sort(scope.member_nodes.begin(), scope.member_nodes.end());
      scope.member_nodes.erase(
          std::unique(scope.member_nodes.begin(), scope.member_nodes.end()),
          scope.member_nodes.end());
      scope_index.try_emplace(subgraph_i, scopes.size());
      scopes.push_back(std::move(scope));
    }

    for (auto child : subgraphs) {
      auto const* it = scope_index.find(child);
      if (!it) continue;
      size_t parent = GRAPH_ID;
      size_t best_span = std::numeric_limits<size_t>::max();
      for (auto candidate : out.scope_memberships.at(child)) {
        if (!scope_index.contains(candidate))
          continue;
        auto const span =
            bundles.subgraph_info(subgraph_bundle_handle(candidate))
                .child_count;
        if (span < best_span) {
          best_span = span;
          parent = candidate;
        }
      }
      if (parent != GRAPH_ID)
        scopes[*it].parent_scope = *scope_index.find(parent);
    }
    return scopes;
  }
public:
  constexpr GraphLowerer(GraphBuilderIdentity const& identity_,
          GraphBuilderNodeBundles const& b, GraphBuilderConnections const& c,
          GraphBuilderPublicPorts const& p, GraphBuilderVirtualNodes const& v,
          GraphBuilderDetach const& d, details::LoweringWorkspace& lowered,
          bool is_execution_root)
      : identity(identity_), bundles(b),connections(c),public_ports(p),virtuals(v),detach(d),
        execution_root(is_execution_root), out(lowered),
        virtual_ports(virtuals.ports(bundles)),
        runtime_node_indices(topology_node_count(), GRAPH_ID) {}

  static consteval ExecutableGraphIR lower(
      AuthoredGraph const&, GraphLoweringOptions = {});
  static consteval size_t profile(
      AuthoredGraph const&, GraphLoweringOptions,
      GraphLoweringProfileStage);
  constexpr void run(bool normalize = true) {
    project_bundles();
    index_runtime_sample_bindings();
    auto const sample_plan = plan_sample_lowering();
    auto sample_state = lower_connected_sample_groups(sample_plan);
    lower_vacant_sample_inputs(sample_state);
    bind_subgraph_sample_inputs(sample_state);
    lower_events();
    lower_detach();
    lower_runtime_output_observers();
    lower_execution_root_ports();
    if (normalize) normalize_topology_edges();
  }
};

consteval size_t GraphLowerer::profile(
    AuthoredGraph const& authored, GraphLoweringOptions options,
    GraphLoweringProfileStage stage) {
  details::LoweringWorkspace lowered;
  GraphLowerer lowerer(
      authored.identity, authored.node_bundles, authored.connections,
      authored.public_ports, authored.virtual_nodes, authored.detach, lowered,
      options.execution_root);
  auto topology_cardinality = [&] {
    return lowerer.out.topology_nodes.size()
        + lowerer.out.topology_edges.size()
        + lowerer.out.topology_event_edges.size()
        + lowerer.out.pending_topology_edges.size()
        + lowerer.out.pending_topology_event_edges.size()
        + lowerer.out.bundle_projections.size()
        + lowerer.out.bundle_by_lowered_node.size()
        + lowerer.out.scope_memberships.size()
        + lowerer.out.subgraph_input_of_boundary_source.size()
        + lowerer.out.subgraph_event_input_of_boundary_source.size()
        + lowerer.out.detached_info_by_source.size()
        + lowerer.out.detached_reader_outputs.size()
        + lowerer.subgraph_by_boundary.size()
        + lowerer.materialized_event_output_ports.size()
        + lowerer.runtime_node_indices.size()
        + lowerer.source_of.size()
        + lowerer.event_source_of.size()
        + lowerer.runtime_binding_by_semantic_target.size()
        + lowerer.runtime_binding_by_concrete_target.size();
  };
  auto graph_cardinality = [&] {
    return lowerer.graph.nodes.size()
        + lowerer.graph.explicit_ttl_samples.size()
        + lowerer.graph.node_ids.size()
        + lowerer.graph.node_virtual_ids.size()
        + lowerer.graph.node_source_infos.size()
        + lowerer.graph.node_construction_order.size()
        + lowerer.graph.node_kinds.size()
        + lowerer.graph.node_type_identities.size()
        + lowerer.graph.edges.size()
        + lowerer.graph.event_edges.size()
        + lowerer.graph.detached_info_by_source.size()
        + lowerer.graph.detached_reader_outputs.size();
  };

  lowerer.run();
  if (stage == GraphLoweringProfileStage::topology)
    return topology_cardinality();

  lowerer.begin_materialization();
    lowerer.append_reflected_nodes();
    lowerer.lower_edges();
    lowerer.add_subgraph_default_edges();
    lowerer.canonicalize_sample_edges();
    lowerer.canonicalize_event_edges();
    lowerer.copy_detach_info();
  if (stage == GraphLoweringProfileStage::materialization)
    return graph_cardinality();

  auto sample_inputs = options.execution_root
      ? std::vector<InputConfig>{}
      : std::vector<InputConfig>(
            authored.public_ports.sample_inputs(authored.node_bundles).begin(),
            authored.public_ports.sample_inputs(authored.node_bundles).end());
  auto sample_outputs = options.execution_root
      ? std::vector<OutputConfig>{}
      : std::vector<OutputConfig>(
            authored.public_ports.sample_outputs(authored.node_bundles).begin(),
            authored.public_ports.sample_outputs(authored.node_bundles).end());

  details::expand_lowered_hyperedge_ports(lowerer.graph, authored.identity.value);
  details::stub_lowered_dangling_ports(
      lowerer.graph, sample_inputs.size(), authored.identity.value);
  details::resolve_lowered_sample_edge_conversions(
      lowerer.graph, sample_inputs, sample_outputs);
  details::validate_lowered_graph(
      lowerer.graph, sample_inputs.size(), sample_outputs.size());
  details::validate_lowered_detached_edges(
      lowerer.graph, authored.identity.value);
  return graph_cardinality();
}

consteval ExecutableGraphIR GraphLowerer::lower(
    AuthoredGraph const& authored, GraphLoweringOptions options) {
  details::LoweringWorkspace lowered;
  GraphLowerer lowerer(
      authored.identity, authored.node_bundles, authored.connections,
      authored.public_ports, authored.virtual_nodes, authored.detach, lowered,
      options.execution_root);
  lowerer.run();
  lowerer.begin_materialization();
  lowerer.append_reflected_nodes();
  lowerer.lower_edges();
  lowerer.add_subgraph_default_edges();
  lowerer.canonicalize_sample_edges();
  lowerer.canonicalize_event_edges();
  lowerer.copy_detach_info();

  auto sample_inputs = options.execution_root
      ? std::vector<InputConfig>{}
      : std::vector<InputConfig>(
            authored.public_ports.sample_inputs(authored.node_bundles).begin(),
            authored.public_ports.sample_inputs(authored.node_bundles).end());
  auto sample_outputs = options.execution_root
      ? std::vector<OutputConfig>{}
      : std::vector<OutputConfig>(
            authored.public_ports.sample_outputs(authored.node_bundles).begin(),
            authored.public_ports.sample_outputs(authored.node_bundles).end());
  auto event_inputs = options.execution_root
      ? std::vector<EventInputConfig>{}
      : std::vector<EventInputConfig>(
            authored.public_ports.event_inputs(authored.node_bundles).begin(),
            authored.public_ports.event_inputs(authored.node_bundles).end());
  auto event_outputs = options.execution_root
      ? std::vector<EventOutputConfig>{}
      : std::vector<EventOutputConfig>(
            authored.public_ports.event_outputs(authored.node_bundles).begin(),
            authored.public_ports.event_outputs(authored.node_bundles).end());

  details::expand_lowered_hyperedge_ports(lowerer.graph, authored.identity.value);
  details::stub_lowered_dangling_ports(
      lowerer.graph, sample_inputs.size(), authored.identity.value);
  details::resolve_lowered_sample_edge_conversions(
      lowerer.graph, sample_inputs, sample_outputs);
  details::validate_lowered_graph(lowerer.graph, sample_inputs.size(),
                          sample_outputs.size());
  details::validate_lowered_detached_edges(lowerer.graph, authored.identity.value);

  auto scopes = lowerer.build_lowered_scopes();
  auto virtual_metadata =
      details::build_virtual_metadata(lowerer.graph, scopes);
  auto [virtual_nodes, virtual_node_ids_by_backing_node_id] =
      std::move(virtual_metadata);
  auto const authored_connectivity =
      details::build_lowering_authored_connectivity(authored.connections);
  GraphIntrospectionMetadata introspection;
  introspection.virtual_nodes = std::move(virtual_nodes);
  details::apply_virtual_port_metadata(
      introspection, authored.node_bundles, authored.virtual_nodes,
      authored_connectivity);

  auto sample_families =
      authored.public_ports.sample_input_families(authored.node_bundles);
  for (auto& family : sample_families.families) {
    family.authored_connected = std::ranges::any_of(
        family.channels, [&](auto const& channel) {
          return std::ranges::any_of(channel.port_ordinals, [&](auto ordinal) {
            auto channels = authored.node_bundles.sample_output_channels(
                {authored.public_ports.boundary_handle(), PortKind::sample,
                 ordinal});
            return std::ranges::any_of(channels, [&](auto c) {
              return authored_connectivity.sample_outputs.contains(c);
            });
          });
        });
  }
  introspection.public_sample_inputs = std::move(sample_families.families);
  introspection.public_event_inputs =
      authored.public_ports.collected_event_inputs(authored.node_bundles);
  for (auto& input : introspection.public_event_inputs) {
    auto ports = authored.node_bundles.event_output_ports(
        {authored.public_ports.boundary_handle(), PortKind::event,
         input.port_ordinal});
    input.graph_connected = std::ranges::any_of(ports, [&](auto p) {
      return authored_connectivity.event_outputs.contains(p);
    });
  }
  introspection.public_sample_outputs =
      authored.public_ports.sample_output_families(authored.node_bundles)
          .families;
  introspection.public_event_outputs =
      authored.public_ports.collected_event_outputs(authored.node_bundles);

  return {
      .graph_id = authored.identity.value,
      .graph = std::move(lowerer.graph),
      .scopes = std::move(scopes),
      .public_inputs = std::move(sample_inputs),
      .public_outputs = std::move(sample_outputs),
      .public_event_inputs = std::move(event_inputs),
      .public_event_outputs = std::move(event_outputs),
      .virtual_node_ids_by_backing_node_id =
          std::move(virtual_node_ids_by_backing_node_id),
      .introspection = std::move(introspection),
  };
}

namespace details {
struct GraphLowererTestAccess {
  static consteval SampleLoweringPassFacts sample_lowering_pass_facts(
      AuthoredGraph const& authored) {
    LoweringWorkspace workspace;
    GraphLowerer lowerer(
        authored.identity, authored.node_bundles, authored.connections,
        authored.public_ports, authored.virtual_nodes, authored.detach,
        workspace, false);
    lowerer.project_bundles();
    lowerer.index_runtime_sample_bindings();
    auto const plan = lowerer.plan_sample_lowering();
    auto state = lowerer.lower_connected_sample_groups(plan);
    SampleLoweringPassFacts facts{
        .planned_groups = plan.groups.size(),
        .connected_bound_targets = state.bound_target_count,
        .vacant_bound_targets = 0,
        .assigned_subgraph_outputs = state.assigned_subgraph_outputs.size(),
    };
    lowerer.lower_vacant_sample_inputs(state);
    facts.vacant_bound_targets = state.bound_target_count;
    lowerer.bind_subgraph_sample_inputs(state);
    return facts;
  }
};
} // namespace details

} // namespace iv
