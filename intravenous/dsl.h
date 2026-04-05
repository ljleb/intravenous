#pragma once
#include "basic_nodes/routing.h"
#include "basic_nodes/arithmetic.h"
#include "graph_build.h"
#include "graph_node.h"

#include <array>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <initializer_list>
#include <memory>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace iv {
    class GraphBuilder;
    class NodeRef;
    template<class Node>
    class TypedNodeRef;
    template<class Node>
    class StructuredNodeRef;

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

    struct EventRef {
        GraphBuilder* graph_builder {};
        size_t node_index {};
        size_t output_port {};

        EventRef() = default;
        explicit EventRef(GraphBuilder& graph_builder_, size_t node_index, size_t output_port);
        operator PortId() const { return { node_index, output_port }; }

        std::string to_string() const;
    };

    struct NamedRef {
        std::string_view name;
        std::variant<SignalRef, Sample> value;

        NamedRef(std::string_view name, SignalRef signal): name(name), value(signal) {}
        NamedRef(std::string_view name, Sample sample): name(name), value(sample) {}
    };

    template<size_t N>
    struct fixed_string {
        char value[N];

        constexpr fixed_string(char const (&str)[N])
        {
            std::ranges::copy(str, value);
        }

        constexpr std::string_view view() const
        {
            return std::string_view(value, N - 1);
        }
    };

    template<size_t N>
    fixed_string(char const (&)[N]) -> fixed_string<N>;

    template<fixed_string Name, class T>
    struct NamedArg {
        using value_type = T;
        static constexpr auto name = Name;

        T value;
    };

    template<fixed_string Name>
    struct PortName {
        template<class T>
        constexpr auto operator=(T&& value) const
        {
            return NamedArg<Name, std::remove_cvref_t<T>>{ std::forward<T>(value) };
        }
    };

    template<fixed_string Name>
    inline constexpr PortName<Name> named{};

    struct EventPortsTag {};
    struct SamplePortsTag {};

    inline constexpr EventPortsTag events {};
    inline constexpr SamplePortsTag samples {};

    namespace literals {
        template<fixed_string Name>
        consteval auto operator""_P()
        {
            return PortName<Name>{};
        }
    }

    using namespace literals;

    struct BuilderNode {
        std::vector<InputConfig> input_configs;
        std::vector<OutputConfig> output_configs;
        std::vector<EventInputConfig> event_input_configs;
        std::vector<EventOutputConfig> event_output_configs;
        std::function<TypeErasedNode(size_t)> materialize {};
        std::vector<std::vector<PortId>> subgraph_input_targets {};
        std::vector<PortId> subgraph_output_sources {};
        std::vector<std::vector<PortId>> subgraph_event_input_targets {};
        std::vector<PortId> subgraph_event_output_sources {};

        std::vector<InputConfig> const& inputs() const
        {
            return input_configs;
        }

        std::vector<OutputConfig> const& outputs() const
        {
            return output_configs;
        }

        std::vector<EventInputConfig> const& event_inputs() const
        {
            return event_input_configs;
        }

        std::vector<EventOutputConfig> const& event_outputs() const
        {
            return event_output_configs;
        }
    };

    namespace details {
        template<typename Inputs>
        struct fixed_input_count : std::integral_constant<size_t, std::dynamic_extent> {};

        template<size_t N>
        struct fixed_input_count<std::array<InputConfig, N>> : std::integral_constant<size_t, N> {};

        template<size_t N>
        struct fixed_input_count<std::span<InputConfig const, N>> : std::integral_constant<size_t, N> {};

        template<size_t N>
        struct fixed_input_count<std::span<InputConfig, N>> : std::integral_constant<size_t, N> {};

        template<typename Node>
        inline constexpr size_t fixed_input_count_v = fixed_input_count<
            std::remove_cvref_t<decltype(get_inputs(std::declval<Node const&>()))>
        >::value;

        template<>
        inline constexpr size_t fixed_input_count_v<void> = std::dynamic_extent;

        template<typename Node>
        inline constexpr bool has_fixed_input_count_v =
            (fixed_input_count_v<Node> != std::dynamic_extent);

        template<typename Outputs>
        struct fixed_output_count : std::integral_constant<size_t, std::dynamic_extent> {};

        template<size_t N>
        struct fixed_output_count<std::array<OutputConfig, N>> : std::integral_constant<size_t, N> {};

        template<size_t N>
        struct fixed_output_count<std::span<OutputConfig const, N>> : std::integral_constant<size_t, N> {};

        template<size_t N>
        struct fixed_output_count<std::span<OutputConfig, N>> : std::integral_constant<size_t, N> {};

        template<typename Node>
        inline constexpr size_t fixed_output_count_v = fixed_output_count<
            std::remove_cvref_t<decltype(get_outputs(std::declval<Node const&>()))>
        >::value;

        template<>
        inline constexpr size_t fixed_output_count_v<void> = std::dynamic_extent;

        template<typename Node>
        inline constexpr bool has_fixed_output_count_v =
            (fixed_output_count_v<Node> != std::dynamic_extent);

        template<typename Node>
        inline constexpr bool should_preserve_node_type_v =
            has_fixed_input_count_v<std::remove_cvref_t<Node>>
            || has_fixed_output_count_v<std::remove_cvref_t<Node>>;

        template<typename Node>
        using node_ref_for_t = std::conditional_t<
            has_fixed_output_count_v<std::remove_cvref_t<Node>>,
            StructuredNodeRef<std::remove_cvref_t<Node>>,
            std::conditional_t<
                should_preserve_node_type_v<std::remove_cvref_t<Node>>,
                TypedNodeRef<std::remove_cvref_t<Node>>,
                NodeRef
            >
        >;

        template<class T>
        struct is_named_arg : std::false_type {};

        template<fixed_string Name, class T>
        struct is_named_arg<NamedArg<Name, T>> : std::true_type {};

        template<class T>
        inline constexpr bool is_named_arg_v = is_named_arg<std::remove_cvref_t<T>>::value;

        template<class T>
        inline constexpr bool is_event_like_v = std::convertible_to<std::remove_cvref_t<T>, EventRef>;

        template<class T>
        inline constexpr bool arg_is_event_v = is_event_like_v<T>;

        template<fixed_string Name, class T>
        inline constexpr bool arg_is_event_v<NamedArg<Name, T>> = is_event_like_v<T>;

        enum class call_domain {
            signal,
            event,
            mixed,
        };

        template<class... Args>
        consteval bool named_args_follow_positionals_only()
        {
            bool seen_named = false;
            bool valid = true;
            ((valid = valid && (!seen_named || is_named_arg_v<Args>),
              seen_named = seen_named || is_named_arg_v<Args>), ...);
            return valid;
        }

        template<class... Args>
        inline constexpr bool named_args_follow_positionals_only_v =
            named_args_follow_positionals_only<Args...>();

        template<class A, class B>
        inline constexpr bool same_named_port_v = false;

        template<fixed_string A, class TA, fixed_string B, class TB>
        inline constexpr bool same_named_port_v<NamedArg<A, TA>, NamedArg<B, TB>> = (A.view() == B.view());

        template<class Arg, class... Rest>
        inline constexpr bool named_port_not_in_rest_v =
            (!same_named_port_v<std::remove_cvref_t<Arg>, std::remove_cvref_t<Rest>> && ...);

        template<class... Args>
        struct unique_named_args : std::true_type {};

        template<class Arg, class... Rest>
        struct unique_named_args<Arg, Rest...> : std::bool_constant<
            (!is_named_arg_v<Arg> || named_port_not_in_rest_v<Arg, Rest...>)
            && unique_named_args<Rest...>::value
        > {};

        template<class... Args>
        inline constexpr bool unique_named_args_v = unique_named_args<Args...>::value;

        template<class... Args>
        inline constexpr bool has_event_args_v = (false || ... || arg_is_event_v<std::remove_cvref_t<Args>>);

        template<class... Args>
        inline constexpr bool has_signal_args_v = (false || ... || !arg_is_event_v<std::remove_cvref_t<Args>>);

        template<class... Args>
        inline constexpr call_domain call_domain_v =
            has_event_args_v<Args...>
                ? (has_signal_args_v<Args...> ? call_domain::mixed : call_domain::event)
                : call_domain::signal;

        template<class... Args>
        inline constexpr size_t arg_count_v = sizeof...(Args);

        template<class Node, class... Args>
        inline constexpr bool accepts_total_arg_count_v =
            std::same_as<Node, void>
            || call_domain_v<Args...> == call_domain::event
            || !has_fixed_input_count_v<Node>
            || arg_count_v<Args...> <= fixed_input_count_v<Node>;

        template<class... Args>
        inline constexpr bool valid_node_call_args_v =
            named_args_follow_positionals_only_v<Args...>
            && unique_named_args_v<Args...>
            && call_domain_v<Args...> != call_domain::mixed;

        template<class Node, class... Args>
        concept node_call_enabled =
            valid_node_call_args_v<Args...>
            && accepts_total_arg_count_v<Node, Args...>;
    }

    template<class Derived, class Node = void>
    class NodeRefBase {
    protected:
        GraphBuilder* _graph_builder{};
        size_t _index{};

        friend class EventPortView;

    private:
        Derived const& derived() const
        {
            return static_cast<Derived const&>(*this);
        }

    public:
        NodeRefBase() = default;
        explicit NodeRefBase(GraphBuilder& graph_builder, size_t index) :
            _graph_builder(&graph_builder),
            _index(index)
        {}

        BuilderNode const& node() const;

        SignalRef operator[](size_t output_index) const;
        SignalRef operator[](std::string_view output_name) const;
        operator SignalRef() const;

        template<class... Args>
        requires(details::node_call_enabled<Node, Args...>)
        Derived operator()(Args&&... args) const;

        template<class... Args>
        requires(!details::node_call_enabled<Node, Args...>)
        Derived operator()(Args&&... args) const = delete;

        SignalRef detach(size_t loop_block_size = 1) const;

        std::string to_string() const;
    };

    class NodeRef : public NodeRefBase<NodeRef> {
    public:
        using NodeRefBase<NodeRef>::NodeRefBase;
    };

    class EventPortView {
        NodeRef _node;

        EventRef event_ref(size_t output_index) const
        {
            auto outputs = get_event_outputs(_node.node());
            if (output_index >= outputs.size()) {
                details::error(
                    "event output port " + std::to_string(output_index) + " of "
                    + _node.to_string() + " is out of bounds"
                );
            }
            return EventRef(*_node._graph_builder, _node._index, output_index);
        }

        EventRef event_ref(std::string_view output_name) const
        {
            auto outputs = get_event_outputs(_node.node());
            for (size_t output_port = 0; output_port < outputs.size(); ++output_port) {
                if (outputs[output_port].name == output_name) {
                    return EventRef(*_node._graph_builder, _node._index, output_port);
                }
            }

            details::error(
                "an event output port named '" + std::string(output_name) + "' "
                "does not exist on " + _node.to_string()
            );
        }

    public:
        EventPortView() = default;

        explicit EventPortView(NodeRef node) :
            _node(std::move(node))
        {}

        EventRef operator[](size_t output_index) const
        {
            return event_ref(output_index);
        }

        EventRef operator[](std::string_view output_name) const
        {
            return event_ref(output_name);
        }

        std::string to_string() const
        {
            return _node.to_string();
        }

        NodeRef const& node() const
        {
            return _node;
        }
    };

    template<class Node>
    class TypedNodeRef : public NodeRefBase<TypedNodeRef<Node>, std::remove_cvref_t<Node>> {
        using Base = NodeRefBase<TypedNodeRef<Node>, std::remove_cvref_t<Node>>;

    public:
        using NodeType = std::remove_cvref_t<Node>;
        using Base::Base;

        operator NodeRef() const
        {
            if (!this->_graph_builder) {
                return NodeRef();
            }
            return NodeRef(*this->_graph_builder, this->_index);
        }
    };

    template<class Node>
    class StructuredNodeRef : public TypedNodeRef<Node> {
        static constexpr size_t output_count = details::fixed_output_count_v<Node>;
        static_assert(details::has_fixed_output_count_v<Node>, "StructuredNodeRef requires a fixed output count");
        using Base = TypedNodeRef<Node>;

    public:
        using NodeType = std::remove_cvref_t<Node>;
        using Base::Base;

        StructuredNodeRef() = default;
        explicit StructuredNodeRef(GraphBuilder& graph_builder, size_t index) :
            Base(graph_builder, index)
        {}

        template<size_t I>
        SignalRef get() const
        {
            static_assert(I < output_count);
            if (!this->_graph_builder) {
                details::error("attempted to use a null NodeRef");
            }
            return SignalRef(*this->_graph_builder, this->_index, I);
        }
    };

    using DetachedSignalInfo = DetachedInfo;

    class GraphBuilder {
        template<class Derived, class Node>
        friend class NodeRefBase;
        friend struct SignalRef;
        friend struct EventRef;

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
        std::unordered_set<GraphEventEdge> _event_edges;
        std::unordered_set<PortId> _placed_input_ports;
        std::unordered_set<PortId> _placed_event_input_ports;

        std::vector<InputConfig> _public_inputs;
        std::unordered_map<std::string, size_t> _input_name_to_index;
        std::vector<EventInputConfig> _public_event_inputs;
        std::unordered_map<std::string, size_t> _event_input_name_to_index;

        std::vector<OutputConfig> _public_outputs;
        std::vector<EventOutputConfig> _public_event_outputs;
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

        EventRef event_input(std::string_view name, EventTypeId type)
        {
            if (name.empty()) {
                details::error("event input name cannot be empty");
            }
            if (_event_input_name_to_index.contains(std::string(name))) {
                details::error("event input '" + std::string(name) + "' already exists");
            }

            _public_event_inputs.emplace_back(EventInputConfig { .name = std::string(name), .type = type });
            _event_input_name_to_index.emplace(std::string(name), _public_event_inputs.size() - 1);
            return EventRef(*this, GRAPH_ID, _public_event_inputs.size() - 1);
        }

        template<class Node, class... Args>
        details::node_ref_for_t<Node> node(Args&&... args)
        {
            using StoredNode = std::remove_cvref_t<Node>;
            StoredNode node_value(std::forward<Args>(args)...);
            auto inputs = get_inputs(node_value);
            auto outputs = get_outputs(node_value);
            auto event_inputs = get_event_inputs(node_value);
            auto event_outputs = get_event_outputs(node_value);

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
                .event_input_configs = std::vector<EventInputConfig>(std::begin(event_inputs), std::end(event_inputs)),
                .event_output_configs = std::vector<EventOutputConfig>(std::begin(event_outputs), std::end(event_outputs)),
                .materialize = std::move(materialize),
            });
            if constexpr (details::has_fixed_output_count_v<StoredNode>) {
                return StructuredNodeRef<StoredNode>(*this, _nodes.size() - 1);
            } else if constexpr (details::should_preserve_node_type_v<StoredNode>) {
                return TypedNodeRef<StoredNode>(*this, _nodes.size() - 1);
            } else {
                return NodeRef(*this, _nodes.size() - 1);
            }
        }

        NodeRef node(GraphBuilder const& child)
        {
            if (!child._outputs_defined) {
                details::error(
                    "builder " + child._builder_id.value + ": g.outputs(...) must be called before insertion"
                );
            }

            size_t const child_detach_offset = _next_detach_id;
            _next_detach_id += child._next_detach_id;

            size_t const placeholder_node = _nodes.size();
            size_t const child_node_offset = placeholder_node + 1;

            auto remap_child_port = [&](PortId port) {
                if (port.node == GRAPH_ID) {
                    return PortId{ placeholder_node, port.port };
                }
                return PortId{ child_node_offset + port.node, port.port };
            };

            _nodes.emplace_back(BuilderNode{
                .input_configs = child._public_inputs,
                .output_configs = child._public_outputs,
                .event_input_configs = child._public_event_inputs,
                .event_output_configs = child._public_event_outputs,
                .subgraph_input_targets = std::vector<std::vector<PortId>>(child._public_inputs.size()),
                .subgraph_output_sources = std::vector<PortId>(child._public_outputs.size()),
                .subgraph_event_input_targets = std::vector<std::vector<PortId>>(child._public_event_inputs.size()),
                .subgraph_event_output_sources = std::vector<PortId>(child._public_event_outputs.size()),
            });

            for (auto const& child_node : child._nodes) {
                BuilderNode copied_node = child_node;
                if (copied_node.materialize) {
                    auto materialize = std::move(copied_node.materialize);
                    copied_node.materialize = [materialize = std::move(materialize), child_detach_offset](size_t parent_detach_offset) {
                        return materialize(parent_detach_offset + child_detach_offset);
                    };
                } else {
                    for (auto& targets : copied_node.subgraph_input_targets) {
                        for (auto& target : targets) {
                            target = remap_child_port(target);
                        }
                    }
                    for (auto& source : copied_node.subgraph_output_sources) {
                        source = remap_child_port(source);
                    }
                    for (auto& targets : copied_node.subgraph_event_input_targets) {
                        for (auto& target : targets) {
                            target = remap_child_port(target);
                        }
                    }
                    for (auto& source : copied_node.subgraph_event_output_sources) {
                        source = remap_child_port(source);
                    }
                }
                _nodes.push_back(std::move(copied_node));
            }

            for (GraphEdge const& edge : child._edges) {
                PortId const source = remap_child_port(edge.source);
                PortId const target = remap_child_port(edge.target);
                if (source.node == placeholder_node && target.node == placeholder_node) {
                    _nodes[placeholder_node].subgraph_output_sources[target.port] = source;
                } else if (source.node == placeholder_node) {
                    _nodes[placeholder_node].subgraph_input_targets[source.port].push_back(target);
                } else if (target.node == placeholder_node) {
                    _nodes[placeholder_node].subgraph_output_sources[target.port] = source;
                } else {
                    _edges.emplace(GraphEdge{ source, target });
                }
            }
            for (GraphEventEdge const& edge : child._event_edges) {
                PortId const source = remap_child_port(edge.source);
                PortId const target = remap_child_port(edge.target);
                if (source.node == placeholder_node && target.node == placeholder_node) {
                    _nodes[placeholder_node].subgraph_event_output_sources[target.port] = source;
                } else if (source.node == placeholder_node) {
                    _nodes[placeholder_node].subgraph_event_input_targets[source.port].push_back(target);
                } else if (target.node == placeholder_node) {
                    _nodes[placeholder_node].subgraph_event_output_sources[target.port] = source;
                } else {
                    _event_edges.emplace(GraphEventEdge{ source, target, edge.conversion });
                }
            }

            for (PortId const port : child._placed_input_ports) {
                if (port.node != GRAPH_ID) {
                    _placed_input_ports.insert(PortId{ child_node_offset + port.node, port.port });
                }
            }

            for (auto const& [source, info] : child._detached_info_by_source) {
                _detached_info_by_source.emplace(
                    remap_child_port(source),
                    DetachedSignalInfo{
                        .detach_id = info.detach_id + child_detach_offset,
                        .original_source = remap_child_port(info.original_source),
                        .writer_node = child_node_offset + info.writer_node,
                        .reader_output = remap_child_port(info.reader_output),
                        .loop_block_size = info.loop_block_size,
                    }
                );
            }

            for (PortId const reader_output : child._detached_reader_outputs) {
                _detached_reader_outputs.insert(remap_child_port(reader_output));
            }

            return NodeRef(*this, placeholder_node);
        }

        template<class... Refs>
        void event_outputs(Refs&&... refs)
        {
            std::array<EventRef, sizeof...(Refs)> refs_array{
                std::forward<Refs>(refs)...
            };
            _public_event_outputs.clear();
            _public_event_outputs.reserve(refs_array.size());

            for (size_t i = 0; i < refs_array.size(); ++i) {
                auto const& ref = refs_array[i];
                if (ref.graph_builder != this) {
                    details::error(
                        "builder " + _builder_id.value + ": event_outputs(...): "
                        "EventRef at index " + std::to_string(i) + " "
                        "belongs to another builder"
                    );
                }

                auto source_type = (ref.node_index == GRAPH_ID)
                    ? _public_event_inputs[ref.output_port].type
                    : _nodes[ref.node_index].event_output_configs[ref.output_port].type;
                _event_edges.emplace(GraphEventEdge{
                    PortId{ ref.node_index, ref.output_port },
                    PortId{ GRAPH_ID, i },
                    EventConversionRegistry::instance().plan(source_type, source_type)
                });
                _public_event_outputs.emplace_back(EventOutputConfig{
                    .type = source_type,
                });
            }
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
                .edges = {},
                .event_edges = {},
                .detached_info_by_source = {},
                .detached_reader_outputs = {},
            };
            std::vector<size_t> runtime_node_indices(_nodes.size(), GRAPH_ID);
            std::unordered_map<PortId, PortId> source_of;
            std::unordered_map<PortId, GraphEventEdge> event_source_of;
            for (GraphEdge const& edge : _edges) {
                source_of[edge.target] = edge.source;
            }
            for (GraphEventEdge const& edge : _event_edges) {
                event_source_of[edge.target] = edge;
            }

            for (size_t node_i = 0; node_i < _nodes.size(); ++node_i) {
                if (!_nodes[node_i].materialize) {
                    continue;
                }
                runtime_node_indices[node_i] = g.nodes.size();
                g.nodes.push_back(_nodes[node_i].materialize(detach_id_offset));
                g.node_ids.push_back(node_id(node_i));
            }

            auto materialize_placeholder_default = [&](size_t placeholder_node, size_t input_port) {
                runtime_node_indices.push_back(g.nodes.size());
                g.nodes.push_back(TypeErasedNode(Constant(_nodes[placeholder_node].input_configs[input_port].default_value)));
                g.node_ids.push_back(
                    node_id(placeholder_node) + ".default." + std::to_string(input_port)
                );
                return PortId{ g.nodes.size() - 1, 0 };
            };

            auto resolve_source = [&](auto const& self, PortId source) -> PortId {
                if (source.node == GRAPH_ID) {
                    return source;
                }
                if (_nodes[source.node].materialize) {
                    return PortId{ runtime_node_indices[source.node], source.port };
                }
                PortId const passthrough_source = _nodes[source.node].subgraph_output_sources[source.port];
                if (passthrough_source.node == source.node) {
                    return self(self, source_of.at(passthrough_source));
                }
                return self(self, passthrough_source);
            };

            auto resolve_event_source = [&](auto const& self, PortId source) -> PortId {
                if (source.node == GRAPH_ID) {
                    return source;
                }
                if (_nodes[source.node].materialize) {
                    return PortId{ runtime_node_indices[source.node], source.port };
                }
                PortId const passthrough_source = _nodes[source.node].subgraph_event_output_sources[source.port];
                if (passthrough_source.node == source.node) {
                    return self(self, event_source_of.at(passthrough_source).source);
                }
                return self(self, passthrough_source);
            };

            auto add_target_edges = [&](auto const& self, PortId source, PortId target) -> void {
                if (target.node == GRAPH_ID) {
                    g.edges.emplace(GraphEdge{ source, target });
                    return;
                }
                if (_nodes[target.node].materialize) {
                    g.edges.emplace(GraphEdge{ source, { runtime_node_indices[target.node], target.port } });
                    return;
                }
                for (PortId const child_target : _nodes[target.node].subgraph_input_targets[target.port]) {
                    self(self, source, child_target);
                }
            };

            auto add_event_target_edges = [&](auto const& self, GraphEventEdge edge, PortId target) -> void {
                if (target.node == GRAPH_ID) {
                    g.event_edges.emplace(GraphEventEdge{ edge.source, target, std::move(edge.conversion) });
                    return;
                }
                if (_nodes[target.node].materialize) {
                    g.event_edges.emplace(GraphEventEdge{
                        edge.source,
                        { runtime_node_indices[target.node], target.port },
                        std::move(edge.conversion)
                    });
                    return;
                }
                for (PortId const child_target : _nodes[target.node].subgraph_event_input_targets[target.port]) {
                    self(self, GraphEventEdge{ edge.source, child_target, edge.conversion }, child_target);
                }
            };

            for (GraphEdge const& edge : _edges) {
                add_target_edges(add_target_edges, resolve_source(resolve_source, edge.source), edge.target);
            }
            for (GraphEventEdge const& edge : _event_edges) {
                GraphEventEdge resolved = edge;
                resolved.source = resolve_event_source(resolve_event_source, edge.source);
                add_event_target_edges(add_event_target_edges, std::move(resolved), edge.target);
            }

            for (size_t node_i = 0; node_i < _nodes.size(); ++node_i) {
                if (_nodes[node_i].materialize) {
                    continue;
                }

                for (size_t input_port = 0; input_port < _nodes[node_i].input_configs.size(); ++input_port) {
                    PortId const placeholder_input{ node_i, input_port };
                    if (source_of.contains(placeholder_input)) {
                        continue;
                    }

                    auto const default_source = materialize_placeholder_default(node_i, input_port);
                    for (PortId const child_target : _nodes[node_i].subgraph_input_targets[input_port]) {
                        add_target_edges(add_target_edges, default_source, child_target);
                    }
                }
            }

            for (auto const& [source, info] : _detached_info_by_source) {
                PortId const remapped_source = resolve_source(resolve_source, source);
                g.detached_info_by_source.emplace(remapped_source, DetachedSignalInfo{
                    .detach_id = info.detach_id,
                    .original_source = remapped_source,
                    .writer_node = runtime_node_indices[info.writer_node],
                    .reader_output = resolve_source(resolve_source, info.reader_output),
                    .loop_block_size = info.loop_block_size,
                });
            }

            for (PortId const reader_output : _detached_reader_outputs) {
                g.detached_reader_outputs.insert(resolve_source(resolve_source, reader_output));
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
            auto execution_plan = details::build_execution_plan(g.nodes, g.edges, g.event_edges, detached);

            return Graph(details::build_graph_artifact(
                _builder_id.value,
                std::move(g.nodes),
                std::move(g.node_ids),
                std::move(g.edges),
                std::move(g.event_edges),
                std::move(detached),
                std::move(execution_plan),
                _public_inputs,
                _public_outputs,
                _public_event_inputs,
                _public_event_outputs
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

    inline SignalRef operator~(SignalRef signal)
    {
        return signal.detach();
    }

    template<class T>
    requires (
        std::convertible_to<std::remove_cvref_t<T>, SignalRef> &&
        !std::same_as<std::remove_cvref_t<T>, SignalRef>
    )
    SignalRef operator~(T&& value)
    {
        return static_cast<SignalRef>(std::forward<T>(value)).detach();
    }

    template<class L, class R>
    requires (
        SignalLike<L> &&
        std::convertible_to<std::remove_cvref_t<R>, NodeRef>
    )
    SignalRef connect_unary_node(L&& lhs, R&& rhs, std::string_view op_name)
    {
        SignalRef source = std::forward<L>(lhs);
        NodeRef target = static_cast<NodeRef>(std::forward<R>(rhs));

        auto const inputs = get_inputs(target.node());
        auto const outputs = get_outputs(target.node());

        if (inputs.size() != 1) {
            details::error(
                std::string(op_name) + " requires target to have exactly 1 input; got " +
                std::to_string(inputs.size()) + " inputs on " + target.to_string()
            );
        }

        target(source);
        if (outputs.empty()) {
            return source;
        }
        if (outputs.size() != 1) {
            details::error(
                std::string(op_name) + " requires target to have at most 1 output when used as an expression; got " +
                std::to_string(outputs.size()) + " outputs on " + target.to_string()
            );
        }
        return static_cast<SignalRef>(target);
    }

    template<class L, class R>
    requires (
        SignalLike<L> &&
        std::convertible_to<std::remove_cvref_t<R>, NodeRef>
    )
    SignalRef operator>>(L&& lhs, R&& rhs)
    {
        return connect_unary_node(std::forward<L>(lhs), std::forward<R>(rhs), "operator>>");
    }

    template<class L, class R>
    requires (
        std::convertible_to<std::remove_cvref_t<L>, NodeRef> &&
        SignalLike<R>
    )
    SignalRef operator<<(L&& lhs, R&& rhs)
    {
        return connect_unary_node(std::forward<R>(rhs), std::forward<L>(lhs), "operator<<");
    }

    inline EventRef connect_unary_event_node(EventPortView const& source, EventPortView const& target, std::string_view op_name)
    {
        auto const source_event_outputs = get_event_outputs(source.node().node());
        auto const event_inputs = get_event_inputs(target.node().node());
        auto const event_outputs = get_event_outputs(target.node().node());

        if (source_event_outputs.size() != 1) {
            details::error(
                std::string(op_name) + " requires source to have exactly 1 event output; got " +
                std::to_string(source_event_outputs.size()) + " event outputs on " + source.to_string()
            );
        }

        if (event_inputs.size() != 1) {
            details::error(
                std::string(op_name) + " requires target to have exactly 1 event input; got " +
                std::to_string(event_inputs.size()) + " event inputs on " + target.to_string()
            );
        }

        target.node()(source[0]);
        if (event_outputs.empty()) {
            return source[0];
        }
        if (event_outputs.size() != 1) {
            details::error(
                std::string(op_name) + " requires target to have at most 1 event output when used as an expression; got " +
                std::to_string(event_outputs.size()) + " event outputs on " + target.to_string()
            );
        }
        return target[0];
    }

    inline EventRef operator>>(EventPortView const& lhs, EventPortView const& rhs)
    {
        return connect_unary_event_node(lhs, rhs, "operator>>");
    }

    inline EventRef operator<<(EventPortView const& lhs, EventPortView const& rhs)
    {
        return connect_unary_event_node(rhs, lhs, "operator<<");
    }

    template<fixed_string Name, class R>
    requires std::convertible_to<std::remove_cvref_t<R>, NodeRef>
    SignalRef operator<<(PortName<Name>, R&& rhs)
    {
        NodeRef node = static_cast<NodeRef>(std::forward<R>(rhs));
        return node[Name.view()];
    }

    template<class L, fixed_string Name>
    requires std::convertible_to<std::remove_cvref_t<L>, NodeRef>
    SignalRef operator>>(L&& lhs, PortName<Name>)
    {
        NodeRef node = static_cast<NodeRef>(std::forward<L>(lhs));
        return node[Name.view()];
    }

    template<class T>
    requires std::convertible_to<std::remove_cvref_t<T>, NodeRef>
    EventPortView operator>>(T&& node, EventPortsTag)
    {
        return EventPortView(static_cast<NodeRef>(std::forward<T>(node)));
    }

    template<class T>
    requires std::convertible_to<std::remove_cvref_t<T>, NodeRef>
    EventPortView operator<<(EventPortsTag, T&& node)
    {
        return EventPortView(static_cast<NodeRef>(std::forward<T>(node)));
    }

    template<class T>
    requires std::convertible_to<std::remove_cvref_t<T>, NodeRef>
    NodeRef operator>>(T&& node, SamplePortsTag)
    {
        return static_cast<NodeRef>(std::forward<T>(node));
    }

    template<class T>
    requires std::convertible_to<std::remove_cvref_t<T>, NodeRef>
    NodeRef operator<<(SamplePortsTag, T&& node)
    {
        return static_cast<NodeRef>(std::forward<T>(node));
    }

    inline NodeRef operator>>(EventPortView const& view, SamplePortsTag)
    {
        return view.node();
    }

    inline NodeRef operator<<(SamplePortsTag, EventPortView const& view)
    {
        return view.node();
    }

    template<fixed_string Name>
    EventRef operator<<(PortName<Name>, EventPortView const& view)
    {
        return view[Name.view()];
    }

    template<fixed_string Name>
    EventRef operator>>(EventPortView const& view, PortName<Name>)
    {
        return view[Name.view()];
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

    inline EventRef::EventRef(GraphBuilder& graph_builder_, size_t node_index, size_t output_port) :
        graph_builder(&graph_builder_),
        node_index(node_index),
        output_port(output_port)
    {
        if (node_index == GRAPH_ID) {
            if (output_port >= graph_builder->_public_event_inputs.size()) {
                details::error(
                    "graph event input port " + std::to_string(output_port) + " "
                    "is out of bounds in builder " + graph_builder->_builder_id.value
                );
            }
            return;
        }

        if (node_index >= graph_builder->_nodes.size()) {
            details::error(
                "node at index " + std::to_string(node_index) + " "
                "is out of bounds in builder " + graph_builder->_builder_id.value
            );
        }

        auto& node = graph_builder->_nodes[node_index];
        size_t num_outputs = node.event_output_configs.size();
        if (output_port >= num_outputs) {
            details::error(
                "event output port " + std::to_string(output_port) + " of "
                "node at index " + std::to_string(node_index) + " in "
                "builder " + graph_builder->_builder_id.value + " "
                "is out of bounds"
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

    inline std::string EventRef::to_string() const
    {
        if (!graph_builder) {
            return "empty event";
        }
        if (node_index == GRAPH_ID) {
            return "graph event input " + std::to_string(output_port) + " in builder " + graph_builder->_builder_id.value;
        }
        return "event at address " + graph_builder->node_id(node_index) + ":" + std::to_string(output_port);
    }

    template<class Derived, class Node>
    inline BuilderNode const& NodeRefBase<Derived, Node>::node() const
    {
        if (!_graph_builder) {
            details::error("attempted to use a null NodeRef");
        }
        return _graph_builder->_nodes[_index];
    }

    template<class Derived, class Node>
    inline SignalRef NodeRefBase<Derived, Node>::operator[](size_t output_index) const
    {
        if (!_graph_builder) {
            details::error("attempted to use a null NodeRef");
        }
        return SignalRef(*_graph_builder, _index, output_index);
    }

    template<class Derived, class Node>
    inline SignalRef NodeRefBase<Derived, Node>::operator[](std::string_view output_name) const
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

    template<class Derived, class Node>
    inline NodeRefBase<Derived, Node>::operator SignalRef() const
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

    template<class Derived, class Node>
    template<class... Args>
    requires(details::node_call_enabled<Node, Args...>)
    Derived NodeRefBase<Derived, Node>::operator()(Args&&... args) const
    {
        if (!_graph_builder) {
            details::error("attempted to use a null NodeRef");
        }

        constexpr auto call_domain = details::call_domain_v<Args...>;
        auto const inputs = get_inputs(node());
        auto const event_inputs = get_event_inputs(node());

        auto connect_input = [&](SignalRef ref, size_t input_port, std::string_view input_name = {}) {
            if (input_port >= inputs.size()) {
                details::error(
                    to_string() + " "
                    "has at most " + std::to_string(inputs.size()) + " inputs, "
                    "got " + std::to_string(input_port + 1)
                );
            }

            if (ref.graph_builder != _graph_builder) {
                details::error(
                    ref.to_string() + " "
                    "does not belong to the same builder as " + to_string()
                );
            }

            PortId const input_port_id = {_index, input_port};
            if (_graph_builder->_placed_input_ports.contains(input_port_id)) {
                details::error(
                    "input port " + std::string(input_name.empty() ? inputs[input_port].name : input_name) + " of " + to_string() + " "
                    "was already placed in the graph"
                );
            }

            _graph_builder->_placed_input_ports.insert(input_port_id);
            _graph_builder->_edges.emplace(GraphEdge{
                ref,
                PortId{ _index, input_port },
            });
        };

        auto connect_event = [&](EventRef ref, size_t input_port, std::string_view input_name = {}) {
            if (input_port >= event_inputs.size()) {
                details::error(
                    to_string() + " "
                    "has at most " + std::to_string(event_inputs.size()) + " event inputs, "
                    "got " + std::to_string(input_port + 1)
                );
            }
            if (ref.graph_builder != _graph_builder) {
                details::error(
                    ref.to_string() + " "
                    "does not belong to the same builder as " + to_string()
                );
            }

            PortId const input_port_id = { _index, input_port };
            if (_graph_builder->_placed_event_input_ports.contains(input_port_id)) {
                details::error(
                    "event input port " + std::string(input_name.empty() ? event_inputs[input_port].name : input_name) + " of " + to_string() + " "
                    "was already placed in the graph"
                );
            }

            auto const source_type = (ref.node_index == GRAPH_ID)
                ? _graph_builder->_public_event_inputs[ref.output_port].type
                : _graph_builder->_nodes[ref.node_index].event_output_configs[ref.output_port].type;

            _graph_builder->_placed_event_input_ports.insert(input_port_id);
            _graph_builder->_event_edges.emplace(GraphEventEdge{
                PortId{ ref.node_index, ref.output_port },
                PortId{ _index, input_port },
                EventConversionRegistry::instance().plan(source_type, event_inputs[input_port].type)
            });
        };

        auto connect_named = [&](auto&& named_arg) {
            using NamedArgT = std::remove_cvref_t<decltype(named_arg)>;
            constexpr std::string_view name = NamedArgT::name.view();

            if constexpr (call_domain == details::call_domain::event) {
                for (size_t input_port = 0; input_port < event_inputs.size(); ++input_port) {
                    if (event_inputs[input_port].name == name) {
                        connect_event(static_cast<EventRef>(named_arg.value), input_port, name);
                        return;
                    }
                }
            } else {
                for (size_t input_port = 0; input_port < inputs.size(); ++input_port) {
                    if (inputs[input_port].name == name) {
                        connect_input(_graph_builder->lift_to_signal(named_arg.value), input_port, name);
                        return;
                    }
                }
            }

            details::error(
                "an input port named '" + std::string(name) + "' "
                "does not exist on " + to_string()
            );
        };

        size_t positional_port = 0;
        auto const process_arg = [&](auto&& arg) {
            if constexpr (details::is_named_arg_v<decltype(arg)>) {
                connect_named(std::forward<decltype(arg)>(arg));
            } else if constexpr (call_domain == details::call_domain::event) {
                connect_event(static_cast<EventRef>(std::forward<decltype(arg)>(arg)), positional_port++);
            } else {
                connect_input(_graph_builder->lift_to_signal(std::forward<decltype(arg)>(arg)), positional_port++);
            }
        };

        (process_arg(std::forward<Args>(args)), ...);

        return derived();
    }

    template<class Derived, class Node>
    inline SignalRef NodeRefBase<Derived, Node>::detach(size_t loop_block_size) const
    {
        return static_cast<SignalRef>(*this).detach(loop_block_size);
    }

    template<class Derived, class Node>
    inline std::string NodeRefBase<Derived, Node>::to_string() const
    {
        if (!_graph_builder) {
            return "empty node";
        }
        return "node at address " + _graph_builder->node_id(_index);
    }

    template<size_t I, class Node>
    SignalRef get(StructuredNodeRef<Node> const& node_ref)
    {
        return node_ref.template get<I>();
    }

    template<size_t I, class Node>
    SignalRef get(StructuredNodeRef<Node>& node_ref)
    {
        return node_ref.template get<I>();
    }

    template<size_t I, class Node>
    SignalRef get(StructuredNodeRef<Node>&& node_ref)
    {
        return node_ref.template get<I>();
    }
}

namespace std {
    template<class Node>
    struct tuple_size<iv::StructuredNodeRef<Node>> :
        std::integral_constant<size_t, iv::details::fixed_output_count_v<Node>> {};

    template<size_t I, class Node>
    struct tuple_element<I, iv::StructuredNodeRef<Node>> {
        using type = iv::SignalRef;
    };
}
