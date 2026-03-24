#pragma once
#include "basic_nodes/routing.h"
#include "basic_nodes/arithmetic.h"
#include "graph_node.h"
#include "node.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace iv {
    namespace details {
        [[noreturn]] inline void error(std::string msg)
        {
            throw std::logic_error(msg);
        }
    }

    class GraphBuilder;

    struct SignalRef {
        GraphBuilder* graph_builder{};
        size_t node_index{};
        size_t output_port{};

        SignalRef() = default;
        explicit SignalRef(GraphBuilder& graph_builder_, size_t node_index, size_t output_port);
        operator PortId() const { return { node_index, output_port }; }

        SignalRef detach() const;

        std::string to_string() const;
    };

    struct NamedRef {
        std::string_view name;
        std::variant<SignalRef, Sample> value;

        NamedRef(std::string_view name, SignalRef signal): name(name), value(signal) {}
        NamedRef(std::string_view name, Sample sample): name(name), value(sample) {}
    };

    struct BuilderNode {
        std::vector<InputConfig> input_configs;
        std::vector<OutputConfig> output_configs;
        std::function<TypeErasedNode(size_t)> materialize;

        std::vector<InputConfig> const& inputs() const
        {
            return input_configs;
        }

        std::vector<OutputConfig> const& outputs() const
        {
            return output_configs;
        }
    };

    class NodeRef {
        GraphBuilder* _graph_builder{};
        size_t _index{};

    public:
        NodeRef() = default;
        explicit NodeRef(GraphBuilder& graph_builder, size_t index);

        BuilderNode const& node() const;

        SignalRef operator[](size_t output_index) const;
        SignalRef operator[](std::string_view output_name) const;
        operator SignalRef() const;

        template<class... Refs>
        NodeRef operator()(Refs&&... refs) const;
        NodeRef operator()(std::initializer_list<NamedRef> refs) const;

        SignalRef detach() const;

        std::string to_string() const;
    };

    struct DetachedSignalInfo {
        size_t detach_id;
        PortId original_source;
        PortId reader_output;
    };

    class GraphBuilder {
        friend class NodeRef;
        friend struct SignalRef;

        struct BuilderIdentity {
            std::string value;
            size_t next_child_id = 0;

            BuilderIdentity() = default;
            explicit BuilderIdentity(std::string value_) :
                value(value_)
            {}

            std::string debug_node_id(size_t index) const
            {
                std::string nested_path = value;
                if (!nested_path.empty()) {
                    nested_path += ".";
                }
                nested_path += std::to_string(index);
                return nested_path;
            }

            std::string allocate_child_path()
            {
                std::string nested_path = value;
                if (!nested_path.empty()) {
                    nested_path += ".";
                }
                nested_path += std::to_string(next_child_id++);
                return nested_path;
            }
        };

        struct PreparedGraph {
            Graph::Nodes nodes;
            Graph::Edges edges;
            std::unordered_map<PortId, DetachedSignalInfo> detached_info_by_source;
            std::unordered_set<PortId> detached_reader_outputs;
        };

        BuilderIdentity _builder_id;
        size_t _next_detach_id = 0;

        std::vector<BuilderNode> _nodes;
        Graph::Edges _edges;
        std::unordered_set<PortId> _placed_input_ports;

        std::vector<InputConfig> _public_inputs;
        std::unordered_map<std::string_view, size_t> _input_name_to_index;

        std::vector<OutputConfig> _public_outputs;
        bool _outputs_defined{ false };

        std::unordered_map<PortId, DetachedSignalInfo> _detached_info_by_source;
        std::unordered_set<PortId> _detached_reader_outputs;

        explicit GraphBuilder(BuilderIdentity builder_id) :
            _builder_id(std::move(builder_id))
        {
        }

    public:
        GraphBuilder() :
            _builder_id(allocate_root_builder_id())
        {
        }

        GraphBuilder derive_nested_builder()
        {
            return GraphBuilder(
                BuilderIdentity(_builder_id.allocate_child_path())
            );
        }

        std::string debug_node_id(size_t index) const
        {
            assert(index < _nodes.size() && "node index out of bounds");
            return _builder_id.debug_node_id(index);
        }

        SignalRef input()
        {
            _public_inputs.emplace_back();
            return SignalRef(*this, GRAPH_ID, _public_inputs.size() - 1);
        }

        SignalRef input(std::string_view name, Sample default_value = 0.0)
        {
            if (name.empty()) {
                details::error("input name cannot be empty");
            }
            if (_input_name_to_index.contains(name)) {
                details::error("input '" + std::string(name) + "' already exists");
            }

            _public_inputs.emplace_back(InputConfig { .name = name, .default_value = default_value });
            _input_name_to_index.emplace(name, _public_inputs.size() - 1);
            return SignalRef(*this, GRAPH_ID, _public_inputs.size() - 1);
        }

        template<class Node, class... Args>
        NodeRef node(Args&&... args)
        {
            using StoredNode = std::remove_cvref_t<Node>;
            StoredNode node_value(std::forward<Args>(args)...);
            auto inputs = get_inputs(node_value);
            auto outputs = get_outputs(node_value);

            auto materialize = [node_value = std::move(node_value)]([[maybe_unused]] size_t detach_id_offset) {
                if constexpr (std::same_as<StoredNode, DetachWriterNode>) {
                    return TypeErasedNode(DetachWriterNode{ BufferId(node_value.id.id + detach_id_offset) });
                } else if constexpr (std::same_as<StoredNode, DetachReaderNode>) {
                    return TypeErasedNode(DetachReaderNode{ BufferId(node_value.id.id + detach_id_offset) });
                } else {
                    return TypeErasedNode(node_value);
                }
            };

            _nodes.emplace_back(BuilderNode{
                .input_configs = std::vector<InputConfig>(std::begin(inputs), std::end(inputs)),
                .output_configs = std::vector<OutputConfig>(std::begin(outputs), std::end(outputs)),
                .materialize = std::move(materialize),
            });
            return NodeRef(*this, _nodes.size() - 1);
        }

        NodeRef node(GraphBuilder child)
        {
            if (!child._outputs_defined) {
                details::error(
                    "builder " + child._builder_id.value + ": g.outputs(...) must be called before insertion"
                );
            }

            size_t const child_detach_offset = _next_detach_id;
            _next_detach_id += child._next_detach_id;

            _nodes.emplace_back(BuilderNode{
                .input_configs = child._public_inputs,
                .output_configs = child._public_outputs,
                .materialize = [child = std::make_shared<GraphBuilder>(std::move(child)), child_detach_offset](size_t parent_detach_offset) {
                    return TypeErasedNode(child->build(parent_detach_offset + child_detach_offset));
                },
            });
            return NodeRef(*this, _nodes.size() - 1);
        }

        template<class Fn>
        NodeRef subgraph(Fn&& fn)
        {
            GraphBuilder g = derive_nested_builder();
            std::forward<Fn>(fn)(g);

            if (!g._outputs_defined) {
                details::error(
                    "builder " + g._builder_id.value + ": g.outputs(...) must be called before returning from subgraph()"
                );
            }

            return node(std::move(g));
        }

        template<class... Refs>
        void outputs(Refs&&... refs)
        {
            if (_outputs_defined) {
                details::error("outputs(...) was already called on builder " + _builder_id.value);
            }

            std::array<SignalRef, sizeof...(Refs)> refs_array{
                lift_to_signal(std::forward<Refs>(refs))...
            };
            _public_outputs.clear();
            _public_outputs.reserve(refs_array.size());

            for (size_t i = 0; i < refs_array.size(); ++i) {
                auto const& ref = refs_array[i];
                if (ref.graph_builder != this) {
                    details::error(
                        "builder " + _builder_id.value + ": outputs(...): "
                        "SignalRef at index " + std::to_string(i) + " "
                        "belongs to builder " + ref.graph_builder->_builder_id.value
                    );
                }

                _edges.emplace(GraphEdge{
                    PortId{ ref.node_index, ref.output_port },
                    PortId{ GRAPH_ID, i },
                });
                _public_outputs.emplace_back();
            }

            _outputs_defined = true;
        }

        void outputs(std::initializer_list<NamedRef> refs)
        {
            if (_outputs_defined) {
                details::error("outputs(...) was already called on builder " + _builder_id.value);
            }

            _public_outputs.reserve(refs.size());
            std::unordered_set<std::string_view> output_names;

            size_t output_port = 0;
            for (auto const& ref : refs) {
                if (ref.name.empty()) {
                    details::error(
                        "builder " + _builder_id.value + ": named outputs must not use empty names"
                    );
                }

                if (output_names.contains(ref.name)) {
                    details::error(
                        "builder " + _builder_id.value + ": duplicate output name '" + std::string(ref.name) + "'"
                    );
                }
                output_names.insert(ref.name);

                auto const signal = lift_to_signal(ref);

                _edges.emplace(GraphEdge{
                    signal,
                    PortId{ GRAPH_ID, output_port++ },
                    });
                _public_outputs.emplace_back(OutputConfig{ .name = ref.name });
            }

            _outputs_defined = true;
        }

        Graph build(size_t detach_id_offset = 0) const
        {
            if (!_outputs_defined) {
                details::error("builder " + _builder_id.value + ": g.outputs(...) must be called before build()");
            }

            PreparedGraph g{
                .nodes = {},
                .edges = _edges,
                .detached_info_by_source = _detached_info_by_source,
                .detached_reader_outputs = _detached_reader_outputs,
            };
            g.nodes.reserve(_nodes.size());
            for (auto const& node : _nodes) {
                g.nodes.push_back(node.materialize(detach_id_offset));
            }

            expand_hyperedge_ports(g);
            stub_dangling_ports(g);
            validate_graph(g);
            validate_detached_edges(g);
            sort_nodes_or_error(g);
            validate_graph(g);

            return Graph(
                std::move(g.nodes),
                std::move(g.edges),
                _public_inputs,
                _public_outputs
            );
        }

    private:
        static std::string allocate_root_builder_id()
        {
            static size_t next_root_builder_id = 0;
            return std::to_string(next_root_builder_id++);
        }

        SignalRef detach_signal(SignalRef signal)
        {
            if (!signal.graph_builder) {
                details::error(
                    "builder " + _builder_id.value + ": cannot detach an empty signal"
                );
            }

            if (signal.graph_builder != this) {
                details::error(
                    "builder " + _builder_id.value + ": cannot detach " + signal.to_string() +
                    " because it belongs to another builder"
                );
            }

            PortId const source = signal;

            // Already detached: return unchanged.
            if (_detached_reader_outputs.contains(source)) {
                return signal;
            }

            // Reuse existing detached bridge for the same source.
            if (auto it = _detached_info_by_source.find(source); it != _detached_info_by_source.end()) {
                PortId const reader = it->second.reader_output;
                return SignalRef(*this, reader.node, reader.port);
            }

            size_t const detach_id = _next_detach_id++;

            auto writer = node<DetachWriterNode>(detach_id);
            writer(signal);

            auto reader = node<DetachReaderNode>(detach_id);
            SignalRef detached = reader;

            _detached_info_by_source.emplace(source, DetachedSignalInfo{
                .detach_id = detach_id,
                .original_source = source,
                .reader_output = detached,
            });
            _detached_reader_outputs.insert(detached);

            return detached;
        }

        SignalRef lift_to_signal(SignalRef s)
        {
            if (s.graph_builder != this) {
                details::error(
                    "builder " + _builder_id.value + ": signal " + s.to_string() + " belongs to another builder"
                );
            }
            return s;
        }

        template<class T>
        requires std::is_arithmetic_v<std::remove_cvref_t<T>>
        SignalRef lift_to_signal(T value)
        {
            return node<Constant>(static_cast<Sample>(value));
        }

        SignalRef lift_to_signal(NamedRef const& ref)
        {
            return std::visit([&](auto const& v) -> SignalRef {
                return lift_to_signal(v);
            }, ref.value);
        }

        static auto make_source_target_edge_maps(PreparedGraph const& g)
        {
            std::unordered_map<PortId, PortId> source_of;
            std::unordered_map<PortId, PortId> target_of;

            for (GraphEdge const& edge : g.edges)
            {
                source_of[edge.target] = edge.source;
                target_of[edge.source] = edge.target;
            }

            return std::make_tuple(std::move(source_of), std::move(target_of));
        }

        void expand_hyperedge_ports(PreparedGraph& g) const
        {
            std::unordered_map<PortId, std::vector<GraphEdge>> reverse_edges_map;
            for (GraphEdge const& edge : g.edges)
            {
                reverse_edges_map[edge.target].push_back(edge);
            }

            size_t nodes_size = g.nodes.size();
            for (size_t node = 0; node < nodes_size; ++node)
            {
                size_t const num_inputs = get_num_inputs(g.nodes[node]);
                for (size_t in_port = 0; in_port < num_inputs; ++in_port)
                {
                    auto it = reverse_edges_map.find({ node, in_port });
                    if (it == reverse_edges_map.end()) continue;

                    auto const& edges_to_expand = it->second;
                    size_t const port_arity = edges_to_expand.size();
                    if (port_arity <= 1) continue;

                    g.nodes.emplace_back(Sum(port_arity));
                    size_t const sum_node = g.nodes.size() - 1;

                    for (size_t out_port = 0; out_port < edges_to_expand.size(); ++out_port)
                    {
                        GraphEdge const& to_rewire = edges_to_expand[out_port];
                        g.edges.erase(to_rewire);
                        g.edges.insert(GraphEdge{ to_rewire.source, { sum_node, out_port } });
                    }
                    g.edges.insert(GraphEdge{ { sum_node, 0 }, { node, in_port } });
                }
            }

            std::unordered_map<PortId, std::vector<GraphEdge>> edges_map;
            for (GraphEdge const& edge : g.edges)
            {
                edges_map[edge.source].push_back(edge);
            }

            nodes_size = g.nodes.size();
            for (size_t node = 0; node < nodes_size; ++node)
            {
                size_t const num_outputs = get_num_outputs(g.nodes[node]);
                for (size_t out_port = 0; out_port < num_outputs; ++out_port)
                {
                    auto it = edges_map.find({ node, out_port });
                    if (it == edges_map.end()) continue;

                    auto const& edges_to_expand = it->second;
                    size_t const port_arity = edges_to_expand.size();
                    if (port_arity <= 1) continue;

                    g.nodes.emplace_back(Broadcast(port_arity));
                    size_t const broadcast_node = g.nodes.size() - 1;

                    for (size_t in_port = 0; in_port < edges_to_expand.size(); ++in_port)
                    {
                        GraphEdge const& to_rewire = edges_to_expand[in_port];
                        g.edges.erase(to_rewire);
                        g.edges.insert(GraphEdge{ { broadcast_node, in_port }, to_rewire.target });
                    }
                    g.edges.insert(GraphEdge{ { node, out_port }, { broadcast_node, 0 } });
                }
            }
        }

        void stub_dangling_ports(PreparedGraph& g) const
        {
            auto [source_of, target_of] = make_source_target_edge_maps(g);

            size_t const num_nodes = g.nodes.size();
            for (size_t node_id = 0; node_id < num_nodes + 1; ++node_id)
            {
                size_t const node = (node_id == num_nodes) ? GRAPH_ID : node_id;
                size_t const num_outputs = (node == GRAPH_ID) ? _public_inputs.size() : get_num_outputs(g.nodes[node]);

                for (size_t output_port = 0; output_port < num_outputs; ++output_port)
                {
                    PortId const this_port{ node, output_port };
                    if (auto it = target_of.find(this_port); it == target_of.end())
                    {
                        g.nodes.emplace_back(DummySink());
                        size_t const new_node = g.nodes.size() - 1;
                        g.edges.insert(GraphEdge{ this_port, { new_node, 0 } });
                    }
                }
            }
        }

        void validate_graph(PreparedGraph const& g) const
        {
            for (auto const& edge : g.edges)
            {
                size_t source_num_outputs = (edge.source.node == GRAPH_ID)
                    ? _public_inputs.size()
                    : get_num_outputs(g.nodes[edge.source.node]);

                size_t target_num_inputs = (edge.target.node == GRAPH_ID)
                    ? _public_outputs.size()
                    : get_num_inputs(g.nodes[edge.target.node]);

                if (edge.source.port >= source_num_outputs) {
                    details::error("bad connection: source output port out of range");
                }
                if (edge.target.port >= target_num_inputs) {
                    details::error("bad connection: target input port out of range");
                }
            }
        }

        static auto make_node_adjacency(PreparedGraph const& g)
        {
            size_t const num_nodes = g.nodes.size();
            std::vector<std::unordered_set<size_t>> outgoing(num_nodes);
            std::vector<size_t> indegree(num_nodes, 0);

            for (GraphEdge const& edge : g.edges)
            {
                if (edge.source.node == GRAPH_ID) continue;
                if (edge.target.node == GRAPH_ID) continue;

                size_t const u = edge.source.node;
                size_t const v = edge.target.node;

                if (outgoing[u].insert(v).second) {
                    ++indegree[v];
                }
            }

            return std::make_pair(std::move(outgoing), std::move(indegree));
        }

        static void apply_node_permutation(PreparedGraph& g, std::vector<size_t> const& sorted)
        {
            size_t const num_nodes = g.nodes.size();

            Graph::Nodes sorted_nodes;
            sorted_nodes.reserve(num_nodes);
            for (size_t old_i = 0; old_i < num_nodes; ++old_i)
            {
                sorted_nodes.push_back(std::move(g.nodes[sorted[old_i]]));
            }
            g.nodes.swap(sorted_nodes);

            std::vector<size_t> reverse_sorted(num_nodes);
            for (size_t new_i = 0; new_i < num_nodes; ++new_i) {
                reverse_sorted[sorted[new_i]] = new_i;
            }

            Graph::Edges sorted_edges;
            sorted_edges.reserve(g.edges.size());
            for (GraphEdge edge : g.edges)
            {
                if (edge.source.node != GRAPH_ID)
                    edge.source.node = reverse_sorted[edge.source.node];
                if (edge.target.node != GRAPH_ID)
                    edge.target.node = reverse_sorted[edge.target.node];
                sorted_edges.insert(edge);
            }
            g.edges.swap(sorted_edges);

            for (auto& [_, info] : g.detached_info_by_source)
            {
                if (info.original_source.node != GRAPH_ID) {
                    info.original_source.node = reverse_sorted[info.original_source.node];
                }
                if (info.reader_output.node != GRAPH_ID) {
                    info.reader_output.node = reverse_sorted[info.reader_output.node];
                }
            }
        }

        void sort_nodes_or_error(PreparedGraph& g) const
        {
            auto [outgoing, indegree] = make_node_adjacency(g);

            std::deque<size_t> ready;
            for (size_t node = 0; node < indegree.size(); ++node) {
                if (indegree[node] == 0) {
                    ready.push_back(node);
                }
            }

            std::vector<size_t> sorted;
            sorted.reserve(g.nodes.size());

            while (!ready.empty())
            {
                size_t const node = ready.front();
                ready.pop_front();
                sorted.push_back(node);

                for (size_t target : outgoing[node])
                {
                    if (--indegree[target] == 0) {
                        ready.push_back(target);
                    }
                }
            }

            if (sorted.size() != g.nodes.size()) {
                details::error(
                    "builder " + _builder_id.value + ": graph contains a cycle; use detach() to break feedback explicitly"
                );
            }

            apply_node_permutation(g, sorted);
        }

        static bool has_path(
            std::vector<std::unordered_set<size_t>> const& outgoing,
            size_t start,
            size_t goal)
        {
            if (start == goal) {
                return true;
            }

            std::vector<bool> seen(outgoing.size(), false);
            std::deque<size_t> q;
            q.push_back(start);
            seen[start] = true;

            while (!q.empty())
            {
                size_t u = q.front();
                q.pop_front();

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

        void validate_detached_edges(PreparedGraph const& g) const
        {
            size_t const num_nodes = g.nodes.size();

            std::vector<std::unordered_set<size_t>> explicit_outgoing(num_nodes);
            std::unordered_map<PortId, std::vector<PortId>> consumers_of_output;

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

                auto it = consumers_of_output.find(info.reader_output);
                if (it == consumers_of_output.end()) {
                    continue;
                }

                for (PortId target_port : it->second)
                {
                    if (target_port.node == GRAPH_ID) {
                        continue;
                    }

                    size_t const u = info.original_source.node;
                    size_t const v = target_port.node;

                    auto augmented = explicit_outgoing;
                    if (!has_path(augmented, v, u)) {
                        details::error(
                            "builder " + _builder_id.value + ": detach() on " +
                            "signal " + std::to_string(info.original_source.node) + ":" + std::to_string(info.original_source.port) +
                            " breaks an acyclic dependency"
                        );
                    }
                }
            }
        }
    };

    inline SignalRef lift(GraphBuilder& g, Sample value)
    {
        return g.node<Constant>(value);
    }

    inline SignalRef lift(SignalRef s)
    {
        if (!s.graph_builder) {
            details::error("cannot lift an empty signal");
        }
        return s;
    }

    template<class T>
    concept SignalLike = std::convertible_to<std::remove_cvref_t<T>, SignalRef>;

    template<class T>
    concept ScalarLike =
        std::integral<std::remove_cvref_t<T>> ||
        std::floating_point<std::remove_cvref_t<T>>;

    template<class T>
    concept Liftable = SignalLike<T> || ScalarLike<T>;

    template<class T>
    SignalRef lift_operand(GraphBuilder& g, T&& x)
    {
        if constexpr (SignalLike<T>) {
            SignalRef s = std::forward<T>(x);
            if (s.graph_builder != &g) {
                details::error("operand belongs to a different builder");
            }
            return s;
        }
        else {
            return g.node<Constant>(static_cast<Sample>(x))();
        }
    }

    template<class Node, class L, class R>
    requires (Liftable<L> && Liftable<R>)
    SignalRef make_binary_op(L&& lhs, R&& rhs, std::string_view op_name)
    {
        GraphBuilder* g = nullptr;

        if constexpr (SignalLike<L>) {
            SignalRef s = std::forward<L>(lhs);
            g = s.graph_builder;
        }

        if constexpr (SignalLike<R>) {
            SignalRef s = std::forward<R>(rhs);
            if (!g) {
                g = s.graph_builder;
            }
            else if (s.graph_builder != g) {
                details::error(std::string(op_name) + ": operands belong to different builders");
            }
        }

        if (!g) {
            details::error(std::string(op_name) + ": at least one operand must be a signal");
        }

        SignalRef lhs_signal = lift_operand(*g, std::forward<L>(lhs));
        SignalRef rhs_signal = lift_operand(*g, std::forward<R>(rhs));

        return g->node<Node>()(lhs_signal, rhs_signal);
    }

    template<class L, class R>
    requires (Liftable<L>&& Liftable<R>)
    SignalRef operator+(L&& lhs, R&& rhs)
    {
        return make_binary_op<Sum>(
            std::forward<L>(lhs),
            std::forward<R>(rhs),
            "operator+"
        );
    }

    template<class L, class R>
    requires (Liftable<L>&& Liftable<R>)
    SignalRef operator-(L&& lhs, R&& rhs)
    {
        return make_binary_op<Subtract>(
            std::forward<L>(lhs),
            std::forward<R>(rhs),
            "operator-"
        );
    }

    template<class L, class R>
    requires (Liftable<L>&& Liftable<R>)
    SignalRef operator*(L&& lhs, R&& rhs)
    {
        return make_binary_op<Product>(
            std::forward<L>(lhs),
            std::forward<R>(rhs),
            "operator*"
        );
    }

    template<class L, class R>
    requires (Liftable<L>&& Liftable<R>)
    SignalRef operator/(L&& lhs, R&& rhs)
    {
        return make_binary_op<Quotient>(
            std::forward<L>(lhs),
            std::forward<R>(rhs),
            "operator/"
        );
    }

    inline SignalRef::SignalRef(GraphBuilder& graph_builder_, size_t node_index, size_t output_port) :
        graph_builder(&graph_builder_),
        node_index(node_index),
        output_port(output_port)
    {
        if (node_index == GRAPH_ID) {
            if (output_port >= graph_builder->_public_inputs.size()) {
                details::error(
                    "graph input port " + std::to_string(output_port) + " "
                    "is out of bounds in builder " + graph_builder->_builder_id.value + ", "
                    "public_inputs.size() = " + std::to_string(graph_builder->_public_inputs.size())
                );
            }
            return;
        }

        if (node_index >= graph_builder->_nodes.size()) {
            details::error(
                "node at index " + std::to_string(node_index) + " "
                "is out of bounds in builder " + graph_builder->_builder_id.value + ", "
                "nodes.size() = " + std::to_string(graph_builder->_nodes.size())
            );
        }

        auto& node = graph_builder->_nodes[node_index];
        size_t num_outputs = get_num_outputs(node);
        if (output_port >= num_outputs) {
            details::error(
                "output port " + std::to_string(output_port) + " of "
                "node at index " + std::to_string(node_index) + " in "
                "builder " + graph_builder->_builder_id.value + " "
                "is out of bounds, get_num_outputs(node) = " + std::to_string(num_outputs)
            );
        }
    }

    inline SignalRef SignalRef::detach() const
    {
        if (!graph_builder) {
            details::error("attempted to detach an empty signal");
        }
        return graph_builder->detach_signal(*this);
    }

    inline std::string SignalRef::to_string() const
    {
        if (!graph_builder) {
            return "empty signal";
        }
        if (node_index == GRAPH_ID) {
            return "graph input " + std::to_string(output_port) + " in builder " + graph_builder->_builder_id.value;
        }
        return "signal at address " + graph_builder->debug_node_id(node_index) + ":" + std::to_string(output_port);
    }

    inline NodeRef::NodeRef(GraphBuilder& graph_builder, size_t index) :
        _graph_builder(&graph_builder),
        _index(index)
    {}

    inline BuilderNode const& NodeRef::node() const
    {
        if (!_graph_builder) {
            details::error("attempted to use a null NodeRef");
        }
        return _graph_builder->_nodes[_index];
    }

    inline SignalRef NodeRef::operator[](size_t output_index) const
    {
        if (!_graph_builder) {
            details::error("attempted to use a null NodeRef");
        }
        return SignalRef(*_graph_builder, _index, output_index);
    }

    inline SignalRef NodeRef::operator[](std::string_view output_name) const
    {
        if (!_graph_builder) {
            details::error("attempted to use a null NodeRef");
        }
        auto outputs = get_outputs(node());
        for (size_t output_port = 0; output_port < outputs.size(); ++output_port) {
            if (outputs[output_port].name == output_name) {
                return SignalRef(*_graph_builder, _index, output_port);
            }
        }

        details::error(
            "an output port named '" + std::string(output_name) + "' "
            "does not exist on " + to_string()
        );
    }

    inline NodeRef::operator SignalRef() const
    {
        if (!_graph_builder) {
            details::error("attempted to use a null NodeRef");
        }
        if (get_num_outputs(node()) != 1) {
            details::error(
                to_string() + " "
                "does not have exactly 1 output port: it cannot be implicitly converted to SignalRef"
            );
        }
        return SignalRef(*_graph_builder, _index, 0);
    }

    template<class... Refs>
    NodeRef NodeRef::operator()(Refs&&... refs) const
    {
        if (!_graph_builder) {
            details::error("attempted to use a null NodeRef");
        }
        std::array<SignalRef, sizeof...(Refs)> refs_array{
            _graph_builder->lift_to_signal(std::forward<Refs>(refs))...
        };

        auto const inputs = get_inputs(node());
        if (sizeof...(Refs) > inputs.size()) {
            details::error(
                to_string() + " "
                "has at most " + std::to_string(inputs.size()) + " inputs, "
                "got " + std::to_string(sizeof...(Refs))
            );
        }

        for (size_t input_port = 0; input_port < refs_array.size(); ++input_port) {
            auto const& ref = refs_array[input_port];

            if (ref.graph_builder != _graph_builder) {
                details::error(
                    ref.to_string() + " "
                    "does not belong to the same builder as " + to_string()
                );
            }

            PortId const input_port_id = {_index, input_port};
            if (_graph_builder->_placed_input_ports.contains(input_port_id)) {
                details::error(
                    "input port " + std::string(inputs[input_port].name) + " of " + to_string() + " "
                    "was already placed in the graph"
                );
            }

            _graph_builder->_placed_input_ports.insert(input_port_id);
            _graph_builder->_edges.emplace(GraphEdge{
                ref,
                PortId{ _index, input_port },
            });
        }

        return *this;
    }

    inline NodeRef NodeRef::operator()(std::initializer_list<NamedRef> refs) const
    {
        if (!_graph_builder) {
            details::error("attempted to use a null NodeRef");
        }

        auto inputs = get_inputs(node());

        for (auto const& ref : refs) {
            if (ref.name.empty()) {
                details::error(
                    "named inputs must not use empty names on " + to_string()
                );
            }

            auto const signal = _graph_builder->lift_to_signal(ref);

            if (signal.graph_builder != _graph_builder) {
                details::error(
                    signal.to_string() + " "
                    "does not belong to the same builder as " + to_string()
                );
            }

            bool placed = false;
            for (size_t input_port = 0; input_port < inputs.size(); ++input_port) {
                if (ref.name != inputs[input_port].name) continue;

                PortId const input_port_id = {_index, input_port};
                if (_graph_builder->_placed_input_ports.contains(input_port_id)) {
                    details::error(
                        "input port " + std::string(inputs[input_port].name) + " of " + to_string() + " "
                        "was already placed in the graph"
                    );
                }

                _graph_builder->_placed_input_ports.insert(input_port_id);
                _graph_builder->_edges.emplace(GraphEdge{
                    signal,
                    PortId { _index, input_port },
                });

                placed = true;
                break;
            }

            if (!placed) {
                details::error(
                    "an input port named '" + std::string(ref.name) + "' "
                    "does not exist on " + to_string()
                );
            }
        }

        return *this;
    }

    inline SignalRef NodeRef::detach() const
    {
        return static_cast<SignalRef>(*this).detach();
    }

    inline std::string NodeRef::to_string() const
    {
        if (!_graph_builder) {
            return "empty node";
        }
        return "node at address " + _graph_builder->debug_node_id(_index);
    }
}
