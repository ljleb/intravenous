#pragma once

#include <intravenous/graph/builder/stored_node.hpp>
#include <intravenous/graph/builder/node_call.h>
#include <intravenous/graph/builder/output_refs.h>
#include <intravenous/graph/error.h>
#include <intravenous/node/static_port_access.h>

#include <concepts>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace iv {
    class GraphBuilder;
    class GraphBuilderAnnotations;
    class NodeRef;
    struct ConcretePortProjection {};
    template<class ChannelType>
    struct TiledPortProjection { using channel_type = ChannelType; };
    template<class Node, class PortProjection = ConcretePortProjection>
    class TypedNodeRef;
    template<class Node, class ChannelType>
    using TiledNodeRef = TypedNodeRef<Node, TiledPortProjection<ChannelType>>;

    // The untyped handle is the public base for every node-bundle case.
    // It deliberately addresses a NodeBundle, never an assumed concrete node.
    class NodeRef {
    protected:
        GraphBuilder* _graph_builder{};
        size_t _index{};
        mutable std::string _virtual_declaration_id {};

        friend class GraphBuilder;
        friend class GraphBuilderAnnotations;

    public:
        constexpr NodeRef() = default;
        NodeRef(NodeRef const&) = delete;
        NodeRef(NodeRef&&) noexcept = default;
        constexpr explicit NodeRef(GraphBuilder& graph_builder, size_t index) :
            _graph_builder(&graph_builder),
            _index(index)
        {}

        constexpr NodeRef& operator=(NodeRef const& rhs);
        constexpr NodeRef& operator=(NodeRef&& rhs);

        constexpr NodeRef node_ref() const;
        constexpr size_t node_bundle_handle() const
        {
            if (!_graph_builder) {
                details::error("attempted to use a null NodeRef");
            }
            return _index;
        }
        constexpr NodeRef _clone_handle() const;
        constexpr SamplePortRef operator[](size_t output_port) const;
        constexpr SamplePortRef operator[](std::string_view output_name) const;
        template<fixed_string Name, NamedPortKind Kind>
        constexpr auto operator[](PortName<Name, Kind>) const
        {
            if constexpr (Kind == NamedPortKind::sample) {
                return (*this)[Name.view()];
            } else {
                return event_port(Name.view());
            }
        }
        constexpr operator SamplePortRef() const;
        constexpr size_t sample_input_count() const;
        constexpr size_t sample_output_count() const;
        constexpr size_t event_input_count() const;
        constexpr size_t event_output_count() const;
        constexpr bool input_is_connected(size_t input_port) const;
        constexpr bool event_input_is_connected(size_t input_port) const;
        template<class T>
        consteval NodeRef connect_input(size_t input_port, T&& value) const;
        template<class T>
        consteval NodeRef connect_input(std::string_view input_name, T&& value) const;
        template<class... Args>
        consteval NodeRef operator()(Args&&... args) const;
        constexpr NodeRef connect_event_input(size_t input_port, EventPortRef value) const;
        constexpr NodeRef connect_event_input(std::string_view input_name, EventPortRef value) const;
        constexpr EventPortRef event_port(size_t output_port) const;
        constexpr EventPortRef event_port(std::string_view output_name) const;
        constexpr EventPortRef event_port() const;
        constexpr NodeRef ttl(size_t samples) const;
        constexpr NodeRef no_ttl() const;
        std::string to_string() const;
        constexpr void _annotate_source_info(
            std::string_view declaration_identity,
            std::string_view file_path,
            uint32_t begin,
            uint32_t end
        ) const;
    };

    // Internal only: it supplies covariant fluent returns for the two typed
    // public subclasses.  It is not part of the DSL-facing hierarchy.
    template<class Derived>
    class NodeRefCrtp : public NodeRef {
        using Base = NodeRef;

    private:
        constexpr Derived& derived() { return static_cast<Derived&>(*this); }

    public:
        using Base::Base;

        constexpr Derived& operator=(Derived const& rhs)
        {
            Base::operator=(static_cast<NodeRef const&>(rhs));
            return derived();
        }

        constexpr Derived& operator=(Derived&& rhs)
        {
            Base::operator=(static_cast<NodeRef&&>(rhs));
            return derived();
        }

        constexpr Derived _clone_handle() const
        {
            if (!this->_graph_builder) {
                return Derived{};
            }
            return Derived(*this->_graph_builder, this->_index);
        }

        constexpr Derived ttl(size_t samples) const
        {
            Base::ttl(samples);
            return _clone_handle();
        }

        template<class T>
        consteval Derived connect_input(size_t input_port, T&& value) const
        {
            Base::connect_input(input_port, std::forward<T>(value));
            return _clone_handle();
        }

        template<class T>
        consteval Derived connect_input(std::string_view input_name, T&& value) const
        {
            Base::connect_input(input_name, std::forward<T>(value));
            return _clone_handle();
        }

        constexpr Derived connect_event_input(size_t input_port, EventPortRef value) const
        {
            Base::connect_event_input(input_port, std::move(value));
            return _clone_handle();
        }

        constexpr Derived no_ttl() const
        {
            Base::no_ttl();
            return _clone_handle();
        }
    };

    // A typed ref is the concrete-bundle specialization.  Concrete-only
    // operations live here; common state and conversion to NodeRef do not.
    template<class Node, class PortProjection>
    class TypedNodeRef : public NodeRefCrtp<TypedNodeRef<Node, PortProjection>> {
        static_assert(std::same_as<PortProjection, ConcretePortProjection>,
                      "TiledPortProjection requires its tiled TypedNodeRef specialization");
        using Base = NodeRefCrtp<TypedNodeRef<Node, PortProjection>>;
    public:
        using NodeType = std::remove_cvref_t<Node>;
        using Base::Base;
        using Base::operator=;
        using Base::operator[];
        using Base::event_port;
        using Base::_graph_builder;
        using Base::_index;
        using Base::to_string;

        TypedNodeRef(TypedNodeRef const&) = delete;
        TypedNodeRef(TypedNodeRef&&) noexcept = default;
        TypedNodeRef& operator=(TypedNodeRef const&) = delete;
        constexpr TypedNodeRef& operator=(TypedNodeRef&& rhs)
        {
            Base::operator=(std::move(rhs));
            return *this;
        }
        constexpr NodePorts const& ports() const;

        constexpr SamplePortRef operator[](size_t output_index) const;
        constexpr SamplePortRef operator[](std::string_view output_name) const;

        template<fixed_string Name, NamedPortKind Kind>
        constexpr auto operator[](PortName<Name, Kind>) const
        {
            if constexpr (Kind == NamedPortKind::sample) {
                constexpr auto output_index = details::static_output_port_index<NodeType, Name>();
                constexpr auto layout = details::static_output_port_layout<NodeType, Name>();
                using ChannelType = typename RuntimeChannelTypeTraits<layout.channel_type>::type;
                if (!this->_graph_builder) {
                    details::error("attempted to use a null NodeRef");
                }
                return TypedSamplePortRef<ChannelType>{
                    this->operator[](output_index)};
            } else {
                return event_port(Name.view());
            }
        }
        constexpr EventPortRef event_port(size_t output_index) const;
        constexpr EventPortRef event_port(std::string_view output_name) const;
        constexpr EventPortRef event_port() const;
        constexpr operator SamplePortRef() const;
        template<class... Args>
        requires(details::node_call_enabled<NodeType, Args...>)
        consteval TypedNodeRef operator()(Args&&... args) const;

        template<class... Args>
        requires(!details::node_call_enabled<NodeType, Args...>)
        TypedNodeRef operator()(Args&&... args) const = delete;

        template<class T>
        consteval TypedNodeRef connect_input(size_t input_port, T&& value) const;
        template<class T>
        consteval TypedNodeRef connect_input(std::string_view input_name, T&& value) const;
        constexpr TypedNodeRef connect_event_input(size_t input_port, EventPortRef value) const;
        constexpr TypedNodeRef connect_event_input(std::string_view input_name, EventPortRef value) const;

        consteval SamplePortRef detach(size_t loop_extra_latency = 1) const;

        template<size_t I>
        constexpr auto static_output() const
        {
            constexpr auto layout = details::static_output_port_layout_at<NodeType, I>();
            using ChannelType = typename RuntimeChannelTypeTraits<layout.channel_type>::type;
            return TypedSamplePortRef<ChannelType>{
                this->operator[](I)};
        }

        // Every typed node has constexpr output configurations. `get` is the
        // tuple spelling for selecting one of them.
        template<size_t I>
        constexpr auto get() const
        {
            static_assert(I < details::static_output_count_v<NodeType>);
            return static_output<I>();
        }

    };

    template<class Node, class ChannelType>
    class TypedNodeRef<Node, TiledPortProjection<ChannelType>>
        : public NodeRefCrtp<TypedNodeRef<Node, TiledPortProjection<ChannelType>>> {
        using Self = TypedNodeRef<Node, TiledPortProjection<ChannelType>>;
        using Base = NodeRefCrtp<Self>;

    public:
        using NodeType = std::remove_cvref_t<Node>;
        using requested_channel_type = ChannelType;
        using Base::Base;
        using Base::operator=;
        using Base::operator[];
        using Base::event_port;
        using Base::_graph_builder;
        using Base::_index;
        using Base::to_string;

        constexpr TypedNodeRef() = default;
        constexpr explicit TypedNodeRef(GraphBuilder& graph_builder, NodeBundleHandle handle) :
            Base(graph_builder, handle) {}

        TypedNodeRef(TypedNodeRef const&) = delete;
        TypedNodeRef(TypedNodeRef&&) noexcept = default;

        TypedNodeRef& operator=(TypedNodeRef const&) = delete;
        constexpr TypedNodeRef& operator=(TypedNodeRef&& rhs)
        {
            Base::operator=(std::move(rhs));
            return *this;
        }

        constexpr Self _clone_handle() const
        {
            if (!this->_graph_builder) {
                return Self {};
            }
            return Self(*this->_graph_builder, this->_index);
        }

        template<fixed_string Name, NamedPortKind Kind>
        constexpr auto operator[](PortName<Name, Kind>) const
        {
            if constexpr (Kind == NamedPortKind::sample) {
                return static_output<details::static_output_port_index<NodeType, Name>()>();
            } else {
                return event_port(Name.view());
            }
        }

        template<class Member>
        constexpr auto operator[](Member) const
        requires std::same_as<typename std::remove_cvref_t<Member>::channel_type,
                              ChannelType>
        {
            using ConcreteRef = TypedNodeRef<NodeType>;
            using MemberType = std::remove_cvref_t<Member>;
            if (!this->_graph_builder) {
                details::error("attempted to select a tile from a null tiled node ref");
            }
            return ConcreteRef(
                *this->_graph_builder,
                this->_graph_builder->_node_bundles.tiled_member(
                    this->_index, MemberType::channel_ordinal));
        }

        template<size_t I>
        constexpr auto static_output() const
        {
            static_assert(I < details::static_output_count_v<NodeType>);
            if (!this->_graph_builder) {
                details::error("attempted to use a null tiled TypedNodeRef");
            }
            return TypedSamplePortTileRef<ChannelType>{
                SamplePortRef{*this->_graph_builder,
                    NodeBundlePortId{this->_index, PortKind::sample, I}}};
        }

        constexpr operator TypedSamplePortTileRef<ChannelType>() const
        requires (details::static_output_count_v<NodeType> == 1)
        {
            return static_output<0>();
        }

        template<size_t I>
        constexpr auto get() const
        {
            static_assert(I < details::static_output_count_v<NodeType>);
            return static_output<I>();
        }

        constexpr EventPortRef event_port(size_t output_ordinal) const
        {
            if (!this->_graph_builder) {
                details::error("attempted to use a null tiled TypedNodeRef");
            }
            return this->_graph_builder->event_output(
                {this->_index, PortKind::event, output_ordinal});
        }

        constexpr EventPortRef event_port(std::string_view name) const
        {
            return event_port(this->_graph_builder->event_port_index(
                this->_index, false, name));
        }

        constexpr Self connect_event_input(size_t input_ordinal, EventPortRef source) const
        {
            if (!this->_graph_builder) {
                details::error("attempted to use a null tiled TypedNodeRef");
            }
            this->_graph_builder->connect_event_input(
                {this->_index, PortKind::event, input_ordinal}, source);
            return _clone_handle();
        }

        constexpr Self connect_event_input(std::string_view name, EventPortRef source) const
        {
            if (!this->_graph_builder) {
                details::error("attempted to use a null tiled TypedNodeRef");
            }
            return connect_event_input(this->_graph_builder->event_port_index(
                this->_index, true, name), source);
        }

        template<class... Args>
        requires details::valid_node_call_args_v<Args...>
        constexpr Self operator()(Args&&... args) const
        {
            if (!this->_graph_builder) {
                details::error("attempted to use a null tiled TypedNodeRef");
            }
            static constexpr auto inputs = NodeType::inputs();
            size_t positional_input = 0;
            auto connect_input = [&]<class Value>(size_t input_ordinal,
                                                   Value&& value) {
                if (input_ordinal >= inputs.size()) {
                    details::error("too many sample inputs for tiled node");
                }
                auto source = this->_graph_builder->lift_to_sample_port(
                    std::forward<Value>(value));
                this->_graph_builder->connect_sample_input(
                    {this->_index, PortKind::sample, input_ordinal},
                    std::move(source));
            };
            auto process = [&](auto&& arg) {
                using Arg = std::remove_cvref_t<decltype(arg)>;
                if constexpr (details::is_named_arg_v<Arg>) {
                    if constexpr (Arg::kind == NamedPortKind::sample) {
                        constexpr auto input_ordinal =
                            details::static_input_port_index<NodeType, Arg::name>();
                        connect_input(input_ordinal, std::forward<decltype(arg)>(arg).value);
                    } else {
                        connect_event_input(std::string_view{Arg::name.value},
                                            std::forward<decltype(arg)>(arg).value);
                    }
                } else {
                    connect_input(positional_input++, std::forward<decltype(arg)>(arg));
                }
            };
            (process(std::forward<Args>(args)), ...);
            return _clone_handle();
        }
    };

    namespace details {
        template<typename Node>
        using node_ref_for_t = std::conditional_t<
            should_preserve_node_type_v<std::remove_cvref_t<Node>>,
            TypedNodeRef<std::remove_cvref_t<Node>>, NodeRef>;
    }

    template<fixed_string Name, class ChannelType, size_t ChannelOrdinal>
    template<class T>
    constexpr auto ChannelPortName<Name, ChannelType, ChannelOrdinal>::operator=(T&& value) const
    {
        using Value = std::remove_cvref_t<T>;
        if constexpr (std::same_as<Value, SamplePortRef>) {
            return ChannelNamedArg<Name, ChannelType, ChannelOrdinal, SamplePortRef>{
                .value = static_cast<SamplePortRef>(std::forward<T>(value)),
            };
        } else if constexpr (std::same_as<Value, EventPortRef>) {
            return ChannelNamedArg<Name, ChannelType, ChannelOrdinal, EventPortRef>{
                .value = std::forward<T>(value),
            };
        } else if constexpr (requires(Value const& ref) { ref._clone_handle(); }) {
            using Handle = decltype(value._clone_handle());
            return ChannelNamedArg<Name, ChannelType, ChannelOrdinal, Handle>{
                .value = value._clone_handle(),
            };
        } else {
            return ChannelNamedArg<Name, ChannelType, ChannelOrdinal, Value>{
                .value = std::forward<T>(value),
            };
        }
    }

    template<fixed_string Name, NamedPortKind Kind>
    template<class T>
    constexpr auto PortName<Name, Kind>::operator=(T&& value) const
    {
        using Value = std::remove_cvref_t<T>;
        if constexpr (std::same_as<Value, SamplePortRef>) {
            return NamedArg<Name, SamplePortRef, Kind>{ static_cast<SamplePortRef>(std::forward<T>(value)) };
        } else if constexpr (std::same_as<Value, EventPortRef>) {
            return NamedArg<Name, EventPortRef, Kind>{
                std::forward<T>(value)
            };
        } else if constexpr (requires(Value const& ref) { ref._clone_handle(); }) {
            using Handle = decltype(value._clone_handle());
            return NamedArg<Name, Handle, Kind>{ value._clone_handle() };
        } else {
            return NamedArg<Name, Value, Kind>{ std::forward<T>(value) };
        }
    }
}
