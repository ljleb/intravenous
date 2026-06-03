#include <intravenous/graph/builder/subgraphs.h>

namespace iv {
bool SubgraphScopeManager::active() const
{
    return !_stack.empty();
}

ScopedSubgraph& SubgraphScopeManager::current()
{
    IV_ASSERT(!_stack.empty(), "current() requires an active subgraph scope");
    return _stack.back();
}

void SubgraphScopeManager::begin(size_t start_node_index, std::string_view kind)
{
    _stack.push_back(ScopedSubgraph{
        .start_node_index = start_node_index,
        .kind = std::string(kind),
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
    std::string_view name,
    Sample default_value,
    bool has_name
)
{
    auto& scope = current();
    if (has_name) {
        scope.input_configs.emplace_back(InputConfig{
            .name = std::string(name),
            .default_value = default_value
        });
    } else {
        scope.input_configs.emplace_back(InputConfig{});
    }
    size_t const placeholder_node = topology.append_placeholder_node(
        std::array<OutputConfig, 1>{ OutputConfig{ .name = has_name ? std::string(name) : std::string{} } },
        {}
    );
    scope.input_placeholder_nodes.push_back(placeholder_node);
    return SamplePortRef(builder, placeholder_node, 0);
}

EventPortRef SubgraphScopeManager::add_scope_event_input(
    GraphBuilder& builder,
    GraphBuilderTopology& topology,
    std::string_view name,
    EventTypeId type,
    bool has_name
)
{
    auto& scope = current();
    if (has_name) {
        scope.event_input_configs.emplace_back(EventInputConfig{ .name = std::string(name), .type = type });
    } else {
        scope.event_input_configs.emplace_back(EventInputConfig{ .type = type });
    }
    size_t const placeholder_node = topology.append_placeholder_node(
        {},
        std::array<EventOutputConfig, 1>{ EventOutputConfig{ .name = has_name ? std::string(name) : std::string{}, .type = type } }
    );
    scope.event_input_placeholder_nodes.push_back(placeholder_node);
    return EventPortRef(builder, placeholder_node, 0);
}

void SubgraphScopeManager::define_sample_outputs(
    std::span<OutputRefConfig const> refs,
    GraphBuilder const& builder,
    GraphBuilderTopology const&,
    GraphBuilderIdentity const& identity
)
{
    auto& scope = current();
    if (scope.outputs_defined) {
        details::error("subgraph outputs(...) was already called on builder " + identity.value);
    }

    scope.output_configs.clear();
    scope.output_sources.clear();
    scope.output_configs.reserve(refs.size());
    scope.output_sources.reserve(refs.size());
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
        scope.output_sources.push_back(PortId{ ref.node_index, ref.output_port });
        scope.output_configs.push_back(config);
    }
    scope.outputs_defined = true;
}

void SubgraphScopeManager::define_event_outputs(
    std::span<EventOutputRefConfig const> refs,
    GraphBuilder const& builder,
    GraphBuilderTopology const& topology,
    GraphBuilderIdentity const& identity,
    std::span<EventInputConfig const> graph_event_inputs
)
{
    auto& scope = current();
    if (scope.event_outputs_defined) {
        details::error("subgraph event_outputs(...) was already called on builder " + identity.value);
    }

    scope.event_output_configs.clear();
    scope.event_output_sources.clear();
    scope.event_output_configs.reserve(refs.size());
    scope.event_output_sources.reserve(refs.size());
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
        auto source_type = (ref.node_index == GRAPH_ID)
            ? graph_event_inputs[ref.output_port].type
            : topology.node(ref.node_index).event_outputs()[ref.output_port].type;
        scope.event_output_sources.push_back(PortId{ ref.node_index, ref.output_port });
        scope.event_output_configs.emplace_back(config);
        scope.event_output_configs.back().type = source_type;
    }
    scope.event_outputs_defined = true;
}

NodeRef SubgraphScopeManager::finalize_scope(GraphBuilder& builder, GraphBuilderTopology& topology, ScopedSubgraph scope)
{
    size_t const placeholder_node_index = topology.node_count();
    std::unordered_map<size_t, size_t> sample_input_index_by_placeholder;
    sample_input_index_by_placeholder.reserve(scope.input_placeholder_nodes.size());
    for (size_t i = 0; i < scope.input_placeholder_nodes.size(); ++i) {
        sample_input_index_by_placeholder.emplace(scope.input_placeholder_nodes[i], i);
    }

    std::unordered_map<size_t, size_t> event_input_index_by_placeholder;
    event_input_index_by_placeholder.reserve(scope.event_input_placeholder_nodes.size());
    for (size_t i = 0; i < scope.event_input_placeholder_nodes.size(); ++i) {
        event_input_index_by_placeholder.emplace(scope.event_input_placeholder_nodes[i], i);
    }

    std::vector<std::vector<PortId>> subgraph_input_targets(scope.input_configs.size());
    std::vector<std::vector<PortId>> subgraph_event_input_targets(scope.event_input_configs.size());

    auto translate_sample_source = [&](PortId source) {
        if (auto const it = sample_input_index_by_placeholder.find(source.node); it != sample_input_index_by_placeholder.end()) {
            return PortId{ placeholder_node_index, it->second };
        }
        return source;
    };

    auto translate_event_source = [&](PortId source) {
        if (auto const it = event_input_index_by_placeholder.find(source.node); it != event_input_index_by_placeholder.end()) {
            return PortId{ placeholder_node_index, it->second };
        }
        return source;
    };

    topology.for_each_sample_edge([&](GraphEdge const& edge) {
        auto const it = sample_input_index_by_placeholder.find(edge.source.node);
        if (it == sample_input_index_by_placeholder.end()) {
            return;
        }
        subgraph_input_targets[it->second].push_back(edge.target);
    });
    topology.erase_sample_edges_matching([&](GraphEdge const& edge) {
        return sample_input_index_by_placeholder.contains(edge.source.node);
    });

    topology.for_each_event_edge([&](GraphEventEdge const& edge) {
        auto const it = event_input_index_by_placeholder.find(edge.source.node);
        if (it == event_input_index_by_placeholder.end()) {
            return;
        }
        subgraph_event_input_targets[it->second].push_back(edge.target);
    });
    topology.erase_event_edges_matching([&](GraphEventEdge const& edge) {
        return event_input_index_by_placeholder.contains(edge.source.node);
    });

    for (auto& source : scope.output_sources) {
        source = translate_sample_source(source);
    }
    for (auto& source : scope.event_output_sources) {
        source = translate_event_source(source);
    }

    size_t const placeholder_node = topology.append_lowered_subgraph_placeholder(
        std::move(scope.kind),
        std::move(scope.input_configs),
        std::move(scope.output_configs),
        std::move(scope.event_input_configs),
        std::move(scope.event_output_configs),
        scope.start_node_index,
        topology.node_count() - scope.start_node_index,
        std::move(subgraph_input_targets),
        std::move(scope.output_sources),
        std::move(subgraph_event_input_targets),
        std::move(scope.event_output_sources)
    );

    return NodeRef(builder, placeholder_node);
}

size_t SubgraphScopeManager::current_start_node_index() const
{
    return _stack.back().start_node_index;
}
}
