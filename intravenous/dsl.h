#pragma once
#include "basic_nodes/routing.h"
#include "basic_nodes/arithmetic.h"
#include "graph_build.h"
#include "graph_node.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <functional>
#include <memory>
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
    class GraphBuilder;

    struct SignalRef {
        GraphBuilder* graph_builder{};
        size_t node_index{};
        size_t output_port{};

        SignalRef() = default;
        explicit SignalRef(GraphBuilder& graph_builder_, size_t node_index, size_t output_port);
        operator PortId() const { return { node_index, output_port }; }

        SignalRef detach(size_t loop_block_size = 1) const;

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

        SignalRef detach(size_t loop_block_size = 1) const;

        std::string to_string() const;
    };

    using DetachedSignalInfo = DetachedInfo;

    class GraphBuilder {
        friend class NodeRef;
        friend struct SignalRef;

        struct BuilderIdentity {
            std::string value;

            BuilderIdentity() = default;
            explicit BuilderIdentity(std::string value_) :
                value(value_)
            {}

            std::string child_id(size_t index) const
            {
                std::string nested_path = value;
                if (!nested_path.empty()) {
                    nested_path += ".";
                }
                nested_path += std::to_string(index);
                return nested_path;
            }

        };

        BuilderIdentity _builder_id;
        size_t _next_detach_id = 0;

        std::vector<BuilderNode> _nodes;
        std::unordered_set<GraphEdge> _edges;
        std::unordered_set<PortId> _placed_input_ports;

        std::vector<InputConfig> _public_inputs;
        std::unordered_map<std::string, size_t> _input_name_to_index;

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
            return GraphBuilder(BuilderIdentity(_builder_id.child_id(_nodes.size())));
        }

        std::string node_id(size_t index) const
        {
            IV_ASSERT(index < _nodes.size(), "node index out of bounds");
            return _builder_id.child_id(index);
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
            if (_input_name_to_index.contains(std::string(name))) {
                details::error("input '" + std::string(name) + "' already exists");
            }

            _public_inputs.emplace_back(InputConfig { .name = std::string(name), .default_value = default_value });
            _input_name_to_index.emplace(std::string(name), _public_inputs.size() - 1);
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
                    return TypeErasedNode(DetachWriterNode{ DetachArrayId(node_value.id.id + detach_id_offset) });
                } else if constexpr (std::same_as<StoredNode, DetachReaderNode>) {
                    return TypeErasedNode(DetachReaderNode{ DetachArrayId(node_value.id.id + detach_id_offset) });
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

        NodeRef node(GraphBuilder const& child)
        {
            std::string expected_builder_id = _builder_id.child_id(_nodes.size());
            GraphBuilder child_copy = child;
            child_copy._builder_id = BuilderIdentity(expected_builder_id);

            if (!child_copy._outputs_defined) {
                details::error(
                    "builder " + child_copy._builder_id.value + ": g.outputs(...) must be called before insertion"
                );
            }

            size_t const child_detach_offset = _next_detach_id;
            _next_detach_id += child_copy._next_detach_id;

            _nodes.emplace_back(BuilderNode{
                .input_configs = child_copy._public_inputs,
                .output_configs = child_copy._public_outputs,
                .materialize = [child = std::make_shared<GraphBuilder>(std::move(child_copy)), child_detach_offset](size_t parent_detach_offset) {
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
            std::unordered_set<std::string> output_names;

            size_t output_port = 0;
            for (auto const& ref : refs) {
                if (ref.name.empty()) {
                    details::error(
                        "builder " + _builder_id.value + ": named outputs must not use empty names"
                    );
                }

                if (output_names.contains(std::string(ref.name))) {
                    details::error(
                        "builder " + _builder_id.value + ": duplicate output name '" + std::string(ref.name) + "'"
                    );
                }
                output_names.insert(std::string(ref.name));

                auto const signal = lift_to_signal(ref);

                _edges.emplace(GraphEdge{
                    signal,
                    PortId{ GRAPH_ID, output_port++ },
                    });
                _public_outputs.emplace_back(OutputConfig{ .name = std::string(ref.name) });
            }

            _outputs_defined = true;
        }

        Graph build(size_t detach_id_offset = 0) const
        {
            if (!_outputs_defined) {
                details::error("builder " + _builder_id.value + ": g.outputs(...) must be called before build()");
            }

            details::PreparedGraph g{
                .nodes = {},
                .node_ids = {},
                .edges = _edges,
                .detached_info_by_source = _detached_info_by_source,
                .detached_reader_outputs = _detached_reader_outputs,
            };
            g.nodes.reserve(_nodes.size());
            g.node_ids.reserve(_nodes.size());
            for (auto const& node : _nodes) {
                g.nodes.push_back(node.materialize(detach_id_offset));
                g.node_ids.push_back(node_id(g.node_ids.size()));
            }

            details::expand_hyperedge_ports(g, _builder_id.value);
            details::stub_dangling_ports(g, _public_inputs.size(), _builder_id.value);
            details::validate_graph(g, _public_inputs.size(), _public_outputs.size());
            details::validate_detached_edges(g, _builder_id.value);
            details::sort_nodes_or_error(g, _builder_id.value);
            details::validate_graph(g, _public_inputs.size(), _public_outputs.size());

            auto detached = [&] {
                std::vector<DetachedInfo> detached_info;
                detached_info.reserve(g.detached_info_by_source.size());
                for (auto const& [_, info] : g.detached_info_by_source) {
                    detached_info.push_back(info);
                }
                return detached_info;
            }();
            auto execution_plan = details::build_execution_plan(g.nodes, g.edges, detached);

            return Graph(details::build_graph_artifact(
                _builder_id.value,
                std::move(g.nodes),
                std::move(g.node_ids),
                std::move(g.edges),
                std::move(detached),
                std::move(execution_plan),
                _public_inputs,
                _public_outputs
            ));
        }

    private:
        static std::string allocate_root_builder_id()
        {
            static size_t next_root_builder_id = 0;
            return std::to_string(next_root_builder_id++);
        }

        SignalRef detach_signal(SignalRef signal, size_t loop_block_size)
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
                if (it->second.loop_block_size != loop_block_size) {
                    details::error(
                        "builder " + _builder_id.value + ": detach loop block size conflict on " + signal.to_string()
                    );
                }
                PortId const reader = it->second.reader_output;
                return SignalRef(*this, reader.node, reader.port);
            }

            try {
                validate_block_size(loop_block_size, "detach loop block size must be a power of 2");
            } catch (std::logic_error const& e) {
                details::error("builder " + _builder_id.value + ": " + e.what());
            }

            size_t const detach_id = _next_detach_id++;

            auto writer = node<DetachWriterNode>(detach_id, loop_block_size);
            size_t const writer_node = _nodes.size() - 1;
            writer(signal);

            auto reader = node<DetachReaderNode>(detach_id, loop_block_size);
            SignalRef detached = reader;

            _detached_info_by_source.emplace(source, DetachedSignalInfo{
                .detach_id = detach_id,
                .original_source = source,
                .writer_node = writer_node,
                .reader_output = detached,
                .loop_block_size = loop_block_size,
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
        requires std::is_arithmetic_v<std::remove_cvref_t<T>> || std::is_same_v<std::remove_cvref_t<T>, Sample>
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
        std::floating_point<std::remove_cvref_t<T>> ||
        std::is_same_v<std::remove_cvref_t<T>, Sample>;

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

    inline SignalRef SignalRef::detach(size_t loop_block_size) const
    {
        if (!graph_builder) {
            details::error("attempted to detach an empty signal");
        }
        return graph_builder->detach_signal(*this, loop_block_size);
    }

    inline std::string SignalRef::to_string() const
    {
        if (!graph_builder) {
            return "empty signal";
        }
        if (node_index == GRAPH_ID) {
            return "graph input " + std::to_string(output_port) + " in builder " + graph_builder->_builder_id.value;
        }
        return "signal at address " + graph_builder->node_id(node_index) + ":" + std::to_string(output_port);
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

    inline SignalRef NodeRef::detach(size_t loop_block_size) const
    {
        return static_cast<SignalRef>(*this).detach(loop_block_size);
    }

    inline std::string NodeRef::to_string() const
    {
        if (!_graph_builder) {
            return "empty node";
        }
        return "node at address " + _graph_builder->node_id(_index);
    }
}
