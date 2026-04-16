#pragma once
#include "basic_nodes/routing.h"
#include "basic_nodes/arithmetic.h"
#include "basic_nodes/midi.h"
#include "compiler.h"
#include "node.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <initializer_list>
#include <memory>
#include <optional>
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
    class SampleNodeRef;
    template<class Node>
    class TypedNodeRef;
    template<class Node>
    class StructuredNodeRef;

    struct SamplePortRef {
        GraphBuilder* graph_builder{};
        size_t node_index{};
        size_t output_port{};

        SamplePortRef() = default;
        explicit SamplePortRef(GraphBuilder& graph_builder_, size_t node_index, size_t output_port);
        operator PortId() const { return { node_index, output_port }; }

        SamplePortRef detach(size_t loop_extra_latency = 1) const;
        void _add_source_span(uint32_t begin, uint32_t end) const;

        std::string to_string() const;
    };

    struct EventPortRef {
        GraphBuilder* graph_builder {};
        size_t node_index {};
        size_t output_port {};

        EventPortRef() = default;
        explicit EventPortRef(GraphBuilder& graph_builder_, size_t node_index, size_t output_port);
        operator PortId() const { return { node_index, output_port }; }

        std::string to_string() const;
    };

    struct NamedRef {
        std::string_view name;
        std::variant<SamplePortRef, Sample, EventPortRef> value;

        NamedRef(std::string_view name, SamplePortRef sample_port): name(name), value(sample_port) {}
        NamedRef(std::string_view name, Sample sample): name(name), value(sample) {}
        NamedRef(std::string_view name, EventPortRef event): name(name), value(event) {}
    };

    struct OutputRefConfig {
        SamplePortRef ref;
        OutputConfig config;
    };

    struct EventOutputRefConfig {
        EventPortRef ref;
        EventOutputConfig config;
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

    template<fixed_string Name, class T>
    NamedRef make_named_ref(NamedArg<Name, T> const& arg)
    {
        return NamedRef(Name.view(), arg.value);
    }

    template<fixed_string Name>
    struct PortName {
        template<class T>
        constexpr auto operator=(T&& value) const
        {
            return NamedArg<Name, std::remove_cvref_t<T>>{ std::forward<T>(value) };
        }
    };

    struct BuilderNode {
        std::vector<InputConfig> input_configs;
        std::vector<OutputConfig> output_configs;
        std::vector<EventInputConfig> event_input_configs;
        std::vector<EventOutputConfig> event_output_configs;
        std::function<TypeErasedNode(size_t)> materialize {};
        std::optional<size_t> ttl_samples {};
        size_t lowered_subgraph_begin = 0;
        size_t lowered_subgraph_count = 0;
        std::vector<std::vector<PortId>> subgraph_input_targets {};
        std::vector<PortId> subgraph_output_sources {};
        std::vector<std::vector<PortId>> subgraph_event_input_targets {};
        std::vector<PortId> subgraph_event_output_sources {};
        std::vector<SourceSpan> source_spans {};

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
        using sample_node_ref_for_t = std::conditional_t<
            has_fixed_output_count_v<std::remove_cvref_t<Node>>,
            StructuredNodeRef<std::remove_cvref_t<Node>>,
            std::conditional_t<
                should_preserve_node_type_v<std::remove_cvref_t<Node>>,
                TypedNodeRef<std::remove_cvref_t<Node>>,
                SampleNodeRef
            >
        >;

        template<class T>
        struct is_named_arg : std::false_type {};

        template<fixed_string Name, class T>
        struct is_named_arg<NamedArg<Name, T>> : std::true_type {};

        template<class T>
        inline constexpr bool is_named_arg_v = is_named_arg<std::remove_cvref_t<T>>::value;

        template<class T>
        inline constexpr bool is_event_like_v = std::convertible_to<std::remove_cvref_t<T>, EventPortRef>;

        template<class T>
        inline constexpr bool arg_is_event_v = is_event_like_v<T>;

        template<fixed_string Name, class T>
        inline constexpr bool arg_is_event_v<NamedArg<Name, T>> = is_event_like_v<T>;

        enum class call_domain {
            sample,
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
        inline constexpr bool has_sample_args_v = (false || ... || !arg_is_event_v<std::remove_cvref_t<Args>>);

        template<class... Args>
        inline constexpr call_domain call_domain_v =
            has_event_args_v<Args...>
                ? (has_sample_args_v<Args...> ? call_domain::mixed : call_domain::event)
                : call_domain::sample;

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

        friend class EventNodeRef;
        friend class GraphBuilder;

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
        SampleNodeRef node_ref() const;

        SamplePortRef operator[](size_t output_index) const;
        SamplePortRef operator[](std::string_view output_name) const;
        operator SamplePortRef() const;
        bool input_is_connected(size_t input_port) const;
        bool event_input_is_connected(size_t input_port) const;

        template<class... Args>
        requires(details::node_call_enabled<Node, Args...>)
        Derived operator()(Args&&... args) const;

        template<class... Args>
        requires(!details::node_call_enabled<Node, Args...>)
        Derived operator()(Args&&... args) const = delete;

        template<class T>
        Derived connect_input(size_t input_port, T&& value) const;
        template<class T>
        Derived connect_input(std::string_view input_name, T&& value) const;
        Derived connect_event_input(size_t input_port, EventPortRef value) const;
        Derived connect_event_input(std::string_view input_name, EventPortRef value) const;

        SamplePortRef detach(size_t loop_extra_latency = 1) const;
        Derived ttl(size_t samples) const;
        Derived no_ttl() const;
        void _add_source_span(uint32_t begin, uint32_t end) const;

        std::string to_string() const;
    };

    class SampleNodeRef : public NodeRefBase<SampleNodeRef> {
    public:
        using NodeRefBase<SampleNodeRef>::NodeRefBase;
    };

    class EventNodeRef {
        SampleNodeRef _node;

        EventPortRef event_port_ref(size_t output_index) const
        {
            auto outputs = get_event_outputs(_node.node());
            if (output_index >= outputs.size()) {
                details::error(
                    "event output port " + std::to_string(output_index) + " of "
                    + _node.to_string() + " is out of bounds"
                );
            }
            return EventPortRef(*_node._graph_builder, _node._index, output_index);
        }

        EventPortRef event_port_ref(std::string_view output_name) const
        {
            auto outputs = get_event_outputs(_node.node());
            for (size_t output_port = 0; output_port < outputs.size(); ++output_port) {
                if (outputs[output_port].name == output_name) {
                    return EventPortRef(*_node._graph_builder, _node._index, output_port);
                }
            }

            details::error(
                "an event output port named '" + std::string(output_name) + "' "
                "does not exist on " + _node.to_string()
            );
        }

    public:
        EventNodeRef() = default;

        explicit EventNodeRef(SampleNodeRef node) :
            _node(std::move(node))
        {}

        EventPortRef operator[](size_t output_index) const
        {
            return event_port_ref(output_index);
        }

        EventPortRef operator[](std::string_view output_name) const
        {
            return event_port_ref(output_name);
        }

        operator EventPortRef() const
        {
            auto outputs = get_event_outputs(_node.node());
            if (outputs.size() != 1) {
                details::error(
                    to_string() + " "
                    "does not have exactly 1 event output port: it cannot be implicitly converted to EventPortRef"
                );
            }
            return event_port_ref(0);
        }

        std::string to_string() const
        {
            return _node.to_string();
        }

        SampleNodeRef const& node() const
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

        operator SampleNodeRef() const
        {
            if (!this->_graph_builder) {
                return SampleNodeRef();
            }
            return SampleNodeRef(*this->_graph_builder, this->_index);
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
        SamplePortRef get() const
        {
            static_assert(I < output_count);
            if (!this->_graph_builder) {
                details::error("attempted to use a null SampleNodeRef");
            }
            return SamplePortRef(*this->_graph_builder, this->_index, I);
        }
    };

    using DetachedSamplePortInfo = DetachedInfo;

    class GraphBuilder {
        template<class Derived, class Node>
        friend class NodeRefBase;
        friend struct SamplePortRef;
        friend struct EventPortRef;

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

        struct ScopedSubgraph {
            size_t start_node_index = 0;
            std::vector<OutputConfig> output_configs;
            std::vector<PortId> output_sources;
            std::vector<EventOutputConfig> event_output_configs;
            std::vector<PortId> event_output_sources;
            bool outputs_defined = false;
            bool event_outputs_defined = false;
        };

        std::vector<ScopedSubgraph> _scope_stack;

        std::unordered_map<PortId, DetachedSamplePortInfo> _detached_info_by_source;
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

        bool inside_subgraph_scope() const
        {
            return !_scope_stack.empty();
        }

        ScopedSubgraph& current_scope()
        {
            IV_ASSERT(!_scope_stack.empty(), "current_scope() requires an active subgraph scope");
            return _scope_stack.back();
        }

        void define_scope_outputs(std::span<OutputRefConfig const> refs)
        {
            auto& scope = current_scope();
            if (scope.outputs_defined) {
                details::error("subgraph outputs(...) was already called on builder " + _builder_id.value);
            }

            scope.output_configs.clear();
            scope.output_sources.clear();
            scope.output_configs.reserve(refs.size());
            scope.output_sources.reserve(refs.size());
            std::unordered_set<std::string> output_names;

            for (size_t i = 0; i < refs.size(); ++i) {
                auto const& ref = refs[i].ref;
                auto const& config = refs[i].config;
                if (ref.graph_builder != this) {
                    details::error(
                        "builder " + _builder_id.value + ": subgraph outputs(...): "
                        "SamplePortRef at index " + std::to_string(i) + " "
                        "belongs to builder " + ref.graph_builder->_builder_id.value
                    );
                }

                if (!config.name.empty()) {
                    if (output_names.contains(config.name)) {
                        details::error(
                            "builder " + _builder_id.value + ": duplicate subgraph output name '" + config.name + "'"
                        );
                    }
                    output_names.insert(config.name);
                }

                scope.output_sources.push_back(PortId{ ref.node_index, ref.output_port });
                scope.output_configs.push_back(config);
            }

            scope.outputs_defined = true;
        }

        void define_scope_event_outputs(std::span<EventOutputRefConfig const> refs)
        {
            auto& scope = current_scope();
            if (scope.event_outputs_defined) {
                details::error("subgraph event_outputs(...) was already called on builder " + _builder_id.value);
            }

            scope.event_output_configs.clear();
            scope.event_output_sources.clear();
            scope.event_output_configs.reserve(refs.size());
            scope.event_output_sources.reserve(refs.size());

            for (size_t i = 0; i < refs.size(); ++i) {
                auto const& ref = refs[i].ref;
                if (ref.graph_builder != this) {
                    details::error(
                        "builder " + _builder_id.value + ": subgraph event_outputs(...): "
                        "EventPortRef at index " + std::to_string(i) + " "
                        "belongs to another builder"
                    );
                }

                auto source_type = (ref.node_index == GRAPH_ID)
                    ? _public_event_inputs[ref.output_port].type
                    : _nodes[ref.node_index].event_output_configs[ref.output_port].type;
                scope.event_output_sources.push_back(PortId{ ref.node_index, ref.output_port });
                scope.event_output_configs.emplace_back(refs[i].config);
                scope.event_output_configs.back().type = source_type;
            }

            scope.event_outputs_defined = true;
        }

        size_t append_lowered_subgraph_placeholder(
            std::vector<InputConfig> input_configs,
            std::vector<OutputConfig> output_configs,
            std::vector<EventInputConfig> event_input_configs,
            std::vector<EventOutputConfig> event_output_configs,
            size_t lowered_subgraph_begin,
            size_t lowered_subgraph_count,
            std::vector<std::vector<PortId>> subgraph_input_targets,
            std::vector<PortId> subgraph_output_sources,
            std::vector<std::vector<PortId>> subgraph_event_input_targets,
            std::vector<PortId> subgraph_event_output_sources
        )
        {
            size_t const placeholder_node = _nodes.size();
            _nodes.emplace_back(BuilderNode{
                .input_configs = std::move(input_configs),
                .output_configs = std::move(output_configs),
                .event_input_configs = std::move(event_input_configs),
                .event_output_configs = std::move(event_output_configs),
                .materialize = {},
                .ttl_samples = std::nullopt,
                .lowered_subgraph_begin = lowered_subgraph_begin,
                .lowered_subgraph_count = lowered_subgraph_count,
                .subgraph_input_targets = std::move(subgraph_input_targets),
                .subgraph_output_sources = std::move(subgraph_output_sources),
                .subgraph_event_input_targets = std::move(subgraph_event_input_targets),
                .subgraph_event_output_sources = std::move(subgraph_event_output_sources),
            });
            return placeholder_node;
        }

        std::string node_id(size_t index) const
        {
            IV_ASSERT(index < _nodes.size(), "node index out of bounds");
            return _builder_id.child_id(index);
        }

        SamplePortRef input()
        {
            _public_inputs.emplace_back();
            return SamplePortRef(*this, GRAPH_ID, _public_inputs.size() - 1);
        }

        SamplePortRef input(std::string_view name, Sample default_value = 0.0)
        {
            if (name.empty()) {
                details::error("input name cannot be empty");
            }
            if (_input_name_to_index.contains(std::string(name))) {
                details::error("input '" + std::string(name) + "' already exists");
            }

            _public_inputs.emplace_back(InputConfig { .name = std::string(name), .default_value = default_value });
            _input_name_to_index.emplace(std::string(name), _public_inputs.size() - 1);
            return SamplePortRef(*this, GRAPH_ID, _public_inputs.size() - 1);
        }

        EventPortRef event_input(std::string_view name, EventTypeId type)
        {
            if (name.empty()) {
                details::error("event input name cannot be empty");
            }
            if (_event_input_name_to_index.contains(std::string(name))) {
                details::error("event input '" + std::string(name) + "' already exists");
            }

            _public_event_inputs.emplace_back(EventInputConfig { .name = std::string(name), .type = type });
            _event_input_name_to_index.emplace(std::string(name), _public_event_inputs.size() - 1);
            return EventPortRef(*this, GRAPH_ID, _public_event_inputs.size() - 1);
        }

        template<class Node, class... Args>
        details::sample_node_ref_for_t<Node> node(Args&&... args)
        {
            using StoredNode = std::remove_cvref_t<Node>;
            StoredNode node_value(std::forward<Args>(args)...);
            auto inputs = get_inputs(node_value);
            auto outputs = get_outputs(node_value);
            auto event_inputs = get_event_inputs(node_value);
            auto event_outputs = get_event_outputs(node_value);

            auto materialize = [node_value = std::move(node_value)]([[maybe_unused]] size_t detach_id_offset) {
                if constexpr (std::same_as<StoredNode, DetachWriterNode>) {
                    return TypeErasedNode(DetachWriterNode{
                        DetachArrayId(node_value.id.id + detach_id_offset),
                        node_value.loop_extra_latency
                    });
                } else if constexpr (std::same_as<StoredNode, DetachReaderNode>) {
                    return TypeErasedNode(DetachReaderNode{
                        DetachArrayId(node_value.id.id + detach_id_offset),
                        node_value.loop_extra_latency
                    });
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
                return SampleNodeRef(*this, _nodes.size() - 1);
            }
        }

        SampleNodeRef embed_subgraph(GraphBuilder const& child)
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

            append_lowered_subgraph_placeholder(
                child._public_inputs,
                child._public_outputs,
                child._public_event_inputs,
                child._public_event_outputs,
                child_node_offset,
                child._nodes.size(),
                std::vector<std::vector<PortId>>(child._public_inputs.size()),
                std::vector<PortId>(child._public_outputs.size()),
                std::vector<std::vector<PortId>>(child._public_event_inputs.size()),
                std::vector<PortId>(child._public_event_outputs.size())
            );

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
                    DetachedSamplePortInfo{
                        .detach_id = info.detach_id + child_detach_offset,
                        .original_source = remap_child_port(info.original_source),
                        .writer_node = child_node_offset + info.writer_node,
                        .reader_output = remap_child_port(info.reader_output),
                        .loop_extra_latency = info.loop_extra_latency,
                    }
                );
            }

            for (PortId const reader_output : child._detached_reader_outputs) {
                _detached_reader_outputs.insert(remap_child_port(reader_output));
            }

            return SampleNodeRef(*this, placeholder_node);
        }

        template<class... Refs>
        void event_outputs(Refs&&... refs)
        {
            std::vector<EventOutputRefConfig> output_refs;
            output_refs.reserve(sizeof...(Refs));
            auto const append_ref = [&](auto&& ref) {
                using RefT = std::remove_cvref_t<decltype(ref)>;
                if constexpr (details::is_named_arg_v<RefT>) {
                    output_refs.push_back(EventOutputRefConfig{
                        .ref = static_cast<EventPortRef>(ref.value),
                        .config = EventOutputConfig{ .name = std::string(RefT::name.view()) },
                    });
                } else {
                    output_refs.push_back(EventOutputRefConfig{
                        .ref = static_cast<EventPortRef>(std::forward<decltype(ref)>(ref)),
                        .config = {},
                    });
                }
            };
            (append_ref(std::forward<Refs>(refs)), ...);
            event_outputs(std::span<EventOutputRefConfig const>(output_refs.data(), output_refs.size()));
        }

        void event_outputs(std::span<EventOutputRefConfig const> refs)
        {
            if (inside_subgraph_scope()) {
                define_scope_event_outputs(refs);
                return;
            }

            _public_event_outputs.clear();
            _public_event_outputs.reserve(refs.size());

            for (size_t i = 0; i < refs.size(); ++i) {
                auto const& ref = refs[i].ref;
                if (ref.graph_builder != this) {
                    details::error(
                        "builder " + _builder_id.value + ": event_outputs(...): "
                        "EventPortRef at index " + std::to_string(i) + " "
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
                _public_event_outputs.emplace_back(refs[i].config);
                _public_event_outputs.back().type = source_type;
            }
        }

        template<class Fn>
        SampleNodeRef subgraph(Fn&& fn)
        {
            static_assert(
                std::invocable<Fn&> && !std::invocable<Fn&, GraphBuilder&>,
                "iv::GraphBuilder::subgraph(Fn) requires a zero-argument callback; use embed_subgraph(GraphBuilder) for isolated nested builders"
            );

            _scope_stack.push_back(ScopedSubgraph{
                .start_node_index = _nodes.size(),
                .output_configs = {},
                .output_sources = {},
                .event_output_configs = {},
                .event_output_sources = {},
                .outputs_defined = false,
                .event_outputs_defined = false,
            });

            struct ScopePopGuard {
                GraphBuilder* builder;
                bool active = true;

                ~ScopePopGuard()
                {
                    if (active) {
                        builder->_scope_stack.pop_back();
                    }
                }
            } guard { this };

            std::forward<Fn>(fn)();

            ScopedSubgraph scope = std::move(_scope_stack.back());
            guard.active = false;
            _scope_stack.pop_back();

            size_t const placeholder_node = append_lowered_subgraph_placeholder(
                {},
                std::move(scope.output_configs),
                {},
                std::move(scope.event_output_configs),
                scope.start_node_index,
                _nodes.size() - scope.start_node_index,
                {},
                std::move(scope.output_sources),
                {},
                std::move(scope.event_output_sources)
            );

            return SampleNodeRef(*this, placeholder_node);
        }

        template<class... Refs>
        requires (details::valid_node_call_args_v<Refs...> && !details::has_event_args_v<Refs...>)
        void outputs(Refs&&... refs)
        {
            if (_outputs_defined) {
                details::error("outputs(...) was already called on builder " + _builder_id.value);
            }

            std::vector<OutputRefConfig> output_refs;
            output_refs.reserve(sizeof...(Refs));
            auto const append_ref = [&](auto&& ref) {
                using RefT = std::remove_cvref_t<decltype(ref)>;
                if constexpr (details::is_named_arg_v<RefT>) {
                    output_refs.push_back(OutputRefConfig{
                        .ref = lift_to_sample_port(ref.value),
                        .config = OutputConfig{ .name = std::string(RefT::name.view()) },
                    });
                } else {
                    output_refs.push_back(OutputRefConfig{
                        .ref = lift_to_sample_port(std::forward<decltype(ref)>(ref)),
                        .config = {},
                    });
                }
            };
            (append_ref(std::forward<Refs>(refs)), ...);
            outputs(std::span<OutputRefConfig const>(output_refs.data(), output_refs.size()));
        }

        void outputs(std::initializer_list<NamedRef> refs)
        {
            if (_outputs_defined) {
                details::error("outputs(...) was already called on builder " + _builder_id.value);
            }
            std::vector<OutputRefConfig> output_refs;
            output_refs.reserve(refs.size());
            for (auto const& ref : refs) {
                output_refs.push_back(OutputRefConfig{
                    .ref = lift_to_sample_port(ref),
                    .config = OutputConfig{ .name = std::string(ref.name) },
                });
            }
            outputs(std::span<OutputRefConfig const>(output_refs.data(), output_refs.size()));
        }

        void outputs(std::span<OutputRefConfig const> refs)
        {
            if (inside_subgraph_scope()) {
                define_scope_outputs(refs);
                return;
            }

            if (_outputs_defined) {
                details::error("outputs(...) was already called on builder " + _builder_id.value);
            }

            _public_outputs.clear();
            _public_outputs.reserve(refs.size());
            std::unordered_set<std::string> output_names;

            for (size_t i = 0; i < refs.size(); ++i) {
                auto const& ref = refs[i].ref;
                auto const& config = refs[i].config;
                if (ref.graph_builder != this) {
                    details::error(
                        "builder " + _builder_id.value + ": outputs(...): "
                        "SamplePortRef at index " + std::to_string(i) + " "
                        "belongs to builder " + ref.graph_builder->_builder_id.value
                    );
                }

                if (!config.name.empty()) {
                    if (output_names.contains(config.name)) {
                        details::error(
                            "builder " + _builder_id.value + ": duplicate output name '" + config.name + "'"
                        );
                    }
                    output_names.insert(config.name);
                }

                _edges.emplace(GraphEdge{
                    PortId{ ref.node_index, ref.output_port },
                    PortId{ GRAPH_ID, i },
                });
                _public_outputs.push_back(config);
            }

            _outputs_defined = true;
        }

        void outputs(std::span<NamedRef const> refs)
        {
            if (_outputs_defined) {
                details::error("outputs(...) was already called on builder " + _builder_id.value);
            }
            std::vector<OutputRefConfig> output_refs;
            output_refs.reserve(refs.size());
            for (auto const& ref : refs) {
                if (ref.name.empty()) {
                    details::error(
                        "builder " + _builder_id.value + ": named outputs must not use empty names"
                    );
                }
                output_refs.push_back(OutputRefConfig{
                    .ref = lift_to_sample_port(ref),
                    .config = OutputConfig{ .name = std::string(ref.name) },
                });
            }
            outputs(std::span<OutputRefConfig const>(output_refs.data(), output_refs.size()));
        }

        Graph build(size_t detach_id_offset = 0) const
        {
            if (!_outputs_defined) {
                details::error("builder " + _builder_id.value + ": g.outputs(...) must be called before build()");
            }

            details::PreparedGraph g{
                .nodes = {},
                .explicit_ttl_samples = {},
                .node_ids = {},
                .node_source_spans = {},
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
                g.explicit_ttl_samples.push_back(_nodes[node_i].ttl_samples);
                g.node_ids.push_back(node_id(node_i));
                g.node_source_spans.push_back(_nodes[node_i].source_spans);
            }

            auto materialize_placeholder_default = [&](size_t placeholder_node, size_t input_port) {
                runtime_node_indices.push_back(g.nodes.size());
                g.nodes.push_back(TypeErasedNode(Constant(_nodes[placeholder_node].input_configs[input_port].default_value)));
                g.explicit_ttl_samples.push_back(std::nullopt);
                g.node_ids.push_back(
                    node_id(placeholder_node) + ".default." + std::to_string(input_port)
                );
                g.node_source_spans.emplace_back();
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
                g.detached_info_by_source.emplace(remapped_source, DetachedSamplePortInfo{
                    .detach_id = info.detach_id,
                    .original_source = remapped_source,
                    .writer_node = runtime_node_indices[info.writer_node],
                    .reader_output = resolve_source(resolve_source, info.reader_output),
                    .loop_extra_latency = info.loop_extra_latency,
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

            auto lowered_scopes = [&] {
                std::vector<size_t> placeholder_indices;
                for (size_t node_i = 0; node_i < _nodes.size(); ++node_i) {
                    if (!_nodes[node_i].materialize) {
                        placeholder_indices.push_back(node_i);
                    }
                }

                auto encloses = [&](size_t parent, size_t child) {
                    auto const& parent_node = _nodes[parent];
                    auto const& child_node = _nodes[child];
                    size_t const parent_begin = parent_node.lowered_subgraph_begin;
                    size_t const parent_end = parent_begin + parent_node.lowered_subgraph_count;
                    size_t const child_begin = child_node.lowered_subgraph_begin;
                    size_t const child_end = child_begin + child_node.lowered_subgraph_count;
                    return parent_begin <= child_begin && child_end <= parent_end;
                };

                std::unordered_map<size_t, size_t> scope_index_by_placeholder;
                std::vector<iv::LoweredScopeSpec> scopes;

                for (size_t placeholder_i : placeholder_indices) {
                    auto const& placeholder = _nodes[placeholder_i];
                    iv::LoweredScopeSpec scope;
                    scope.ttl_samples = placeholder.ttl_samples;

                    size_t const begin = placeholder.lowered_subgraph_begin;
                    size_t const end = begin + placeholder.lowered_subgraph_count;
                    for (size_t node_i = begin; node_i < end; ++node_i) {
                        if (!_nodes[node_i].materialize) {
                            continue;
                        }
                        scope.member_node_ids.push_back(node_id(node_i));
                    }

                    if (scope.member_node_ids.empty()) {
                        continue;
                    }

                    std::sort(scope.member_node_ids.begin(), scope.member_node_ids.end());
                    scope.member_node_ids.erase(std::unique(scope.member_node_ids.begin(), scope.member_node_ids.end()), scope.member_node_ids.end());
                    scope_index_by_placeholder.emplace(placeholder_i, scopes.size());
                    scopes.push_back(std::move(scope));
                }

                for (size_t placeholder_i : placeholder_indices) {
                    auto scope_it = scope_index_by_placeholder.find(placeholder_i);
                    if (scope_it == scope_index_by_placeholder.end()) {
                        continue;
                    }
                    size_t const scope_i = scope_it->second;

                    size_t best_parent_placeholder = GRAPH_ID;
                    size_t best_parent_span = std::numeric_limits<size_t>::max();
                    for (size_t other_placeholder : placeholder_indices) {
                        if (other_placeholder == placeholder_i || !encloses(other_placeholder, placeholder_i)) {
                            continue;
                        }
                        size_t const span = _nodes[other_placeholder].lowered_subgraph_count;
                        if (span < best_parent_span && scope_index_by_placeholder.contains(other_placeholder)) {
                            best_parent_span = span;
                            best_parent_placeholder = other_placeholder;
                        }
                    }
                    if (best_parent_placeholder != GRAPH_ID) {
                        scopes[scope_i].parent_scope = scope_index_by_placeholder.at(best_parent_placeholder);
                    }
                }

                return scopes;
            }();

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
                std::move(g.explicit_ttl_samples),
                std::move(g.node_ids),
                std::move(g.node_source_spans),
                std::move(g.edges),
                std::move(g.event_edges),
                std::move(detached),
                std::move(execution_plan),
                _public_inputs,
                _public_outputs,
                _public_event_inputs,
                _public_event_outputs,
                details::compile_dormancy_groups(g, lowered_scopes, _builder_id.value, execution_plan)
            ));
        }

    private:
        std::optional<SampleNodeRef> _source_node_for_source_span(SamplePortRef ref)
        {
            if (!ref.graph_builder) {
                return std::nullopt;
            }

            if (ref.graph_builder != this) {
                details::error(
                    "builder " + _builder_id.value + ": cannot resolve source span origin for " + ref.to_string() +
                    " because it belongs to another builder"
                );
            }

            PortId source = ref;
            for (auto const& [_, info] : _detached_info_by_source) {
                if (info.reader_output == source) {
                    source = info.original_source;
                    break;
                }
            }

            if (source.node == GRAPH_ID) {
                return std::nullopt;
            }

            return SampleNodeRef(*this, source.node);
        }

        void _add_node_source_span(SampleNodeRef ref, uint32_t begin, uint32_t end)
        {
            if (!ref._graph_builder) {
                return;
            }

            if (ref._graph_builder != this) {
                details::error(
                    "builder " + _builder_id.value + ": cannot record source span for " + ref.to_string() +
                    " because it belongs to another builder"
                );
            }

            auto& spans = _nodes[ref._index].source_spans;
            SourceSpan span {
                .begin = begin,
                .end = end,
            };
            if (std::find(spans.begin(), spans.end(), span) == spans.end()) {
                spans.push_back(span);
            }
        }

        static std::string allocate_root_builder_id()
        {
            static size_t next_root_builder_id = 0;
            return std::to_string(next_root_builder_id++);
        }

        SamplePortRef detach_sample_port(SamplePortRef sample_port, size_t loop_extra_latency)
        {
            if (!sample_port.graph_builder) {
                details::error(
                    "builder " + _builder_id.value + ": cannot detach an empty sample port"
                );
            }

            if (sample_port.graph_builder != this) {
                details::error(
                    "builder " + _builder_id.value + ": cannot detach " + sample_port.to_string() +
                    " because it belongs to another builder"
                );
            }

            PortId const source = sample_port;

            // Already detached: return unchanged.
            if (_detached_reader_outputs.contains(source)) {
                return sample_port;
            }

            // Reuse existing detached bridge for the same source.
            if (auto it = _detached_info_by_source.find(source); it != _detached_info_by_source.end()) {
                if (it->second.loop_extra_latency != loop_extra_latency) {
                    details::error(
                        "builder " + _builder_id.value + ": detach loop extra latency conflict on " + sample_port.to_string()
                    );
                }
                PortId const reader = it->second.reader_output;
                return SamplePortRef(*this, reader.node, reader.port);
            }

            if (loop_extra_latency < 1) {
                details::error("builder " + _builder_id.value + ": detach loop extra latency must be at least 1");
            }

            size_t const detach_id = _next_detach_id++;

            auto writer = node<DetachWriterNode>(detach_id, loop_extra_latency);
            size_t const writer_node = _nodes.size() - 1;
            writer(sample_port);

            auto reader = node<DetachReaderNode>(detach_id, loop_extra_latency);
            SamplePortRef detached = reader;

            _detached_info_by_source.emplace(source, DetachedSamplePortInfo{
                .detach_id = detach_id,
                .original_source = source,
                .writer_node = writer_node,
                .reader_output = detached,
                .loop_extra_latency = loop_extra_latency,
            });
            _detached_reader_outputs.insert(detached);

            return detached;
        }

        SamplePortRef lift_to_sample_port(SamplePortRef sample_port)
        {
            if (sample_port.graph_builder != this) {
                details::error(
                    "builder " + _builder_id.value + ": sample port " + sample_port.to_string() + " belongs to another builder"
                );
            }
            return sample_port;
        }

        template<class T>
        requires std::is_arithmetic_v<std::remove_cvref_t<T>> || std::is_same_v<std::remove_cvref_t<T>, Sample>
        SamplePortRef lift_to_sample_port(T value)
        {
            return node<Constant>(static_cast<Sample>(value));
        }

        SamplePortRef lift_to_sample_port(NamedRef const& ref)
        {
            return std::visit([&](auto const& v) -> SamplePortRef {
                using T = std::remove_cvref_t<decltype(v)>;
                if constexpr (std::same_as<T, EventPortRef>) {
                    details::error(
                        "builder " + _builder_id.value + ": expected sample/sample-port value, got EventPortRef"
                    );
                } else {
                    return lift_to_sample_port(v);
                }
            }, ref.value);
        }
    };

    inline SamplePortRef::SamplePortRef(GraphBuilder& graph_builder_, size_t node_index, size_t output_port) :
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

    inline EventPortRef::EventPortRef(GraphBuilder& graph_builder_, size_t node_index, size_t output_port) :
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

    inline SamplePortRef SamplePortRef::detach(size_t loop_extra_latency) const
    {
        if (!graph_builder) {
            details::error("attempted to detach an empty sample port");
        }
        return graph_builder->detach_sample_port(*this, loop_extra_latency);
    }

    inline void SamplePortRef::_add_source_span(uint32_t begin, uint32_t end) const
    {
        if (!graph_builder) {
            return;
        }

        if (auto source_node = graph_builder->_source_node_for_source_span(*this); source_node.has_value()) {
            graph_builder->_add_node_source_span(*source_node, begin, end);
        }
    }

    inline std::string SamplePortRef::to_string() const
    {
        if (!graph_builder) {
            return "empty sample port";
        }
        if (node_index == GRAPH_ID) {
            return "graph input " + std::to_string(output_port) + " in builder " + graph_builder->_builder_id.value;
        }
        return "sample port at address " + graph_builder->node_id(node_index) + ":" + std::to_string(output_port);
    }

    inline std::string EventPortRef::to_string() const
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
            details::error("attempted to use a null SampleNodeRef");
        }
        return _graph_builder->_nodes[_index];
    }

    template<class Derived, class Node>
    inline SampleNodeRef NodeRefBase<Derived, Node>::node_ref() const
    {
        if (!_graph_builder) {
            details::error("attempted to use a null SampleNodeRef");
        }
        return SampleNodeRef(*_graph_builder, _index);
    }

    template<class Derived, class Node>
    inline void NodeRefBase<Derived, Node>::_add_source_span(uint32_t begin, uint32_t end) const
    {
        if (!_graph_builder) {
            return;
        }
        _graph_builder->_add_node_source_span(SampleNodeRef(*_graph_builder, _index), begin, end);
    }

    template<class Derived, class Node>
    inline SamplePortRef NodeRefBase<Derived, Node>::operator[](size_t output_index) const
    {
        if (!_graph_builder) {
            details::error("attempted to use a null SampleNodeRef");
        }
        return SamplePortRef(*_graph_builder, _index, output_index);
    }

    template<class Derived, class Node>
    inline SamplePortRef NodeRefBase<Derived, Node>::operator[](std::string_view output_name) const
    {
        if (!_graph_builder) {
            details::error("attempted to use a null SampleNodeRef");
        }
        auto outputs = get_outputs(node());
        for (size_t output_port = 0; output_port < outputs.size(); ++output_port) {
            if (outputs[output_port].name == output_name) {
                return SamplePortRef(*_graph_builder, _index, output_port);
            }
        }

        details::error(
            "an output port named '" + std::string(output_name) + "' "
            "does not exist on " + to_string()
        );
    }

    template<class Derived, class Node>
    inline NodeRefBase<Derived, Node>::operator SamplePortRef() const
    {
        if (!_graph_builder) {
            details::error("attempted to use a null SampleNodeRef");
        }
        if (get_num_outputs(node()) != 1) {
            details::error(
                to_string() + " "
                "does not have exactly 1 output port: it cannot be implicitly converted to SamplePortRef"
            );
        }
        return SamplePortRef(*_graph_builder, _index, 0);
    }

    template<class Derived, class Node>
    inline bool NodeRefBase<Derived, Node>::input_is_connected(size_t input_port) const
    {
        if (!_graph_builder) {
            details::error("attempted to use a null SampleNodeRef");
        }
        return _graph_builder->_placed_input_ports.contains(PortId{ _index, input_port });
    }

    template<class Derived, class Node>
    inline bool NodeRefBase<Derived, Node>::event_input_is_connected(size_t input_port) const
    {
        if (!_graph_builder) {
            details::error("attempted to use a null SampleNodeRef");
        }
        return _graph_builder->_placed_event_input_ports.contains(PortId{ _index, input_port });
    }

    template<class Derived, class Node>
    inline SamplePortRef NodeRefBase<Derived, Node>::detach(size_t loop_extra_latency) const
    {
        return static_cast<SamplePortRef>(*this).detach(loop_extra_latency);
    }

    template<class Derived, class Node>
    inline Derived NodeRefBase<Derived, Node>::ttl(size_t ttl_samples) const
    {
        if (!_graph_builder) {
            details::error("attempted to use a null SampleNodeRef");
        }
        auto& builder_node = _graph_builder->_nodes[_index];
        if (!builder_node.materialize) {
            builder_node.ttl_samples = ttl_samples;
            size_t const begin = builder_node.lowered_subgraph_begin;
            size_t const end = begin + builder_node.lowered_subgraph_count;
            for (size_t node_i = begin; node_i < end; ++node_i) {
                _graph_builder->_nodes[node_i].ttl_samples = ttl_samples;
            }
        } else {
            builder_node.ttl_samples = ttl_samples;
        }
        return derived();
    }

    template<class Derived, class Node>
    inline Derived NodeRefBase<Derived, Node>::no_ttl() const
    {
        return ttl(std::numeric_limits<size_t>::max());
    }

    template<class Derived, class Node>
    inline std::string NodeRefBase<Derived, Node>::to_string() const
    {
        if (!_graph_builder) {
            return "empty node";
        }
        return "node at address " + _graph_builder->node_id(_index);
    }

}
