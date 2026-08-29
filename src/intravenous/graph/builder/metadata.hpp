#pragma once

#include <intravenous/graph/names.h>
#include <intravenous/graph/compiler.h>

#include <algorithm>
#include <flat_map>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace iv {
class GraphBuilderNodeBundles;
class GraphBuilderVirtualNodes;
class GraphBuilderConnections;

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
  std::vector<SourceInfo> source_infos;
  std::vector<VirtualConcretePortInfo> sample_inputs;
  std::vector<VirtualConcretePortInfo> sample_outputs;
  std::vector<VirtualConcretePortInfo> event_inputs;
  std::vector<VirtualConcretePortInfo> event_outputs;
};

struct MetadataGraphNode {
  std::vector<InputConfig> input_configs;
  std::vector<OutputConfig> output_configs;
  std::vector<EventInputConfig> event_input_configs;
  std::vector<EventOutputConfig> event_output_configs;

  constexpr std::vector<InputConfig> const& inputs() const { return input_configs; }
  constexpr std::vector<OutputConfig> const& outputs() const { return output_configs; }
  constexpr std::vector<EventInputConfig> const& event_inputs() const {
    return event_input_configs;
  }
  constexpr std::vector<EventOutputConfig> const& event_outputs() const {
    return event_output_configs;
  }

  void tick(TickSampleContext<MetadataGraphNode> const&) const {}
};

constexpr VirtualPortConnectivity
aggregate_connectivity(std::span<VirtualConcretePortInfo const> ports) {
  bool any_connected = false;
  bool any_disconnected = false;
  for (auto const& port : ports) {
    any_connected = any_connected || port.connected;
    any_disconnected = any_disconnected || !port.connected;
  }
  if (any_connected && any_disconnected) {
    return VirtualPortConnectivity::mixed;
  }
  return any_connected ? VirtualPortConnectivity::connected
                       : VirtualPortConnectivity::disconnected;
}

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
    std::vector<VirtualConcretePortInfo> concrete_ports;
    concrete_ports.reserve(nodes.size());
    for (auto const* node : nodes) {
      concrete_ports.push_back((node->*member)[i]);
    }

    virtual_ports.push_back(IntrospectionPortInfo{
        .name = first_ports[i].name,
        .type = first_ports[i].type,
        .connectivity = aggregate_connectivity(concrete_ports),
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
build_virtual_metadata(PreparedGraph const& g,
                       std::span<LoweredSubgraphSpec const> lowered_scopes) {
  auto sample_input_connected = [&](size_t node, size_t port) {
    return std::ranges::any_of(g.edges, [&](GraphEdge const& edge) {
      return edge.target.node == node && edge.target.port == port;
    });
  };
  auto sample_output_connected = [&](size_t node, size_t port) {
    return std::ranges::any_of(g.edges, [&](GraphEdge const& edge) {
      return edge.source.node == node && edge.source.port == port;
    });
  };
  auto event_input_connected = [&](size_t node, size_t port) {
    return std::ranges::any_of(g.event_edges, [&](GraphEventEdge const& edge) {
      return edge.target.node == node && edge.target.port == port;
    });
  };
  auto event_output_connected = [&](size_t node, size_t port) {
    return std::ranges::any_of(g.event_edges, [&](GraphEventEdge const& edge) {
      return edge.source.node == node && edge.source.port == port;
    });
  };

  std::vector<VirtualConcreteNode> concrete_nodes;
  std::vector<std::vector<std::string>> concrete_node_virtual_ids;
  concrete_nodes.reserve(g.nodes.size());
  concrete_node_virtual_ids.reserve(g.nodes.size() + lowered_scopes.size());
  for (size_t node_i = 0; node_i < g.nodes.size(); ++node_i) {
    VirtualConcreteNode concrete;
    concrete.id = g.node_ids[node_i];
    concrete.kind = node_i < g.node_kinds.size()
                        ? g.node_kinds[node_i]
                        : std::string(g.nodes[node_i].type_name);
    concrete.construction_order = node_i < g.node_construction_order.size()
                                      ? g.node_construction_order[node_i]
                                      : node_i;
    concrete.type_identity = node_i < g.node_type_identities.size()
                                 ? g.node_type_identities[node_i]
                                 : concrete.kind;
    if (node_i < g.node_source_infos.size()) {
      concrete.source_infos = g.node_source_infos[node_i];
    }

    auto const inputs = g.nodes[node_i].inputs();
    concrete.sample_inputs.reserve(inputs.size());
    for (size_t input_i = 0; input_i < inputs.size(); ++input_i) {
      concrete.sample_inputs.push_back(VirtualConcretePortInfo{
          .name = inputs[input_i].name,
          .type = "sample",
          .connected = sample_input_connected(node_i, input_i),
          .history = inputs[input_i].history,
          .default_value = inputs[input_i].default_value,
          .min = inputs[input_i].min,
          .max = inputs[input_i].max,
      });
    }

    auto const outputs = g.nodes[node_i].outputs();
    concrete.sample_outputs.reserve(outputs.size());
    for (size_t output_i = 0; output_i < outputs.size(); ++output_i) {
      concrete.sample_outputs.push_back(VirtualConcretePortInfo{
          .name = outputs[output_i].name,
          .type = "sample",
          .connected = sample_output_connected(node_i, output_i),
          .history = outputs[output_i].history,
          .latency = outputs[output_i].latency,
      });
    }

    auto const event_inputs = g.nodes[node_i].event_inputs();
    concrete.event_inputs.reserve(event_inputs.size());
    for (size_t input_i = 0; input_i < event_inputs.size(); ++input_i) {
      concrete.event_inputs.push_back(VirtualConcretePortInfo{
          .name = event_inputs[input_i].name,
          .type = event_type_name(event_inputs[input_i].type),
          .connected = event_input_connected(node_i, input_i),
      });
    }

    auto const event_outputs = g.nodes[node_i].event_outputs();
    concrete.event_outputs.reserve(event_outputs.size());
    for (size_t output_i = 0; output_i < event_outputs.size(); ++output_i) {
      concrete.event_outputs.push_back(VirtualConcretePortInfo{
          .name = event_outputs[output_i].name,
          .type = event_type_name(event_outputs[output_i].type),
          .connected = event_output_connected(node_i, output_i),
      });
    }

    concrete_nodes.push_back(std::move(concrete));
    concrete_node_virtual_ids.push_back(node_i < g.node_virtual_ids.size()
                                            ? g.node_virtual_ids[node_i]
                                            : std::vector<std::string>{});
  }

  for (size_t scope_i = 0; scope_i < lowered_scopes.size(); ++scope_i) {
    auto const& scope = lowered_scopes[scope_i];
    VirtualConcreteNode concrete;
    concrete.id = scope.backing_node_id;
    concrete.kind = scope.kind;
    concrete.construction_order = g.nodes.size() + scope_i;
    concrete.type_identity = "lowered-subgraph:" + scope.kind;
    concrete.source_infos = scope.source_infos;

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
                       (scope.sample_output_sources[output_i].is_graph_port ||
                        !scope.sample_output_sources[output_i].node_id.empty()),
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
                       (scope.event_output_sources[output_i].is_graph_port ||
                        !scope.event_output_sources[output_i].node_id.empty()),
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
  for (size_t i = 0; i < concrete_nodes.size(); ++i) {
    if (i >= concrete_node_virtual_ids.size() ||
        concrete_node_virtual_ids[i].empty()) {
      continue;
    }
    for (auto const& virtual_node_id : concrete_node_virtual_ids[i]) {
      auto group_it =
          std::find_if(groups.begin(), groups.end(), [&](auto const& group) {
            return group.virtual_node_id == virtual_node_id;
          });
      if (group_it == groups.end()) {
        groups.push_back(VirtualGroup{
            .virtual_node_id = virtual_node_id,
            .member_indices = {i},
        });
      } else if (!std::ranges::contains(group_it->member_indices, i)) {
        group_it->member_indices.push_back(i);
      }
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

  VirtualNodeIdsByBackingNodeId virtual_node_ids_by_backing_node_id;
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

// Execution-root construction needs only this reverse lookup, not the full
// introspection nodes built by build_virtual_metadata(). Keep that path small:
// it deliberately mirrors the virtual-ID and backing-node relationship used
// above without constructing port metadata, source-span aggregates, or groups.
constexpr VirtualNodeIdsByBackingNodeId
build_virtual_node_ids_by_backing_node_id(
    PreparedGraph const& g,
    std::span<LoweredSubgraphSpec const> lowered_scopes) {
  VirtualNodeIdsByBackingNodeId result;
  for (size_t node_i = 0; node_i < g.nodes.size(); ++node_i) {
    if (node_i >= g.node_virtual_ids.size()) continue;
    for (auto const& virtual_node_id : g.node_virtual_ids[node_i]) {
      result[g.node_ids[node_i]].push_back(virtual_node_id);
    }
  }

  for (auto const& scope : lowered_scopes) {
    auto const type_identity = "lowered-subgraph:" + scope.kind;
    for (auto const& info : scope.source_infos) {
      if (info.declaration_identity.empty()) continue;
      result[scope.backing_node_id].push_back(typed_virtual_node_id(
          info.declaration_identity, type_identity));
    }
  }

  for (auto&& [_, virtual_ids] : result) {
    std::sort(virtual_ids.begin(), virtual_ids.end());
    virtual_ids.erase(std::unique(virtual_ids.begin(), virtual_ids.end()),
                      virtual_ids.end());
  }
  return result;
}

namespace {
constexpr bool sample_channel_is_connected(
    GraphBuilderConnections const& connections, SampleInputChannelId channel) {
  return connections.sample_input_is_connected(channel);
}

constexpr bool sample_channel_is_connected(
    GraphBuilderConnections const& connections, SampleOutputChannelId channel) {
  return connections.sample_output_is_connected(channel);
}

template <class Mapping>
constexpr std::vector<IntrospectionPortInfo> project_virtual_sample_ports(
    std::vector<Mapping> const& mappings,
    GraphBuilderNodeBundles const& node_bundles,
    GraphBuilderConnections const& connections, bool inputs) {
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
      bool const connected = sample_channel_is_connected(connections, channel);
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
    GraphBuilderConnections const& connections, NodeBundleHandle bundle_handle,
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
      bundle_mappings, node_bundles, connections, inputs);
}

constexpr std::vector<IntrospectionPortInfo> project_virtual_event_ports(
    std::vector<VirtualEventPortMapping> const& mappings,
    GraphBuilderNodeBundles const&,
    GraphBuilderConnections const& connections, bool inputs) {
  std::vector<IntrospectionPortInfo> result;
  result.reserve(mappings.size());
  for (auto const& mapping : mappings) {
    if (mapping.node_bundle_ports.empty()) continue;
    bool connected = false;
    for (auto const port : mapping.node_bundle_ports) {
      connected = connected || (inputs
          ? connections.event_input_is_connected(
                EventInputPortId{port.node_bundle_handle, port.port_ordinal})
          : connections.event_output_is_connected(
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
    GraphBuilderConnections const& connections,
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
      projected, node_bundles, connections, inputs);
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
    GraphBuilderConnections const& connections) {
  for (auto const& record : virtual_nodes.records()) {
    auto it = std::find_if(metadata.virtual_nodes.begin(),
                           metadata.virtual_nodes.end(),
                           [&](auto const& node) { return node.id == record.id; });
    if (it == metadata.virtual_nodes.end()) {
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
      it = std::prev(metadata.virtual_nodes.end());
    }
    it->source_identity = record.source_identity;
    it->type_identity = record.type_identity;
    it->sample_inputs = project_virtual_sample_ports(
        record.sample_inputs, node_bundles, connections, true);
    it->sample_outputs = project_virtual_sample_ports(
        record.sample_outputs, node_bundles, connections, false);
    it->event_inputs = project_virtual_event_ports(
        record.event_inputs, node_bundles, connections, true);
    it->event_outputs = project_virtual_event_ports(
        record.event_outputs, node_bundles, connections, false);

    it->members.clear();
    it->backing_node_ids.clear();
    if (record.type_identity == "sample-port"
        || record.type_identity == "event-port") {
      it->kind = record.type_identity == "sample-port"
          ? "Sample port" : "Event port";
      auto const backing_id = record.type_identity + ":" + record.id;
      it->backing_node_ids.push_back(backing_id);
      it->members.push_back(IntrospectionVirtualNode::Member{
          .ordinal = 0,
          .backing_node_id = backing_id,
          .kind = it->kind,
          .type_identity = record.type_identity,
          .sample_inputs = it->sample_inputs,
          .sample_outputs = it->sample_outputs,
          .event_inputs = it->event_inputs,
          .event_outputs = it->event_outputs,
      });
      continue;
    }
    for (size_t bundle_ordinal = 0;
         bundle_ordinal < record.node_bundle_handles.size(); ++bundle_ordinal) {
      auto const handle = record.node_bundle_handles[bundle_ordinal];
      auto const& bundle = node_bundles.bundle(handle);
      auto const [kind, type_identity] = bundle_display_type(bundle);
      if (it->kind.empty()) it->kind = kind;
      if (it->type_identity.empty()) it->type_identity = type_identity;
      auto const backing_id = "node-bundle:" + record.id + ":" +
                              decimal_string(bundle_ordinal);
      it->backing_node_ids.push_back(backing_id);
      it->members.push_back(IntrospectionVirtualNode::Member{
          .ordinal = bundle_ordinal,
          .backing_node_id = backing_id,
          .kind = kind,
          .type_identity = type_identity,
          .sample_inputs = project_bundle_sample_ports(
              record.sample_inputs, node_bundles, connections, handle, true),
          .sample_outputs = project_bundle_sample_ports(
              record.sample_outputs, node_bundles, connections, handle, false),
          .event_inputs = project_bundle_event_ports(
              record.event_inputs, node_bundles, connections, handle, true),
          .event_outputs = project_bundle_event_ports(
              record.event_outputs, node_bundles, connections, handle, false),
      });
    }
  }
}
} // namespace details
} // namespace iv
