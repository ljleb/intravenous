#pragma once

#include "stored_node.h"
#include "node_call.h"
#include "output_refs.h"
#include "../compiler.h"

#include <concepts>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace iv {
    class GraphBuilder;
    class GraphBuilderAnnotations;
    class NodeRef;
    template<class Node>
    class TypedNodeRef;
    template<class Node>
    class StructuredNodeRef;

    struct LogicalEmptyTag {
        explicit constexpr LogicalEmptyTag() = default;
    };

    inline constexpr LogicalEmptyTag logical_empty_tag {};

    template<class Derived, class Node = void>
    class NodeRefBase {
    protected:
        GraphBuilder* _graph_builder{};
        size_t _index{};
        mutable std::string _logical_declaration_id {};
        bool _allows_single_assignment = false;

        friend class GraphBuilder;
        friend class GraphBuilderAnnotations;

    private:
        Derived& derived()
        {
            return static_cast<Derived&>(*this);
        }

        Derived const& derived() const
        {
            return static_cast<Derived const&>(*this);
        }

    public:
        NodeRefBase() = default;
        NodeRefBase(NodeRefBase const& rhs) :
            _graph_builder(rhs._graph_builder),
            _index(rhs._index),
            _logical_declaration_id(rhs._allows_single_assignment ? rhs._logical_declaration_id : std::string{}),
            _allows_single_assignment(rhs._allows_single_assignment)
        {}
        NodeRefBase(NodeRefBase&& rhs) noexcept :
            _graph_builder(rhs._graph_builder),
            _index(rhs._index),
            _logical_declaration_id(std::move(rhs._logical_declaration_id)),
            _allows_single_assignment(rhs._allows_single_assignment)
        {
            rhs._graph_builder = nullptr;
            rhs._index = 0;
            rhs._allows_single_assignment = false;
        }
        explicit NodeRefBase(LogicalEmptyTag, std::string_view declaration_identity) :
            _logical_declaration_id(declaration_identity),
            _allows_single_assignment(true)
        {}
        explicit NodeRefBase(GraphBuilder& graph_builder, size_t index) :
            _graph_builder(&graph_builder),
            _index(index)
        {}

        Derived& operator=(Derived const& rhs);
        Derived& operator=(Derived&& rhs);

        BuilderNode const& node() const;
        NodeRef node_ref() const;
        Derived _clone_handle() const;

        SamplePortRef operator[](size_t output_index) const;
        SamplePortRef operator[](std::string_view output_name) const;
        EventPortRef event_port(size_t output_index) const;
        EventPortRef event_port(std::string_view output_name) const;
        EventPortRef event_port() const;
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
        void _annotate_source_info(
            std::string_view declaration_identity,
            std::string_view file_path,
            uint32_t begin,
            uint32_t end
        ) const;

        std::string to_string() const;
    };

    class NodeRef : public NodeRefBase<NodeRef> {
    public:
        using NodeRefBase<NodeRef>::NodeRefBase;
        using NodeRefBase<NodeRef>::operator=;

        NodeRef(NodeRef const&) = delete;
        NodeRef(NodeRef&&) noexcept = default;

        NodeRef& operator=(NodeRef const& rhs) = delete;

        NodeRef& operator=(NodeRef&& rhs)
        {
            return NodeRefBase<NodeRef>::operator=(std::move(rhs));
        }
    };

    template<class Node>
    class TypedNodeRef : public NodeRefBase<TypedNodeRef<Node>, std::remove_cvref_t<Node>> {
        using Base = NodeRefBase<TypedNodeRef<Node>, std::remove_cvref_t<Node>>;

    public:
        using NodeType = std::remove_cvref_t<Node>;
        using Base::Base;
        using Base::operator=;

        TypedNodeRef(TypedNodeRef const&) = delete;
        TypedNodeRef(TypedNodeRef&&) noexcept = default;

        TypedNodeRef& operator=(TypedNodeRef const& rhs) = delete;

        TypedNodeRef& operator=(TypedNodeRef&& rhs)
        {
            return Base::operator=(std::move(rhs));
        }

        TypedNodeRef _clone_handle() const
        {
            if (!this->_graph_builder) {
                return TypedNodeRef {};
            }
            return TypedNodeRef(*this->_graph_builder, this->_index);
        }

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
        using Base::operator=;

        StructuredNodeRef(StructuredNodeRef const&) = delete;
        StructuredNodeRef(StructuredNodeRef&&) noexcept = default;

        StructuredNodeRef& operator=(StructuredNodeRef const& rhs) = delete;

        StructuredNodeRef& operator=(StructuredNodeRef&& rhs)
        {
            return Base::operator=(std::move(rhs));
        }

        StructuredNodeRef() = default;
        explicit StructuredNodeRef(GraphBuilder& graph_builder, size_t index) :
            Base(graph_builder, index)
        {}

        StructuredNodeRef _clone_handle() const
        {
            if (!this->_graph_builder) {
                return StructuredNodeRef {};
            }
            return StructuredNodeRef(*this->_graph_builder, this->_index);
        }

        template<size_t I>
        SamplePortRef get() const
        {
            static_assert(I < output_count);
            if (!this->_graph_builder) {
                details::error("attempted to use a null NodeRef");
            }
            return SamplePortRef(*this->_graph_builder, this->_index, I);
        }
    };

    namespace details {
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
    }

    template<fixed_string Name, NamedPortKind Kind>
    template<class T>
    constexpr auto PortName<Name, Kind>::operator=(T&& value) const
    {
        using Value = std::remove_cvref_t<T>;
        if constexpr (std::same_as<Value, SamplePortRef>) {
            return NamedArg<Name, SamplePortRef, Kind>{ static_cast<SamplePortRef>(std::forward<T>(value)) };
        } else if constexpr (std::same_as<Value, EventPortRef>) {
            if (!value.graph_builder) {
                return NamedArg<Name, EventPortRef, Kind>{ EventPortRef{} };
            }
            return NamedArg<Name, EventPortRef, Kind>{
                EventPortRef(*value.graph_builder, value.node_index, value.output_port)
            };
        } else if constexpr (requires(Value const& ref) { ref.node_ref(); }) {
            return NamedArg<Name, NodeRef, Kind>{ value.node_ref() };
        } else {
            return NamedArg<Name, Value, Kind>{ std::forward<T>(value) };
        }
    }
}
