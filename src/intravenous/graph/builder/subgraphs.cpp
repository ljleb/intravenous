#include <intravenous/graph/builder/subgraphs.h>
#include <intravenous/graph/builder.h>

#include <algorithm>
#include <limits>
#include <unordered_map>

namespace iv {
namespace {
void append_scope_source_info(ScopedSubgraph& scope, SourceInfo info)
{
    if (std::find(scope.source_infos.begin(), scope.source_infos.end(), info)
        == scope.source_infos.end()) {
        scope.source_infos.push_back(std::move(info));
    }
}

template<class Descriptor>
TopologyPortId single_boundary_projection(Descriptor const& descriptor,
                                          std::string_view kind)
{
    if (descriptor.endpoints.size() != 1) {
        details::error(
            "active subgraph " + std::string(kind)
            + " input does not have exactly one topology projection");
    }
    return descriptor.endpoints.front();
}
} // namespace

bool SubgraphScopeManager::active() const
{
    return !_stack.empty();
}

ScopedSubgraph& SubgraphScopeManager::current()
{
    IV_ASSERT(!_stack.empty(), "current() requires an active subgraph scope");
    return _stack.back();
}

void SubgraphScopeManager::begin(size_t start_node_index, std::string_view kind,
                                 GraphBuilderNodeBundles& node_bundles)
{
    _stack.push_back(ScopedSubgraph{
        .start_node_index = start_node_index,
        .kind = std::string(kind),
        .boundary = node_bundles.append_scope_boundary(),
    });
}

ScopedSubgraph SubgraphScopeManager::finish()
{
    ScopedSubgraph scope = std::move(_stack.back());
    _stack.pop_back();
    return scope;
}

void SubgraphScopeManager::abandon_top()
{
    _stack.pop_back();
}

SamplePortRef SubgraphScopeManager::add_scope_sample_input(
    GraphBuilder& builder,
    GraphBuilderTopology& topology,
    GraphBuilderNodeBundles& node_bundles,
    std::string_view name,
    Sample default_value,
    std::optional<Sample> min,
    std::optional<Sample> max,
    bool has_name
)
{
    auto& scope = current();
    ScopeBoundaryPortId const topology_boundary = topology.append_scope_sample_input(
        OutputConfig{ .name = has_name ? std::string(name) : std::string{} });
    auto const ordinal = node_bundles.bundle(scope.boundary)
        .append_boundary_sample_input(
            InputConfig{
                .name = has_name ? std::string(name) : std::string{},
                .default_value = default_value,
                .min = min.value_or(-std::numeric_limits<Sample::storage>::infinity()),
                .max = max.value_or(std::numeric_limits<Sample::storage>::infinity()),
            },
            topology_boundary.topology_port());
    return SamplePortRef(builder, NodeBundlePortId{
        scope.boundary, PortKind::sample, ordinal});
}

EventPortRef SubgraphScopeManager::add_scope_event_input(
    GraphBuilder& builder,
    GraphBuilderTopology& topology,
    GraphBuilderNodeBundles& node_bundles,
    std::string_view name,
    EventTypeId type,
    bool has_name
)
{
    auto& scope = current();
    ScopeBoundaryPortId const topology_boundary = topology.append_scope_event_input(
        EventOutputConfig{
            .name = has_name ? std::string(name) : std::string{},
            .type = type,
        });
    auto config = has_name
        ? EventInputConfig{ .name = std::string(name), .type = type }
        : EventInputConfig{ .type = type };
    node_bundles.bundle(scope.boundary).append_boundary_event_input(
        std::move(config), topology_boundary.topology_port());
    return EventPortRef(builder, topology_boundary);
}

void SubgraphScopeManager::annotate_scope_input_source_info(
    NodeBundlePortId boundary,
    GraphBuilderNodeBundles const& node_bundles,
    SourceInfo info)
{
    auto& scope = current();
    if (boundary.port_kind != PortKind::sample
        || boundary.node_bundle_handle != scope.boundary
        || boundary.port_ordinal
            >= node_bundles.bundle(scope.boundary).boundary_sample_inputs().size()) {
        details::error("attempted to annotate a scope input from another scope");
    }
    append_scope_source_info(scope, std::move(info));
}

void SubgraphScopeManager::annotate_scope_input_source_info(
    ScopeBoundaryPortId boundary,
    GraphBuilderNodeBundles const& node_bundles,
    SourceInfo info)
{
    auto& scope = current();
    bool found = false;
    if (boundary.port_kind == PortKind::sample) {
        auto const sample_inputs =
            node_bundles.bundle(scope.boundary).boundary_sample_inputs();
        for (size_t ordinal = 0; ordinal < sample_inputs.size(); ++ordinal) {
            auto const descriptor = node_bundles.resolve_sample_output(NodeBundlePortId{
                scope.boundary, PortKind::sample, ordinal});
            found = std::find(descriptor.endpoints.begin(), descriptor.endpoints.end(),
                              boundary.topology_port()) != descriptor.endpoints.end();
            if (found) break;
        }
    } else {
        auto const event_inputs =
            node_bundles.bundle(scope.boundary).boundary_event_inputs();
        for (size_t ordinal = 0; ordinal < event_inputs.size(); ++ordinal) {
            auto const descriptor = node_bundles.resolve_event_output(NodeBundlePortId{
                scope.boundary, PortKind::event, ordinal});
            found = std::find(descriptor.endpoints.begin(), descriptor.endpoints.end(),
                              boundary.topology_port()) != descriptor.endpoints.end();
            if (found) break;
        }
    }
    if (!found) {
        details::error("attempted to annotate a scope input from another scope");
    }
    append_scope_source_info(scope, std::move(info));
}

void SubgraphScopeManager::define_sample_outputs(
    std::span<OutputRefConfig const> refs,
    GraphBuilder& builder,
    GraphBuilderTopology const&,
    GraphBuilderNodeBundles& node_bundles,
    GraphBuilderIdentity const& identity
)
{
    auto& scope = current();
    auto& boundary = node_bundles.bundle(scope.boundary);
    bool const require_names = refs.size() > 1;

    for (size_t i = 0; i < refs.size(); ++i) {
        auto const& ref = refs[i].ref;
        auto const& config = refs[i].config;
        if (ref.graph_builder != &builder) {
            details::error(
                "builder " + identity.value + ": subgraph outputs(...): "
                "SamplePortRef at index " + std::to_string(i) + " "
                "belongs to another builder"
            );
        }
        if (require_names && config.name.empty()) {
            details::error(
                "builder " + identity.value
                + ": subgraph outputs(...) requires names when exposing more than one sample output"
            );
        }
        auto const output_ordinal =
            boundary.append_boundary_sample_output(config);
        builder.record_authored_sample_connection(
            NodeBundlePortId{
                scope.boundary, PortKind::sample, output_ordinal},
            ref);
    }
    scope.outputs_defined = true;
}

void SubgraphScopeManager::define_event_outputs(
    std::span<EventOutputRefConfig const> refs,
    GraphBuilder& builder,
    GraphBuilderTopology const&,
    GraphBuilderNodeBundles& node_bundles,
    GraphBuilderIdentity const& identity,
    std::span<EventInputConfig const>
)
{
    auto& scope = current();
    if (scope.event_outputs_defined) {
        details::error("subgraph event_outputs(...) was already called on builder " + identity.value);
    }

    auto& boundary = node_bundles.bundle(scope.boundary);
    boundary.clear_boundary_event_outputs();
    bool const require_names = refs.size() > 1;

    for (size_t i = 0; i < refs.size(); ++i) {
        auto const& ref = refs[i].ref;
        auto const& config = refs[i].config;
        if (ref.graph_builder != &builder) {
            details::error(
                "builder " + identity.value + ": subgraph event_outputs(...): "
                "EventPortRef at index " + std::to_string(i) + " "
                "belongs to another builder"
            );
        }
        if (require_names && config.name.empty()) {
            details::error(
                "builder " + identity.value
                + ": subgraph event_outputs(...) requires names when exposing more than one event output"
            );
        }
        auto const source_type = ref.type;
        auto output = config;
        output.type = source_type;
        auto const output_ordinal =
            boundary.append_boundary_event_output(std::move(output));
        builder.record_authored_event_connection(
            NodeBundlePortId{
                scope.boundary, PortKind::event, output_ordinal},
            ref);
    }
    scope.event_outputs_defined = true;
}

NodeRef SubgraphScopeManager::finalize_scope(GraphBuilder& builder,
                                              GraphBuilderTopology& topology,
                                              GraphBuilderNodeBundles& node_bundles,
                                              ScopedSubgraph scope)
{
    auto const& boundary = node_bundles.bundle(scope.boundary);
    auto const sample_inputs = boundary.boundary_sample_inputs();
    auto const sample_outputs = boundary.boundary_sample_outputs();
    auto const event_inputs = boundary.boundary_event_inputs();
    auto const event_outputs = boundary.boundary_event_outputs();

    std::unordered_map<TopologyPortId, size_t> sample_input_index_by_boundary;
    sample_input_index_by_boundary.reserve(sample_inputs.size());
    for (size_t i = 0; i < sample_inputs.size(); ++i) {
        auto const descriptor = node_bundles.resolve_sample_output(NodeBundlePortId{
            scope.boundary, PortKind::sample, i});
        sample_input_index_by_boundary.emplace(
            single_boundary_projection(descriptor, "sample"), i);
    }

    std::unordered_map<TopologyPortId, size_t> event_input_index_by_boundary;
    event_input_index_by_boundary.reserve(event_inputs.size());
    for (size_t i = 0; i < event_inputs.size(); ++i) {
        auto const descriptor = node_bundles.resolve_event_output(NodeBundlePortId{
            scope.boundary, PortKind::event, i});
        event_input_index_by_boundary.emplace(
            single_boundary_projection(descriptor, "event"), i);
    }

    std::vector<std::vector<TopologyPortId>> subgraph_input_targets(sample_inputs.size());
    std::vector<TopologyPortId> subgraph_output_sources(sample_outputs.size());
    std::vector<std::vector<TopologyPortId>> subgraph_event_input_targets(event_inputs.size());
    std::vector<TopologyPortId> subgraph_event_output_sources(event_outputs.size());

    topology.for_each_sample_edge([&](TopologyEdge const& edge) {
        auto const it = sample_input_index_by_boundary.find(edge.source);
        if (it == sample_input_index_by_boundary.end()) {
            return;
        }
        subgraph_input_targets[it->second].push_back(edge.target);
    });
    topology.erase_sample_edges_matching([&](TopologyEdge const& edge) {
        return sample_input_index_by_boundary.contains(edge.source);
    });

    topology.for_each_event_edge([&](TopologyEventEdge const& edge) {
        auto const it = event_input_index_by_boundary.find(edge.source);
        if (it == event_input_index_by_boundary.end()) {
            return;
        }
        subgraph_event_input_targets[it->second].push_back(edge.target);
    });
    topology.erase_event_edges_matching([&](TopologyEventEdge const& edge) {
        return event_input_index_by_boundary.contains(edge.source);
    });

    size_t const subgraph_node = topology.append_lowered_subgraph_node(
        std::move(scope.kind),
        std::vector<InputConfig>(sample_inputs.begin(), sample_inputs.end()),
        std::vector<OutputConfig>(sample_outputs.begin(), sample_outputs.end()),
        std::vector<EventInputConfig>(event_inputs.begin(), event_inputs.end()),
        std::vector<EventOutputConfig>(event_outputs.begin(), event_outputs.end()),
        scope.start_node_index,
        topology.node_count() - scope.start_node_index,
        std::move(subgraph_input_targets),
        std::move(subgraph_output_sources),
        std::move(subgraph_event_input_targets),
        std::move(subgraph_event_output_sources)
    );

    NodeRef result(
        builder, node_bundles.append_subgraph(
            topology, subgraph_node, scope.boundary));
    for (auto const& info : scope.source_infos) {
        result._annotate_source_info(
            info.declaration_identity, info.span.file_path,
            info.span.begin, info.span.end);
    }
    return result;
}

size_t SubgraphScopeManager::current_start_node_index() const
{
    return _stack.back().start_node_index;
}
}
