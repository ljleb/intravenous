#pragma once

// Private implementation for libiv_builder only. Do not include from a
// module TU.
#include <intravenous/graph/builder.h>
#include <intravenous/graph/builder/embedding.h>
#include <intravenous/basic_nodes/arithmetic.h>
#include <intravenous/basic_nodes/routing.h>
#include <intravenous/channel_ports.h>
#include <intravenous/graph/builder/annotations.hpp>
#include <intravenous/graph/configured_graph.hpp>
#include <intravenous/graph/builder/connections.hpp>
#include <intravenous/graph/builder/identity.h>
#include <intravenous/graph/builder/mark.h>
#include <intravenous/graph/builder/node_refs.h>
#include <intravenous/graph/builder/node_bundles.hpp>
#include <intravenous/graph/reflected_node_description.h>
#include <intravenous/graph/builder/public_ports.hpp>
#include <intravenous/graph/builder/snapshot_error.h>
#include <intravenous/graph/builder/subgraphs.hpp>
#include <intravenous/graph/builder/virtual_nodes.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cxxabi.h>
#include <expected>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace iv {


// A closed copy of the graph contributed by one registered-definition
// invocation. Bundle 0 is an empty boundary; bundles 1.. are the invocation's
// contents in construction order, and `local_root` is its result.
struct GraphBuilderInvocationSnapshot {
  ConfiguredGraph graph{};
  NodeBundleHandle local_root = 0;
};

class GraphBuilderState {
  friend class GraphBuilderChildEmbedder;
  friend class GraphBuilderPublicPorts;

public:
  struct ScopeFrame {
    details::BuilderScopeId id = details::root_builder_scope_id;
    details::BuilderScopeKind kind = details::BuilderScopeKind::root;
    GraphBuilderPublicPorts ports{0};
    // Root and definition scopes own a virtual-record namespace. Subgraph
    // scopes share the namespace of their enclosing definition, so repeated
    // declarations of one module source group as they always have.
    std::uint64_t virtual_namespace = 0;
    // First bundle of the namespace owner. Frozen embeddings qualify their
    // records relative to it, so the qualification is the same wherever the
    // owning module is constructed or imported.
    NodeBundleHandle namespace_begin = 0;
    // Records created while this scope is open start here.
    std::size_t record_begin = 0;
  };

  explicit GraphBuilderState(
      details::BuilderSession* session,
      GraphBuilderIdentity identity = GraphBuilderIdentity("root"));

  [[nodiscard]] details::BuilderSession* session() const noexcept { return _session; }

  [[nodiscard]] GraphBuilderMark mark() const noexcept;

  // ---- scopes ------------------------------------------------------------
  details::BuilderScopeId push_scope(details::BuilderScopeKind);
  // Finalizes the top scope in place and returns its subgraph bundle. The
  // records created inside a definition scope, including the declarations of
  // its public inputs, are qualified by `qualifier`; without one they are
  // qualified by the subgraph's handle relative to the enclosing namespace,
  // exactly as embed_configured_graph() qualifies a frozen placement.
  NodeBundleHandle finish_scope(
      details::BuilderScopeId, std::string_view kind,
      std::optional<std::string_view> qualifier);
  [[nodiscard]] std::vector<SourceInfo> subgraph_boundary_source_infos(
      NodeBundleHandle subgraph) const;
  [[nodiscard]] bool scope_is_open(details::BuilderScopeId) const noexcept;
  [[nodiscard]] details::BuilderScopeId current_scope_id() const noexcept;
  [[nodiscard]] std::size_t open_scope_count() const noexcept { return _scopes.size(); }
  [[nodiscard]] bool scope_sample_outputs_defined(details::BuilderScopeId) const;
  [[nodiscard]] NodeBundleHandle scope_boundary(details::BuilderScopeId) const;

  PublicSampleInputRef scope_input(details::BuilderScopeId, ContinuousInputConfig);
  PublicEventInputRef scope_event_input(
      details::BuilderScopeId, std::string_view name, EventTypeId);
  void scope_outputs(details::BuilderScopeId, std::span<SampleOutputRequest const>);
  void scope_outputs(details::BuilderScopeId, std::span<NamedRef const>);
  void scope_event_outputs(details::BuilderScopeId, std::span<EventOutputRequest const>);
  void annotate_scope_sample_output_source_info(
      details::BuilderScopeId, std::span<SourceInfo const>);
  void annotate_scope_event_output_source_info(
      details::BuilderScopeId, std::span<SourceInfo const>);
  void annotate_scope_sample_output_source_info(
      details::BuilderScopeId, std::size_t index, SourceInfo);
  void annotate_scope_event_output_source_info(
      details::BuilderScopeId, std::size_t index, SourceInfo);

  // ---- node construction -------------------------------------------------
  NodeBundleHandle append_node_description(ReflectedNodeDescription);
  NodeBundleHandle append_tiled_node_description(
      ReflectedNodeDescription const&, ChannelLayout);
  NodeBundleHandle append_tiled_node_bundles(
      std::span<NodeBundleHandle const>, ChannelLayout);
  void set_registered_node_type_identity(
      NodeBundleHandle, RegisteredNodeTypeIdentity);
  // Members are finished definition-scope subgraphs of one tiled invocation.
  void validate_tiled_module_interfaces(std::span<NodeBundleHandle const>) const;

  // ---- frozen graphs -----------------------------------------------------
  // Imports a closed graph as one new subgraph scope of the current scope.
  ConfiguredGraphEmbedding embed_configured_graph(
      ConfiguredGraph const& child, std::string_view kind = "Configured graph");
  // Copies the contents of an invocation (bundles appended since `begin`)
  // into a closed graph. Returns nothing when the contents are not
  // independently representable.
  [[nodiscard]] std::optional<GraphBuilderInvocationSnapshot> snapshot_contents(
      GraphBuilderMark const& begin, NodeBundleHandle result) const;
  // Places a snapshot produced by snapshot_contents into the current scope
  // without a synthetic wrapper. Returns the live handle of `local_root`.
  NodeBundleHandle import_contents(
      ConfiguredGraph const&, NodeBundleHandle local_root, std::string_view qualifier);
  // Non-destructive closedness-checked copy of an open scope.
  [[nodiscard]] std::expected<ConfiguredGraph, ConfiguredGraphSnapshotError>
  snapshot_scope(details::BuilderScopeId) const;
  void qualify_virtual_records(std::size_t begin, std::string_view qualifier);

  // ---- refs and connections ---------------------------------------------
  [[nodiscard]] SamplePortRef require_sample_port(SamplePortRef const&) const;
  [[nodiscard]] EventPortRef require_event_port(EventPortRef const&) const;
  void connect_sample_input(NodeBundlePortId target, SamplePortRef const& source);
  void connect_sample_input(SampleInputChannelId target, SamplePortRef const& source);
  void connect_sample_input(NodeBundlePortId target, std::span<SamplePortRef const> sources);
  void connect_event_input(NodeBundlePortId target, EventPortRef const& source);
  bool sample_input_is_connected(NodeBundlePortId target) const;
  bool event_input_is_connected(NodeBundlePortId target) const;
  size_t sample_port_index(NodeBundleHandle, bool inputs, std::string_view name) const;
  size_t event_port_index(NodeBundleHandle, bool inputs, std::string_view name) const;
  size_t sample_input_count(NodeBundleHandle) const;
  size_t sample_output_count(NodeBundleHandle) const;
  size_t event_input_count(NodeBundleHandle) const;
  size_t event_output_count(NodeBundleHandle) const;
  NodeBundlePortId input_port_at(NodeBundleHandle, size_t) const;
  ContinuousInputConfig sample_input_config(NodeBundleHandle, size_t) const;
  EventInputConfig event_input_config(NodeBundleHandle, size_t) const;
  NodeBundleHandle tiled_member(NodeBundleHandle, size_t) const;
  NodePorts const& typed_ports(NodeBundleHandle) const;
  SamplePortRef sample_port_from_output(NodeBundlePortId);
  EventPortRef event_port_from_output(NodeBundlePortId);
  SamplePortRef make_sample_port(ChannelTypeId, std::span<SampleOutputChannelId const>);
  SamplePortRef make_tiled_sample_port(ChannelTypeId, std::span<SamplePortRef const>);
  SamplePortRef select_sample_port_channel(SamplePortRef const&, size_t);
  std::span<SampleOutputChannelId const> sample_port_channels(SamplePortRef const&) const;
  EventPortRef make_event_port(EventTypeId, std::span<EventOutputPortId const>);
  EventPortRef make_event_port(std::span<EventPortRef const>);
  std::span<EventOutputPortId const> event_port_sources(EventPortRef const&) const;
  SamplePortRef detach_sample_port(
      SamplePortRef const&, size_t loop_extra_latency, std::optional<Sample> initial_value);
  EventPortRef detach_event_port(EventPortRef const&, size_t loop_extra_latency);
  void apply_ttl(NodeBundleHandle, size_t);

  // ---- annotations ------------------------------------------------------
  void annotate_node(NodeBundleHandle, std::string_view, std::string_view,
      uint32_t, uint32_t);
  void annotate_node_input_source_info(NodeBundleHandle, PortKind,
      std::string_view, std::string_view, std::string_view, uint32_t, uint32_t);
  void annotate_public_sample_input_source_info(SamplePortRef const&,
      std::string_view, std::string_view, uint32_t, uint32_t);
  void annotate_public_event_input_source_info(EventPortRef const&,
      std::string_view, std::string_view, uint32_t, uint32_t);
  void annotate_sample_port_source_info(SamplePortRef const&,
      std::string_view, std::string_view, uint32_t, uint32_t);
  void annotate_event_port_source_info(EventPortRef const&,
      std::string_view, std::string_view, uint32_t, uint32_t);

  // ---- host introspection -----------------------------------------------
  using VacantSampleInput = GraphBuilderVacantSampleInput;
  using VacantEventInput = GraphBuilderVacantEventInput;
  using VacantInputs = GraphBuilderVacantInputs;
  using VirtualSampleInput = GraphBuilderVirtualSampleInput;
  using VirtualEventInput = GraphBuilderVirtualEventInput;
  using VirtualInputs = GraphBuilderVirtualInputs;
  using VirtualSampleInputChannel = GraphBuilderVirtualSampleInputChannel;
  using VirtualSampleInputFamily = GraphBuilderVirtualSampleInputFamily;
  using VirtualSampleInputFamilies = GraphBuilderVirtualSampleInputFamilies;
  using VirtualSampleOutput = GraphBuilderVirtualSampleOutput;
  using VirtualEventOutput = GraphBuilderVirtualEventOutput;
  using VirtualOutputs = GraphBuilderVirtualOutputs;
  using VirtualSampleOutputChannel = GraphBuilderVirtualSampleOutputChannel;
  using VirtualSampleOutputFamily = GraphBuilderVirtualSampleOutputFamily;
  using VirtualSampleOutputFamilies = GraphBuilderVirtualSampleOutputFamilies;
  using VirtualPorts = GraphBuilderVirtualPorts;

  VacantInputs vacant_inputs() const;
  VirtualInputs virtual_inputs() const;
  VirtualSampleInputFamilies virtual_sample_input_families() const;
  VirtualOutputs virtual_outputs() const;
  VirtualSampleOutputFamilies virtual_sample_output_families() const;
  VirtualPorts virtual_ports() const;
  GraphBuilderPublicContinuousPortFamilies public_sample_input_families() const;
  bool public_sample_input_is_connected(size_t port_index) const;
  std::vector<GraphBuilderPublicEventInput> public_event_inputs() const;
  bool public_event_input_is_connected(size_t port_index) const;
  std::span<SourceInfo const> public_event_input_source_infos(size_t) const;
  GraphBuilderPublicContinuousPortFamilies public_sample_output_families() const;
  std::vector<GraphBuilderPublicEventOutput> public_event_outputs() const;

  [[nodiscard]] std::size_t bundle_count() const noexcept { return _node_bundles.size(); }
  [[nodiscard]] std::size_t virtual_record_count() const noexcept { return _virtual_nodes.size(); }
  // Live graph tables, for host-side semantic path resolution while the
  // session is still constructing.
  [[nodiscard]] GraphBuilderNodeBundles const& node_bundles() const noexcept { return _node_bundles; }
  [[nodiscard]] GraphBuilderVirtualNodes const& virtual_nodes() const noexcept { return _virtual_nodes; }

  // The root scope is the complete graph. Finishing requires every nested
  // scope to be closed.
  ConfiguredGraph finish() const &;
  ConfiguredGraph finish() &&;

  void record_configured_sample_connection(NodeBundlePortId, SamplePortRef const&);
  void record_configured_sample_connection(SampleInputChannelId, SamplePortRef const&);
  void record_configured_event_connection(NodeBundlePortId, EventPortRef const&);

private:
  struct SamplePortExpression {
    ChannelTypeId channel_type = ChannelTypeId::mono;
    std::vector<SampleOutputChannelId> channels{};
    std::vector<std::optional<ConfiguredSampleConnectionDetach>> detaches{};
  };
  struct EventPortExpression {
    EventTypeId type = EventTypeId::empty;
    std::vector<EventOutputPortId> sources{};
    std::vector<std::optional<ConfiguredEventConnectionDetach>> detaches{};
  };

  ScopeFrame& frame(details::BuilderScopeId);
  ScopeFrame const& frame(details::BuilderScopeId) const;
  std::size_t frame_index(details::BuilderScopeId) const;
  [[nodiscard]] std::uint64_t current_namespace() const noexcept;


  // Namespace-aware get-or-create for live virtual records.
  VirtualNodeHandle virtual_record(std::string_view source_identity, std::string_view type_identity);
  VirtualNodeHandle append_virtual_record(
      std::string_view source_identity, std::string_view type_identity,
      std::uint64_t name_space);
  void index_imported_records(std::size_t begin);
  static std::string virtual_record_key(
      std::uint64_t name_space, std::string_view source_identity,
      std::string_view type_identity);
  void attach_virtual_bundle_member(
      NodeBundleHandle, std::string_view source_identity, SourceInfo const*);
  void attach_virtual_input_source_info(
      NodeBundleHandle, std::string_view source_identity, PortKind,
      std::string_view port_name, SourceInfo const&);

  SamplePortRef lift_named_sample(NamedRef const&);
  std::string relative_scope_qualifier(NodeBundleHandle subgraph) const;

  details::BuilderSession* _session = nullptr;
  GraphBuilderIdentity _identity;
  GraphBuilderNodeBundles _node_bundles;
  GraphBuilderConnections _connections;
  GraphBuilderAnnotations _annotations;
  GraphBuilderVirtualNodes _virtual_nodes;
  // Parallel to the virtual-node records: the live namespace that may still
  // coalesce new members into each record. Imported records receive a
  // namespace of their own and never coalesce with live declarations.
  std::vector<std::uint64_t> _virtual_namespaces;
  std::unordered_map<std::string, VirtualNodeHandle> _virtual_record_index;
  std::vector<ScopeFrame> _scopes;
  details::BuilderScopeId _next_scope_id = 1;
  std::uint64_t _next_namespace = 1;
  // SamplePortRef intentionally stores only an index into this session-owned
  // table. Variable-length expressions never live in module-generated code.
  std::vector<SamplePortExpression> _sample_port_expressions;
  std::vector<EventPortExpression> _event_port_expressions;
};

} // namespace iv
