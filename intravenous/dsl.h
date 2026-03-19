#pragma once

#include "graph_node.h"
#include "basic_nodes.h"
#include "node.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
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

        std::string to_string() const;
    };

    struct NamedRef {
        std::string_view name;
        SignalRef signal;
    };

    class NodeRef {
        GraphBuilder* _graph_builder{};
        size_t _index{};

    public:
        NodeRef() = default;
        explicit NodeRef(GraphBuilder& graph_builder, size_t index);

        TypeErasedNode& node() const;

        SignalRef operator[](size_t output_index) const;
        SignalRef operator[](std::string_view output_name) const;
        operator SignalRef() const;

        template<class... Refs>
        NodeRef operator()(Refs&&... refs) const;
        NodeRef operator()(std::initializer_list<NamedRef> refs) const;

        std::string to_string() const;
    };

    class GraphBuilder {
        friend class NodeRef;
        friend struct SignalRef;

        std::string _parent_path;

        GraphNode::Nodes _nodes;
        GraphNode::Edges _edges;
        std::unordered_set<size_t> _placed_nodes;

        std::vector<InputConfig> _public_inputs;
        std::unordered_map<std::string_view, size_t> _input_name_to_index;

        std::vector<OutputConfig> _public_outputs;
        bool _outputs_defined{ false };

    public:
        explicit GraphBuilder(std::string_view parent_path = {}):
            _parent_path(parent_path)
        {
        }

        std::string debug_node_id(size_t index) const
        {
            assert(index < _nodes.size() && "node index out of bounds");
            std::string nested_path = _parent_path;
            if (!nested_path.empty()) {
                nested_path += ".";
            }
            nested_path += std::to_string(index);
            return nested_path;
        }

        SignalRef input()
        {
            _public_inputs.emplace_back();
            return SignalRef(*this, GRAPH_ID, _public_inputs.size() - 1);
        }

        SignalRef input(std::string_view name)
        {
            if (name.empty()) {
                details::error("input name cannot be empty");
            }
            if (_input_name_to_index.contains(name)) {
                details::error("input '" + std::string(name) + "' already exists");
            }

            _public_inputs.emplace_back(InputConfig{ .name = name });
            _input_name_to_index.emplace(name, _public_inputs.size() - 1);
            return SignalRef(*this, GRAPH_ID, _public_inputs.size() - 1);
        }

        template<class Node, class... Args>
        NodeRef node(Args&&... args)
        {
            _nodes.emplace_back(Node(std::forward<Args>(args)...));
            return NodeRef(*this, _nodes.size() - 1);
        }

        template<class Fn>
        NodeRef subgraph(Fn&& fn)
        {
            std::string nested_path = _parent_path;
            if (!nested_path.empty()) {
                nested_path += ".";
            }
            nested_path += std::to_string(_nodes.size());

            GraphBuilder g(nested_path);
            std::forward<Fn>(fn)(g);

            if (!g._outputs_defined) {
                details::error(
                    "builder " + g._parent_path + ": g.outputs(...) must be called before returning from subgraph()"
                );
            }

            return node<GraphNode>(std::move(g).build());
        }

        template<class... Refs>
        void outputs(Refs&&... refs)
        {
            if (_outputs_defined) {
                details::error("outputs(...) was already called on builder " + _parent_path);
            }

            std::array<SignalRef, sizeof...(Refs)> refs_array{ std::forward<Refs>(refs)... };
            _public_outputs.clear();
            _public_outputs.reserve(refs_array.size());

            for (size_t i = 0; i < refs_array.size(); ++i) {
                auto const& ref = refs_array[i];
                if (ref.graph_builder != this) {
                    details::error(
                        "builder " + _parent_path + ": outputs(...): "
                        "SignalRef at index " + std::to_string(i) + " "
                        "belongs to builder " + ref.graph_builder->_parent_path
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
                details::error("outputs(...) was already called on builder " + _parent_path);
            }

            _public_outputs.reserve(refs.size());
            std::unordered_set<std::string_view> output_names;

            size_t output_port = 0;
            for (auto const& ref : refs) {
                if (ref.signal.graph_builder != this) {
                    details::error(
                        "builder " + _parent_path + ": outputs({ ... }): "
                        + ref.signal.to_string() + " belongs to another builder"
                    );
                }

                if (ref.name.empty()) {
                    details::error(
                        "builder " + _parent_path + ": named outputs must not use empty names"
                    );
                }

                if (output_names.contains(ref.name)) {
                    details::error(
                        "builder " + _parent_path + ": duplicate output name '" + std::string(ref.name) + "'"
                    );
                }
                output_names.insert(ref.name);

                _edges.emplace(GraphEdge{
                    PortId{ ref.signal.node_index, ref.signal.output_port },
                    PortId{ GRAPH_ID, output_port++ },
                    });
                _public_outputs.emplace_back(OutputConfig{ .name = ref.name });
            }

            _outputs_defined = true;
        }

        GraphNode build()&&
        {
            if (!_outputs_defined) {
                details::error("builder " + _parent_path + ": g.outputs(...) must be called before build()");
            }

            return GraphNode(
                std::move(_nodes),
                std::move(_edges),
                std::move(_public_inputs),
                std::move(_public_outputs)
            );
        }
    };

    SignalRef::SignalRef(GraphBuilder& graph_builder_, size_t node_index, size_t output_port) :
        graph_builder(&graph_builder_),
        node_index(node_index),
        output_port(output_port)
    {
        if (node_index == GRAPH_ID) {
            if (output_port >= graph_builder->_public_inputs.size()) {
                details::error(
                    "graph input port " + std::to_string(output_port) + " "
                    "is out of bounds in builder " + graph_builder->_parent_path + ", "
                    "public_inputs.size() = " + std::to_string(graph_builder->_public_inputs.size())
                );
            }
            return;
        }

        if (node_index >= graph_builder->_nodes.size()) {
            details::error(
                "node at index " + std::to_string(node_index) + " "
                "is out of bounds in builder " + graph_builder->_parent_path + ", "
                "nodes.size() = " + std::to_string(graph_builder->_nodes.size())
            );
        }

        auto& node = graph_builder->_nodes[node_index];
        size_t num_outputs = get_num_outputs(node);
        if (output_port >= num_outputs) {
            details::error(
                "output port " + std::to_string(output_port) + " of "
                "node at index " + std::to_string(node_index) + " in "
                "builder " + graph_builder->_parent_path + " "
                "is out of bounds, get_num_outputs(node) = " + std::to_string(num_outputs)
            );
        }
    }

    std::string SignalRef::to_string() const
    {
        if (!graph_builder) {
            return "empty signal";
        }
        if (node_index == GRAPH_ID) {
            return "graph input " + std::to_string(output_port) + " in builder " + graph_builder->_parent_path;
        }
        return "signal at address " + graph_builder->debug_node_id(node_index) + ":" + std::to_string(output_port);
    }


    NodeRef::NodeRef(GraphBuilder& graph_builder, size_t index) :
        _graph_builder(&graph_builder),
        _index(index)
    {
    }

    TypeErasedNode& NodeRef::node() const
    {
        if (!_graph_builder) {
            details::error("attempted to use a null NodeRef");
        }
        return _graph_builder->_nodes[_index];
    }

    SignalRef NodeRef::operator[](size_t output_index) const
    {
        if (!_graph_builder) {
            details::error("attempted to use a null NodeRef");
        }
        return SignalRef(*_graph_builder, _index, output_index);
    }

    SignalRef NodeRef::operator[](std::string_view output_name) const
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

    NodeRef::operator SignalRef() const
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
        std::array<SignalRef, sizeof...(Refs)> refs_array{ std::forward<Refs>(refs)... };

        if (_graph_builder->_placed_nodes.contains(_index)) {
            details::error(
                to_string() + " "
                "was already placed in the graph"
            );
        }

        size_t num_inputs = get_num_inputs(node());
        if (sizeof...(Refs) > num_inputs) {
            details::error(
                to_string() + " "
                "has at most " + std::to_string(num_inputs) + " inputs, "
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

            PortId source{ ref.node_index, ref.output_port };
            PortId target{ _index, input_port };
            _graph_builder->_edges.emplace(GraphEdge{ source, target });
        }

        _graph_builder->_placed_nodes.insert(_index);
        return *this;
    }

    NodeRef NodeRef::operator()(std::initializer_list<NamedRef> refs) const
    {
        if (!_graph_builder) {
            details::error("attempted to use a null NodeRef");
        }
        if (_graph_builder->_placed_nodes.contains(_index)) {
            details::error(
                to_string() + " "
                "was already placed in the graph"
            );
        }

        auto inputs = get_inputs(node());
        std::vector<bool> placed_inputs(inputs.size(), false);

        for (auto const& ref : refs) {
            if (ref.name.empty()) {
                details::error(
                    "named inputs must not use empty names on " + to_string()
                );
            }

            if (ref.signal.graph_builder != _graph_builder) {
                details::error(
                    ref.signal.to_string() + " "
                    "does not belong to the same builder as " + to_string()
                );
            }

            bool placed = false;
            for (size_t input_port = 0; input_port < inputs.size(); ++input_port) {
                if (ref.name == inputs[input_port].name) {
                    if (placed_inputs[input_port]) {
                        details::error(
                            "input '" + std::string(ref.name) + "' "
                            "was specified more than once on " + to_string()
                        );
                    }

                    PortId source{ ref.signal.node_index, ref.signal.output_port };
                    PortId target{ _index, input_port };
                    _graph_builder->_edges.emplace(GraphEdge{ source, target });

                    placed_inputs[input_port] = true;
                    placed = true;
                    break;
                }
            }

            if (!placed) {
                details::error(
                    "an input port named '" + std::string(ref.name) + "' "
                    "does not exist on " + to_string()
                );
            }
        }

        _graph_builder->_placed_nodes.insert(_index);
        return *this;
    }

    std::string NodeRef::to_string() const
    {
        if (!_graph_builder) {
            return "empty node";
        }
        return "node at address " + _graph_builder->debug_node_id(_index);
    }
}
