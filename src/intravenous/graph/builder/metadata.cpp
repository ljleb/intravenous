#include <intravenous/graph/builder/metadata.h>

#include <intravenous/graph/builder/connections.h>
#include <intravenous/graph/builder/node_bundles.h>
#include <intravenous/graph/builder/virtual_nodes.h>

namespace iv::details {

VirtualPortConnectivity
aggregate_connectivity(std::span<VirtualConcretePortInfo const> ports) {
  bool any_connected = false;
  bool any_disconnected = false;
  for (auto const &port : ports) {
    any_connected = any_connected || port.connected;
    any_disconnected = any_disconnected || !port.connected;
  }
  if (any_connected && any_disconnected) {
    return VirtualPortConnectivity::mixed;
  }
  return any_connected ? VirtualPortConnectivity::connected
                       : VirtualPortConnectivity::disconnected;
}

void sort_and_deduplicate_spans(std::vector<SourceSpan> &spans) {
  std::sort(spans.begin(), spans.end(), [](auto const &a, auto const &b) {
    return std::tie(a.file_path, a.begin, a.end) <
           std::tie(b.file_path, b.begin, b.end);
  });
  spans.erase(std::unique(spans.begin(), spans.end()), spans.end());
}

std::vector<SourceSpan>
source_spans_for(std::span<VirtualConcreteNode const *const> nodes) {
  std::vector<SourceSpan> spans;
  for (auto const *node : nodes) {
    for (auto const &info : node->source_infos) {
      if (info.span.file_path.empty() || info.span.begin > info.span.end) {
        continue;
      }
      spans.push_back(info.span);
    }
  }
  sort_and_deduplicate_spans(spans);
  return spans;
}

VirtualMetadataBuildResult
build_virtual_metadata(PreparedGraph const &g,
                       std::span<LoweredSubgraphSpec const> lowered_scopes) {
  auto sample_input_connected = [&](size_t node, size_t port) {
    return std::ranges::any_of(g.edges, [&](GraphEdge const &edge) {
      return edge.target.node == node && edge.target.port == port &&
             !g.timeline_filled_input_ports.contains(edge.target);
    });
  };
  auto sample_output_connected = [&](size_t node, size_t port) {
    return std::ranges::any_of(g.edges, [&](GraphEdge const &edge) {
      return edge.source.node == node && edge.source.port == port;
    });
  };
  auto event_input_connected = [&](size_t node, size_t port) {
    return std::ranges::any_of(g.event_edges, [&](GraphEventEdge const &edge) {
      return edge.target.node == node && edge.target.port == port &&
             !g.timeline_filled_event_input_ports.contains(edge.target);
    });
  };
  auto event_output_connected = [&](size_t node, size_t port) {
    return std::ranges::any_of(g.event_edges, [&](GraphEventEdge const &edge) {
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
                        : demangle_type_name(g.nodes[node_i].type_name());
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
    auto const &scope = lowered_scopes[scope_i];
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
    for (auto const &info : scope.source_infos) {
      if (info.declaration_identity.empty()) {
        continue;
      }
      if (!std::ranges::contains(virtual_node_ids, info.declaration_identity)) {
        virtual_node_ids.push_back(info.declaration_identity);
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
    for (auto const &virtual_node_id : concrete_node_virtual_ids[i]) {
      auto group_it =
          std::find_if(groups.begin(), groups.end(), [&](auto const &group) {
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
    auto const &group = groups[group_i];
    std::vector<VirtualConcreteNode const *> members;
    members.reserve(group.member_indices.size());
    auto member_indices = group.member_indices;
    std::sort(member_indices.begin(), member_indices.end(),
              [&](auto a, auto b) {
                auto const &left = concrete_nodes[a];
                auto const &right = concrete_nodes[b];
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
      auto const &backing_node_id = concrete_nodes[member_index].id;
      if (!std::ranges::contains(backing_node_ids, backing_node_id)) {
        backing_node_ids.push_back(backing_node_id);
      }
      virtual_members.push_back(IntrospectionVirtualNode::Member{
          .ordinal = virtual_members.size(),
          .backing_node_id = backing_node_id,
          .kind = concrete_nodes[member_index].kind,
          .type_identity = concrete_nodes[member_index].type_identity,
          .sample_inputs = aggregate_ports(
              std::span<VirtualConcreteNode const *const>(&members.back(), 1),
              &VirtualConcreteNode::sample_inputs),
          .sample_outputs = aggregate_ports(
              std::span<VirtualConcreteNode const *const>(&members.back(), 1),
              &VirtualConcreteNode::sample_outputs),
          .event_inputs = aggregate_ports(
              std::span<VirtualConcreteNode const *const>(&members.back(), 1),
              &VirtualConcreteNode::event_inputs),
          .event_outputs = aggregate_ports(
              std::span<VirtualConcreteNode const *const>(&members.back(), 1),
              &VirtualConcreteNode::event_outputs),
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
            [](auto const &a, auto const &b) {
              auto const a_file = a.source_spans.empty()
                                      ? std::string{}
                                      : a.source_spans.front().file_path;
              auto const b_file = b.source_spans.empty()
                                      ? std::string{}
                                      : b.source_spans.front().file_path;
              if (a_file != b_file) {
                return a_file < b_file;
              }
              auto const a_begin =
                  a.source_spans.empty() ? 0u : a.source_spans.front().begin;
              auto const b_begin =
                  b.source_spans.empty() ? 0u : b.source_spans.front().begin;
              if (a_begin != b_begin) {
                return a_begin < b_begin;
              }
              auto const a_end =
                  a.source_spans.empty() ? 0u : a.source_spans.front().end;
              auto const b_end =
                  b.source_spans.empty() ? 0u : b.source_spans.front().end;
              if (a_end != b_end) {
                return a_end < b_end;
              }
              if (a.kind != b.kind) {
                return a.kind < b.kind;
              }
              if (a.id != b.id) {
                return a.id < b.id;
              }
              if (a.source_identity != b.source_identity) {
                return a.source_identity < b.source_identity;
              }
              return a.backing_node_ids < b.backing_node_ids;
            });

  std::unordered_map<std::string, std::vector<std::string>>
      virtual_node_ids_by_backing_node_id;
  for (auto const &virtual_node : virtual_nodes) {
    for (auto const &backing_node_id : virtual_node.backing_node_ids) {
      virtual_node_ids_by_backing_node_id[backing_node_id].push_back(
          virtual_node.id);
    }
  }
  for (auto &[_, virtual_ids] : virtual_node_ids_by_backing_node_id) {
    std::sort(virtual_ids.begin(), virtual_ids.end());
    virtual_ids.erase(std::unique(virtual_ids.begin(), virtual_ids.end()),
                      virtual_ids.end());
  }

  return std::make_pair(std::move(virtual_nodes),
                        std::move(virtual_node_ids_by_backing_node_id));
}

namespace {
bool sample_channel_is_connected(
    GraphBuilderConnections const& connections, SampleInputChannelId channel) {
  return connections.sample_input_is_connected(channel);
}

bool sample_channel_is_connected(
    GraphBuilderConnections const& connections, SampleOutputChannelId channel) {
  return connections.sample_output_is_connected(channel);
}

template <class Mapping>
std::vector<IntrospectionPortInfo> project_virtual_sample_ports(
    std::vector<Mapping> const &mappings,
    GraphBuilderNodeBundles const &node_bundles,
    GraphBuilderConnections const &connections, bool inputs) {
  std::vector<IntrospectionPortInfo> result;
  result.reserve(mappings.size());
  for (auto const &mapping : mappings) {
    if (mapping.channels.empty()) {
      continue;
    }

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
std::vector<IntrospectionPortInfo> project_bundle_sample_ports(
    std::vector<Mapping> const &mappings,
    GraphBuilderNodeBundles const &node_bundles,
    GraphBuilderConnections const &connections, NodeBundleHandle bundle_handle,
    bool inputs) {
  std::vector<Mapping> bundle_mappings;
  bundle_mappings.reserve(mappings.size());
  for (auto const &mapping : mappings) {
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

std::vector<IntrospectionPortInfo> project_bundle_event_ports(
    GraphBuilderNodeBundles const &node_bundles,
    GraphBuilderConnections const &connections,
    NodeBundleHandle bundle_handle, bool inputs) {
  auto const &bundle = node_bundles.bundle(bundle_handle);
  auto project = [&](size_t count) {
    std::vector<IntrospectionPortInfo> result;
    result.reserve(count);
    for (size_t ordinal = 0; ordinal < count; ++ordinal) {
      std::string name;
      EventTypeId type = EventTypeId::empty;
      bool connected = false;
      if (inputs) {
        auto const config = node_bundles.resolve_event_input(
            {bundle_handle, PortKind::event, ordinal}).config;
        name = config.name;
        type = config.type;
        connected = connections.event_input_is_connected(
            EventInputPortId{bundle_handle, ordinal});
      } else {
        auto const config = node_bundles.resolve_event_output(
            {bundle_handle, PortKind::event, ordinal}).config;
        name = config.name;
        type = config.type;
        connected = connections.event_output_is_connected(
            EventOutputPortId{bundle_handle, ordinal});
      }

      result.push_back(IntrospectionPortInfo{
          .name = std::move(name), .type = event_type_name(type),
          .connectivity = connected ? VirtualPortConnectivity::connected
                                    : VirtualPortConnectivity::disconnected,
          .ordinal = ordinal});
    }
    return result;
  };
  return project(inputs ? bundle.event_input_count() : bundle.event_output_count());
}

std::pair<std::string, std::string> bundle_display_type(
    NodeBundle const &bundle) {
  auto const type = std::string(bundle.type_identity());
  return {type, type};
}
} // namespace

void apply_virtual_port_metadata(
    GraphIntrospectionMetadata &metadata,
    GraphBuilderNodeBundles const &node_bundles,
    GraphBuilderVirtualNodes const &virtual_nodes,
    GraphBuilderConnections const &connections) {
  for (auto const &record : virtual_nodes.records()) {
    auto it = std::find_if(metadata.virtual_nodes.begin(),
                           metadata.virtual_nodes.end(),
                           [&](auto const &node) { return node.id == record.id; });
    if (it == metadata.virtual_nodes.end()) {
      std::vector<SourceSpan> spans;
      spans.reserve(record.source_infos.size());
      for (auto const &info : record.source_infos) spans.push_back(info.span);
      sort_and_deduplicate_spans(spans);
      metadata.virtual_nodes.push_back(IntrospectionVirtualNode{
          .id = record.id,
          .source_identity = record.id,
          .source_spans = std::move(spans),
      });
      it = std::prev(metadata.virtual_nodes.end());
    }
    it->sample_inputs = project_virtual_sample_ports(
        record.sample_inputs, node_bundles, connections, true);
    it->sample_outputs = project_virtual_sample_ports(
        record.sample_outputs, node_bundles, connections, false);

    // The execution graph may contain several concrete nodes for one tiled
    // NodeBundle.  The UI's "concrete member" is deliberately the bundle,
    // never one of those tiles.
    it->members.clear();
    it->backing_node_ids.clear();
    for (size_t bundle_ordinal = 0;
         bundle_ordinal < record.node_bundle_handles.size(); ++bundle_ordinal) {
      auto const handle = record.node_bundle_handles[bundle_ordinal];
      auto const &bundle = node_bundles.bundle(handle);
      auto const [kind, type_identity] = bundle_display_type(bundle);
      if (it->kind.empty()) it->kind = kind;
      if (it->type_identity.empty()) it->type_identity = type_identity;
      auto const backing_id = "node-bundle:" + record.id + ":" +
                              std::to_string(bundle_ordinal);
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
              node_bundles, connections, handle, true),
          .event_outputs = project_bundle_event_ports(
              node_bundles, connections, handle, false),
      });
    }
  }
}
} // namespace iv::details
