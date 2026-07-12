#include <intravenous/graph/builder.h>
#include <intravenous/graph/builder/embedder.h>

namespace iv {
SamplePortRef::SamplePortRef(GraphBuilder& graph_builder_, size_t node_index, size_t output_port) :
    graph_builder(&graph_builder_),
    node_index(node_index),
    output_port(output_port)
{
    if (node_index == GRAPH_ID) {
        if (output_port >= graph_builder->_public_ports.sample_inputs().size()) {
            details::error(
                "graph input port " + std::to_string(output_port) + " "
                "is out of bounds in builder " + graph_builder->_identity.value + ", "
                "public_inputs.size() = " + std::to_string(graph_builder->_public_ports.sample_inputs().size())
            );
        }
        return;
    }

    if (node_index >= graph_builder->_topology.node_count()) {
        details::error(
            "node at index " + std::to_string(node_index) + " "
            "is out of bounds in builder " + graph_builder->_identity.value + ", "
            "nodes.size() = " + std::to_string(graph_builder->_topology.node_count())
        );
    }

    auto& node = graph_builder->_topology.node(node_index);
    size_t num_outputs = get_num_outputs(node);
    if (output_port >= num_outputs) {
        details::error(
            "output port " + std::to_string(output_port) + " of "
            "node at index " + std::to_string(node_index) + " in "
            "builder " + graph_builder->_identity.value + " "
            "is out of bounds, get_num_outputs(node) = " + std::to_string(num_outputs)
        );
    }
}

EventPortRef::EventPortRef(GraphBuilder& graph_builder_, size_t node_index, size_t output_port) :
    graph_builder(&graph_builder_),
    node_index(node_index),
    output_port(output_port)
{
    if (node_index == GRAPH_ID) {
        if (output_port >= graph_builder->_public_ports.event_inputs().size()) {
            details::error(
                "graph event input port " + std::to_string(output_port) + " "
                "is out of bounds in builder " + graph_builder->_identity.value
            );
        }
        return;
    }

    if (node_index >= graph_builder->_topology.node_count()) {
        details::error(
            "node at index " + std::to_string(node_index) + " "
            "is out of bounds in builder " + graph_builder->_identity.value
        );
    }

    auto& node = graph_builder->_topology.node(node_index);
    size_t num_outputs = node.ports.event_outputs.size();
    if (output_port >= num_outputs) {
        details::error(
            "event output port " + std::to_string(output_port) + " of "
            "node at index " + std::to_string(node_index) + " in "
            "builder " + graph_builder->_identity.value + " "
            "is out of bounds"
        );
    }
}

SamplePortRef SamplePortRef::_clone_handle() const
{
    if (!graph_builder) {
        return SamplePortRef {};
    }
    return SamplePortRef(*graph_builder, node_index, output_port);
}

SamplePortRef SamplePortRef::detach(size_t loop_extra_latency) const
{
    if (!graph_builder) {
        details::error("attempted to detach an empty sample port");
    }
    return graph_builder->detach_sample_port(*this, loop_extra_latency);
}

std::string SamplePortRef::to_string() const
{
    if (!graph_builder) {
        return "empty sample port";
    }
    if (node_index == GRAPH_ID) {
        return "graph input " + std::to_string(output_port) + " in builder " + graph_builder->_identity.value;
    }
    return "sample port at address " + graph_builder->node_id(node_index) + ":" + std::to_string(output_port);
}

std::string EventPortRef::to_string() const
{
    if (!graph_builder) {
        return "empty event";
    }
    if (node_index == GRAPH_ID) {
        return "graph event input " + std::to_string(output_port) + " in builder " + graph_builder->_identity.value;
    }
    return "event at address " + graph_builder->node_id(node_index) + ":" + std::to_string(output_port);
}

GraphBuilderIdentity::GraphBuilderIdentity(std::string value_) :
    value(std::move(value_))
{
}

std::string GraphBuilderIdentity::child_id(size_t index) const
{
    std::string nested_path = value;
    if (!nested_path.empty()) {
        nested_path += ".";
    }
    nested_path += std::to_string(index);
    return nested_path;
}

GraphBuilder::GraphBuilder(GraphBuilderIdentity identity) :
    _identity(std::move(identity))
{
}

GraphBuilder::GraphBuilder() :
    _identity(allocate_root_builder_id())
{
}

GraphBuilder GraphBuilder::derive_nested_builder()
{
    return GraphBuilder(GraphBuilderIdentity(_identity.child_id(_topology.node_count())));
}

bool GraphBuilder::inside_subgraph_scope() const
{
    return _subgraphs.active();
}

ScopedSubgraph& GraphBuilder::current_scope()
{
    return _subgraphs.current();
}

void GraphBuilder::define_scope_outputs(std::span<OutputRefConfig const> refs)
{
    _subgraphs.define_sample_outputs(refs, *this, _topology, _identity);
}

void GraphBuilder::define_scope_event_outputs(std::span<EventOutputRefConfig const> refs)
{
    _subgraphs.define_event_outputs(refs, *this, _topology, _identity, _public_ports.event_inputs());
}

std::string GraphBuilder::node_id(size_t index) const
{
    IV_ASSERT(index < _topology.node_count(), "node index out of bounds");
    return _identity.child_id(index);
}

PublicSampleInputRef GraphBuilder::input()
{
    return input(Sample{0.0f});
}

PublicSampleInputRef GraphBuilder::input(
    std::string_view name,
    Sample default_value,
    std::optional<Sample> min,
    std::optional<Sample> max)
{
    if (inside_subgraph_scope()) {
        return PublicSampleInputRef(_subgraphs.add_scope_sample_input(*this, _topology, name, default_value, min, max, true));
    }
    return PublicSampleInputRef(_public_ports.add_sample_input(*this, name, default_value, min, max));
}

PublicSampleInputRef GraphBuilder::input(
    Sample default_value,
    std::optional<Sample> min,
    std::optional<Sample> max)
{
    if (inside_subgraph_scope()) {
        return PublicSampleInputRef(_subgraphs.add_scope_sample_input(*this, _topology, {}, default_value, min, max, false));
    }
    return PublicSampleInputRef(_public_ports.add_sample_input(*this, {}, default_value, min, max));
}

void GraphBuilder::annotate_public_sample_input_source_info(
    PublicSampleInputRef const& ref,
    std::string_view declaration_identity,
    std::string_view file_path,
    uint32_t begin,
    uint32_t end)
{
    if (declaration_identity.empty() || ref.port.graph_builder != this) {
        return;
    }
    // Scoped inputs are represented by placeholder nodes until the enclosing
    // scope is lowered.  Annotating that placeholder is deliberately the
    // same path as node annotation: finalization transfers it to the lowered
    // logical scope, making a `g.input()` inside channel/loop scopes queryable
    // at the declaration identifier as well.
    if (ref.port.node_index != GRAPH_ID) {
        NodeRef(*this, ref.port.node_index)._annotate_source_info(
            declaration_identity, file_path, begin, end);
        return;
    }
    _public_ports.annotate_sample_input_source_info(
        ref.port.output_port, declaration_identity, file_path, begin, end);
}

void PublicSampleInputRef::_annotate_source_info(
    std::string_view declaration_identity,
    std::string_view file_path,
    uint32_t begin,
    uint32_t end) const
{
    if (port.graph_builder != nullptr) {
        port.graph_builder->annotate_public_sample_input_source_info(*this, declaration_identity, file_path, begin, end);
    }
}

PublicEventInputRef GraphBuilder::event_input(std::string_view name, EventTypeId type)
{
    if (inside_subgraph_scope()) {
        return PublicEventInputRef(_subgraphs.add_scope_event_input(*this, _topology, name, type, true));
    }
    return PublicEventInputRef(_public_ports.add_event_input(*this, name, type));
}

PublicEventInputRef GraphBuilder::event_input(EventTypeId type)
{
    if (inside_subgraph_scope()) {
        return PublicEventInputRef(_subgraphs.add_scope_event_input(*this, _topology, {}, type, false));
    }
    return PublicEventInputRef(_public_ports.add_event_input(*this, {}, type));
}

void GraphBuilder::annotate_public_event_input_source_info(PublicEventInputRef const& ref,
    std::string_view identity, std::string_view file, uint32_t begin, uint32_t end)
{
    if (identity.empty() || ref.port.graph_builder != this) return;
    if (ref.port.node_index != GRAPH_ID) {
        NodeRef(*this, ref.port.node_index)._annotate_source_info(identity, file, begin, end);
        return;
    }
    _public_ports.annotate_event_input_source_info(ref.port.output_port, identity, file, begin, end);
}

void PublicEventInputRef::_annotate_source_info(std::string_view identity, std::string_view file,
    uint32_t begin, uint32_t end) const
{
    if (port.graph_builder) port.graph_builder->annotate_public_event_input_source_info(*this, identity, file, begin, end);
}

NodeRef GraphBuilder::embed_subgraph(GraphBuilder const& child)
{
    if (!child._public_ports.sample_outputs_defined()) {
        details::error(
            "builder " + child._identity.value + ": g.outputs(...) must be called before insertion"
        );
    }

    size_t const placeholder_node = GraphBuilderChildEmbedder::embed(
        _topology,
        _connections,
        _detach,
        child._public_ports,
        child._topology,
        child._connections,
        child._detach
    );
    return NodeRef(*this, placeholder_node);
}

void GraphBuilder::event_outputs(std::span<EventOutputRefConfig const> refs)
{
    if (inside_subgraph_scope()) {
        define_scope_event_outputs(refs);
        return;
    }
    _public_ports.define_event_outputs(*this, _topology, _identity, refs);
}

void GraphBuilder::outputs(std::initializer_list<NamedRef> refs)
{
    if (inside_subgraph_scope()) {
        _subgraphs.define_sample_outputs_from_named_refs(
            *this,
            _topology,
            _identity,
            [&](auto&& value) {
                return lift_to_sample_port(std::forward<decltype(value)>(value));
            },
            std::span<NamedRef const>(refs.begin(), refs.size())
        );
        return;
    }

    _public_ports.define_sample_outputs_from_named_refs(
        *this,
        _topology,
        _identity,
        [&](auto&& value) {
            return lift_to_sample_port(std::forward<decltype(value)>(value));
        },
        std::span<NamedRef const>(refs.begin(), refs.size())
    );
}

void GraphBuilder::outputs(std::span<OutputRefConfig const> refs)
{
    if (inside_subgraph_scope()) {
        define_scope_outputs(refs);
        return;
    }
    _public_ports.define_sample_outputs(*this, _topology, _identity, refs);
}

void GraphBuilder::outputs(std::span<NamedRef const> refs)
{
    if (inside_subgraph_scope()) {
        _subgraphs.define_sample_outputs_from_named_refs(
            *this,
            _topology,
            _identity,
            [&](auto&& value) {
                return lift_to_sample_port(std::forward<decltype(value)>(value));
            },
            refs
        );
        return;
    }

    _public_ports.define_sample_outputs_from_named_refs(
        *this,
        _topology,
        _identity,
        [&](auto&& value) {
            return lift_to_sample_port(std::forward<decltype(value)>(value));
        },
        refs
    );
}

std::string GraphBuilder::allocate_root_builder_id()
{
    static size_t next_root_builder_id = 0;
    return std::to_string(next_root_builder_id++);
}

SamplePortRef GraphBuilder::detach_sample_port(SamplePortRef const& sample_port, size_t loop_extra_latency)
{
    if (!sample_port.graph_builder) {
        details::error("builder " + _identity.value + ": cannot detach an empty sample port");
    }
    if (sample_port.graph_builder != this) {
        details::error(
            "builder " + _identity.value + ": cannot detach " + sample_port.to_string()
            + " because it belongs to another builder"
        );
    }

    PortId const source = sample_port;
    if (_detach.reader_output_exists(source)) {
        return sample_port;
    }
    if (auto const* existing = _detach.info_for_source(source)) {
        if (existing->loop_extra_latency != loop_extra_latency) {
            details::error(
                "builder " + _identity.value + ": detach loop extra latency conflict on " + sample_port.to_string()
            );
        }
        PortId const reader = existing->reader_output;
        return SamplePortRef(*this, reader.node, reader.port);
    }
    if (loop_extra_latency < 1) {
        details::error("builder " + _identity.value + ": detach loop extra latency must be at least 1");
    }

    size_t const detach_id = _detach.allocate_detach_id();
    auto writer = node<DetachWriterNode>(detach_id, loop_extra_latency);
    (void) writer;
    size_t const writer_node = _topology.node_count() - 1;
    connect_sample_input(PortId{ writer_node, 0 }, sample_port);

    auto reader = node<DetachReaderNode>(detach_id, loop_extra_latency);
    SamplePortRef detached = reader;

    _detach.record_detached_source(source, DetachedSamplePortInfo{
        .detach_id = detach_id,
        .original_source = source,
        .writer_node = writer_node,
        .reader_output = detached,
        .loop_extra_latency = loop_extra_latency,
    });
    return detached;
}

GraphBuilder::VacantInputs GraphBuilder::vacant_inputs() const
{
    return _connections.collect_vacant_inputs(_topology);
}

GraphBuilder::LogicalInputs GraphBuilder::logical_inputs() const
{
    return _connections.collect_logical_inputs(_topology);
}

GraphBuilder::LogicalSampleInputFamilies GraphBuilder::logical_sample_input_families() const
{
    return _connections.collect_logical_sample_input_families(_topology);
}

GraphBuilder::LogicalOutputs GraphBuilder::logical_outputs() const
{
    return _connections.collect_logical_outputs(_topology);
}

GraphBuilder::LogicalSampleOutputFamilies GraphBuilder::logical_sample_output_families() const
{
    return _connections.collect_logical_sample_output_families(_topology);
}

GraphBuilderPublicSamplePortFamilies GraphBuilder::public_sample_input_families() const
{
    return _public_ports.sample_input_families();
}

bool GraphBuilder::public_sample_input_is_connected(size_t port_ordinal) const
{
    bool connected = false;
    _topology.for_each_sample_edge([&](GraphEdge const &edge) {
        connected = connected || (edge.source == PortId{GRAPH_ID, port_ordinal});
    });
    return connected;
}

std::vector<GraphBuilderPublicEventInput> GraphBuilder::public_event_inputs() const
{
    return _public_ports.collected_event_inputs();
}

bool GraphBuilder::public_event_input_is_connected(size_t port_ordinal) const
{
    bool connected = false;
    _topology.for_each_event_edge([&](GraphEventEdge const &edge) {
        connected = connected || (edge.source == PortId{GRAPH_ID, port_ordinal});
    });
    return connected;
}

std::span<SourceInfo const> GraphBuilder::public_event_input_source_infos(size_t port_ordinal) const
{
    return _public_ports.event_input_source_infos(port_ordinal);
}

GraphBuilderPublicSamplePortFamilies GraphBuilder::public_sample_output_families() const
{
    return _public_ports.sample_output_families();
}

std::vector<GraphBuilderPublicEventOutput> GraphBuilder::public_event_outputs() const
{
    return _public_ports.collected_event_outputs();
}

void GraphBuilder::connect_sample_input(PortId target, SamplePortRef source)
{
    _connections.connect_sample_input(_topology, _identity, target, source);
}

void GraphBuilder::connect_event_input(PortId target, EventPortRef source)
{
    _connections.connect_event_input(_topology, _public_ports.event_inputs(), _identity, target, source);
}

void GraphBuilder::mark_runtime_filled_sample_input(PortId target)
{
    _connections.mark_runtime_filled_sample_input(target);
}

void GraphBuilder::mark_runtime_filled_event_input(PortId target)
{
    _connections.mark_runtime_filled_event_input(target);
}

SamplePortRef GraphBuilder::lift_to_sample_port(SamplePortRef const& sample_port)
{
    if (sample_port.graph_builder != this) {
        details::error(
            "builder " + _identity.value + ": sample port " + sample_port.to_string() + " belongs to another builder"
        );
    }
    return sample_port;
}

SamplePortRef GraphBuilder::lift_to_sample_port(SamplePortRef&& sample_port)
{
    if (sample_port.graph_builder != this) {
        details::error(
            "builder " + _identity.value + ": sample port " + sample_port.to_string() + " belongs to another builder"
        );
    }
    return std::move(sample_port);
}

SamplePortRef GraphBuilder::lift_to_sample_port(NamedRef const& ref)
{
    return std::visit([&](auto const& v) -> SamplePortRef {
        using T = std::remove_cvref_t<decltype(v)>;
        if constexpr (std::same_as<T, EventPortRef>) {
            details::error(
                "builder " + _identity.value + ": expected sample/sample-port value, got EventPortRef"
            );
        } else if constexpr (std::same_as<T, SamplePortRef>) {
            return lift_to_sample_port(v);
        } else {
            return lift_to_sample_port(v);
        }
    }, ref.value);
}

GraphIntrospectionMetadata GraphBuilder::build_metadata(size_t detach_id_offset) const
{
    return GraphBuilderFinalizer::build_metadata(_identity, _topology, detach_id_offset);
}

GraphBuilder::RootNodeBuildResult GraphBuilder::build_root_node(size_t detach_id_offset) const
{
    return GraphBuilderFinalizer::build_root_node(
        _identity,
        _topology,
        _connections,
        _public_ports,
        _detach,
        detach_id_offset
    );
}

GraphBuilder::RootNodeBuildResult GraphBuilder::build_execution_root_node(
    size_t detach_id_offset) const
{
    GraphBuilder execution_builder;
    execution_builder.embed_subgraph(*this);
    execution_builder.outputs({});
    return execution_builder.build_root_node(detach_id_offset);
}

} // namespace iv
