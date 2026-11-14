#pragma once

#include <intravenous/graph/builder/node_call.h>
#include <intravenous/graph/builder/output_refs.h>
#include <intravenous/graph/builder/session_ops.h>
#include <intravenous/graph/error.h>
#include <intravenous/graph/node_ports.h>
#include <intravenous/node/address.h>
#include <intravenous/node/static_port_access.h>

#include <concepts>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

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

    namespace details {
        // These are the erased, short-lived result of a typed node-call
        // argument pack. They cross into the precompiled builder before the
        // call returns, so no module-owned data is retained here.
        enum class NodeCallInputTarget : uint8_t {
            positional,
            named,
            explicit_index,
        };

        struct NodeCallSampleInput {
            SamplePortRef source;
            std::string_view name;
            size_t input_index;
            NodeCallInputTarget target;
        };

        struct NodeCallEventInput {
            EventPortRef source;
            std::string_view name;
            size_t input_index;
            NodeCallInputTarget target;
        };

        // Unlike std::span, these two tiny concrete views do not make the
        // standard library instantiate range machinery for every module-side
        // node-call argument pack. They are internal and only valid for the
        // synchronous apply_node_call invocation.
        struct NodeCallSampleInputList {
            NodeCallSampleInput const* data;
            size_t size;
        };

        struct NodeCallEventInputList {
            NodeCallEventInput const* data;
            size_t size;
        };

        template<size_t SampleCount, size_t EventCount>
        struct NodeCallRequests {
            std::array<NodeCallSampleInput, SampleCount> sample_inputs {};
            std::array<NodeCallEventInput, EventCount> event_inputs {};
            size_t sample_input_count = 0;
            size_t event_input_count = 0;
        };

        template<class... Args>
        auto make_node_call_requests(BuilderSession*, Args&&...);

    }

    // The untyped handle is the public base for every node-bundle case.
    // It deliberately addresses a NodeBundle, never an assumed concrete node.
    // Its identity is the owning BuilderSession, never the GraphBuilder view
    // through which it was created.
    class NodeRef {
    protected:
        details::BuilderSession* _session{};
        size_t _index{};
        // The declaration identity itself is recorded in the host annotation
        // table. The façade only needs this bit to preserve the one-time
        // initialization rule for annotated virtual refs.
        mutable bool _has_source_identity = false;
        bool _has_message_path = false;
        std::vector<ProjectNodePathSelector> _message_path{};

        friend class GraphBuilder;
        friend class GraphBuilderAnnotations;

        void apply_node_call(
            details::NodeCallSampleInputList,
            details::NodeCallEventInputList) const;

    public:
        constexpr NodeRef() = default;
        NodeRef(NodeRef const&);
        NodeRef(NodeRef&&) noexcept;
        constexpr explicit NodeRef(details::BuilderSession* session, size_t index) :
            _session(session),
            _index(index)
        {}
        explicit NodeRef(GraphBuilder const& graph_builder, size_t index);

        NodeRef& operator=(NodeRef const& rhs);
        NodeRef& operator=(NodeRef&& rhs);

        NodeRef node_ref() const;
        constexpr size_t node_bundle_handle() const
        {
            if (!_session) {
                details::error("attempted to use a null NodeRef");
            }
            return _index;
        }
        NodeRef _clone_handle() const;
        [[nodiscard]] constexpr details::BuilderSession* _builder_session() const noexcept
        {
            return _session;
        }
        SamplePortRef operator[](size_t output_port) const;
        SamplePortRef operator[](std::string_view output_name) const;
        template<class Member>
        auto operator[](Member member) const
        requires requires {
            typename std::remove_cvref_t<Member>::channel_type;
            std::remove_cvref_t<Member>::channel_index;
        }
        {
            // An erased node only knows its output layout at build time.  Its
            // conversion to SamplePortRef requires one sample output, then
            // SamplePortRef validates the requested channel member at runtime.
            return static_cast<SamplePortRef>(*this)[member];
        }
        template<fixed_string Name, NamedPortKind Kind>
        auto operator[](PortName<Name, Kind>) const
        {
            if constexpr (Kind == NamedPortKind::sample) {
                return (*this)[Name.view()];
            } else {
                return event_port(Name.view());
            }
        }
        operator SamplePortRef() const;
        // Mixed declaration order, unlike the per-kind operator[]/event_port indices.
        NodeBundlePortId output_port_at(size_t position) const;
        size_t sample_input_count() const;
        size_t sample_output_count() const;
        size_t event_input_count() const;
        size_t event_output_count() const;
        bool input_is_connected(size_t input_port) const;
        bool event_input_is_connected(size_t input_port) const;
        template<class T>
        NodeRef connect_input(size_t input_port, T&& value) const;
        template<class T>
        NodeRef connect_input(std::string_view input_name, T&& value) const;
        template<class... Args>
        NodeRef operator()(Args&&... args) const;
        NodeRef connect_event_input(size_t input_port, EventPortRef value) const;
        NodeRef connect_event_input(std::string_view input_name, EventPortRef value) const;
        EventPortRef event_port(size_t output_port) const;
        EventPortRef event_port(std::string_view output_name) const;
        EventPortRef event_port() const;
        NodeRef ttl(size_t samples) const;
        NodeRef no_ttl() const;
        std::string to_string() const;
        [[nodiscard]] bool _has_semantic_message_path() const noexcept
        {
            return _has_message_path;
        }
        [[nodiscard]] std::span<ProjectNodePathSelector const>
        _semantic_message_path() const noexcept { return _message_path; }
        void _set_semantic_message_path(
            std::span<ProjectNodePathSelector const> path)
        {
            _message_path.assign(path.begin(), path.end());
            _has_message_path = true;
        }
        void _annotate_source_info(
            std::string_view declaration_identity,
            std::string_view file_path,
            uint32_t begin,
            uint32_t end
        ) const;
        void _annotate_input_source_info(
            PortKind,
            std::string_view port_name,
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

        constexpr Derived& derived() { return static_cast<Derived&>(*this); }

    public:
        using Base::Base;

        Derived& operator=(Derived const& rhs)
        {
            Base::operator=(static_cast<NodeRef const&>(rhs));
            return derived();
        }

        Derived& operator=(Derived&& rhs)
        {
            Base::operator=(static_cast<NodeRef&&>(rhs));
            return derived();
        }

        Derived _clone_handle() const
        {
            if (!this->_session) {
                return Derived{};
            }
            Derived result(this->_session, this->_index);
            result._has_source_identity = this->_has_source_identity;
            result._has_message_path = this->_has_message_path;
            result._message_path = this->_message_path;
            return result;
        }

        Derived ttl(size_t samples) const
        {
            Base::ttl(samples);
            return _clone_handle();
        }

        template<class T>
        Derived connect_input(size_t input_port, T&& value) const
        {
            Base::connect_input(input_port, std::forward<T>(value));
            return _clone_handle();
        }

        template<class T>
        Derived connect_input(std::string_view input_name, T&& value) const
        {
            Base::connect_input(input_name, std::forward<T>(value));
            return _clone_handle();
        }

        Derived connect_event_input(size_t input_port, EventPortRef value) const
        {
            Base::connect_event_input(input_port, std::move(value));
            return _clone_handle();
        }

        Derived no_ttl() const
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
        using Base::_session;
        using Base::_index;
        using Base::to_string;

        TypedNodeRef(TypedNodeRef const&) = default;
        TypedNodeRef(TypedNodeRef&&) noexcept = default;
        TypedNodeRef& operator=(TypedNodeRef const& rhs)
        {
            Base::operator=(rhs);
            return *this;
        }
        TypedNodeRef& operator=(TypedNodeRef&& rhs)
        {
            Base::operator=(std::move(rhs));
            return *this;
        }
        NodePorts const& ports() const;

        SamplePortRef operator[](size_t output_index) const;
        SamplePortRef operator[](std::string_view output_name) const;

        template<fixed_string Name, NamedPortKind Kind>
        auto operator[](PortName<Name, Kind>) const
        {
            if constexpr (Kind == NamedPortKind::sample) {
                constexpr auto output_index = details::static_output_port_index<NodeType, Name>();
                constexpr auto layout = details::static_output_port_layout<NodeType, Name>();
                using ChannelType = typename RuntimeChannelTypeTraits<layout.channel_type>::type;
                if (!this->_session) {
                    details::error("attempted to use a null NodeRef");
                }
                return TypedSamplePortRef<ChannelType>{
                    this->operator[](output_index)};
            } else {
                return event_port(Name.view());
            }
        }
        EventPortRef event_port(size_t output_index) const;
        EventPortRef event_port(std::string_view output_name) const;
        EventPortRef event_port() const;
        operator SamplePortRef() const;
        template<class... Args>
        requires(details::node_call_enabled<NodeType, Args...>)
        TypedNodeRef operator()(Args&&... args) const;

        template<class... Args>
        requires(!details::node_call_enabled<NodeType, Args...>)
        TypedNodeRef operator()(Args&&... args) const = delete;

        template<class T>
        TypedNodeRef connect_input(size_t input_port, T&& value) const;
        template<class T>
        TypedNodeRef connect_input(std::string_view input_name, T&& value) const;
        TypedNodeRef connect_event_input(size_t input_port, EventPortRef value) const;
        TypedNodeRef connect_event_input(std::string_view input_name, EventPortRef value) const;

        SamplePortRef detach(
            size_t loop_extra_latency = 1,
            std::optional<Sample> initial_value = std::nullopt) const;

        template<size_t I>
        auto static_output() const
        {
            constexpr auto layout = details::static_output_port_layout_at<NodeType, I>();
            using ChannelType = typename RuntimeChannelTypeTraits<layout.channel_type>::type;
            return TypedSamplePortRef<ChannelType>{
                this->operator[](I)};
        }

        // Every typed node has constexpr output configurations. `get` is the
        // tuple spelling for selecting one of them.
        template<size_t I>
        auto get() const
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
        using Base::event_port;
        using Base::_session;
        using Base::_index;
        using Base::to_string;

        constexpr TypedNodeRef() = default;
        constexpr explicit TypedNodeRef(
            details::BuilderSession* session, NodeBundleHandle handle) :
            Base(session, handle) {}
        explicit TypedNodeRef(GraphBuilder const& graph_builder, NodeBundleHandle handle) :
            Base(graph_builder, handle) {}

        TypedNodeRef(TypedNodeRef const&) = default;
        TypedNodeRef(TypedNodeRef&&) noexcept = default;

        TypedNodeRef& operator=(TypedNodeRef const& rhs)
        {
            Base::operator=(rhs);
            return *this;
        }
        TypedNodeRef& operator=(TypedNodeRef&& rhs)
        {
            Base::operator=(std::move(rhs));
            return *this;
        }

        Self _clone_handle() const
        {
            if (!this->_session) {
                return Self {};
            }
            Self result(this->_session, this->_index);
            result._has_source_identity = this->_has_source_identity;
            result._has_message_path = this->_has_message_path;
            result._message_path = this->_message_path;
            return result;
        }

        // Keep the dynamic output selectors available without importing the
        // base channel-member selector.  A tiled typed ref has its own
        // compile-time checked channel selector below.
        SamplePortRef operator[](size_t output_index) const
        {
            return Base::operator[](output_index);
        }

        SamplePortRef operator[](std::string_view output_name) const
        {
            return Base::operator[](output_name);
        }

        template<fixed_string Name, NamedPortKind Kind>
        auto operator[](PortName<Name, Kind>) const
        {
            if constexpr (Kind == NamedPortKind::sample) {
                return static_output<details::static_output_port_index<NodeType, Name>()>();
            } else {
                return event_port(Name.view());
            }
        }

        template<class Member>
        auto operator[](Member) const
        requires std::same_as<typename std::remove_cvref_t<Member>::channel_type,
                              ChannelType>
        {
            using ConcreteRef = TypedNodeRef<NodeType>;
            using MemberType = std::remove_cvref_t<Member>;
            if (!this->_session) {
                details::error("attempted to select a tile from a null tiled node ref");
            }
            ConcreteRef result(
                this->_session,
                details::iv_builder_tiled_member(
                    this->_session, this->_index, MemberType::channel_index));
            if (this->_has_message_path) {
                auto path = this->_message_path;
                path.push_back(ProjectTiledChildSelector{
                    .name = std::string(
                        ChannelType::channel_names[MemberType::channel_index]),
                });
                result._set_semantic_message_path(path);
            }
            return result;
        }

        template<size_t I>
        auto static_output() const
        {
            static_assert(I < details::static_output_count_v<NodeType>);
            if (!this->_session) {
                details::error("attempted to use a null tiled TypedNodeRef");
            }
            return TypedSamplePortTileRef<ChannelType>{
                details::iv_builder_sample_port_from_output(
                    this->_session,
                    NodeBundlePortId{this->_index, PortKind::sample, I})};
        }

        operator TypedSamplePortTileRef<ChannelType>() const
        requires (details::static_output_count_v<NodeType> > 0)
        {
            return static_output<0>();
        }

        template<size_t I>
        auto get() const
        {
            static_assert(I < details::static_output_count_v<NodeType>);
            return static_output<I>();
        }

        EventPortRef event_port(size_t output_index) const
        {
            return NodeRef::event_port(output_index);
        }

        EventPortRef event_port(std::string_view name) const
        {
            return NodeRef::event_port(name);
        }

        Self connect_event_input(size_t input_index, EventPortRef source) const
        {
            return Base::connect_event_input(input_index, std::move(source));
        }

        Self connect_event_input(std::string_view name, EventPortRef source) const
        {
            NodeRef::connect_event_input(name, std::move(source));
            return _clone_handle();
        }

        template<class... Args>
        requires details::valid_node_call_args_v<Args...>
        Self operator()(Args&&... args) const
        {
            if (!this->_session) {
                details::error("attempted to use a null tiled TypedNodeRef");
            }
            auto requests = details::make_node_call_requests(
                this->_session, std::forward<Args>(args)...);
            this->apply_node_call(
                {.data = requests.sample_inputs.data(),
                 .size = requests.sample_input_count},
                {.data = requests.event_inputs.data(),
                 .size = requests.event_input_count});
            return _clone_handle();
        }
    };

    namespace details {
        template<typename Node>
        using node_ref_for_t = std::conditional_t<
            should_preserve_node_type_v<std::remove_cvref_t<Node>>,
            TypedNodeRef<std::remove_cvref_t<Node>>, NodeRef>;
    }

    template<fixed_string Name, class ChannelType, size_t ChannelIndex>
    template<class T>
    constexpr auto ChannelPortName<Name, ChannelType, ChannelIndex>::operator=(T&& value) const
    {
        using Value = std::remove_cvref_t<T>;
        // Channel qualification carries the static information needed for a
        // public port.  The source's concrete ref type is no longer useful
        // once it crosses the builder interface, so erase it here.  This
        // keeps variadic node/output call instantiations independent of the
        // particular typed-ref facade that produced the sample expression.
        if constexpr (std::convertible_to<Value, SamplePortRef>) {
            return ChannelNamedArg<Name, ChannelType, ChannelIndex, SamplePortRef>{
                .value = static_cast<SamplePortRef>(std::forward<T>(value)),
            };
        } else if constexpr (std::same_as<Value, EventPortRef>) {
            return ChannelNamedArg<Name, ChannelType, ChannelIndex, EventPortRef>{
                .value = std::forward<T>(value),
            };
        } else if constexpr (requires(Value const& ref) { ref._clone_handle(); }) {
            using Handle = decltype(value._clone_handle());
            return ChannelNamedArg<Name, ChannelType, ChannelIndex, Handle>{
                .value = value._clone_handle(),
            };
        } else {
            return ChannelNamedArg<Name, ChannelType, ChannelIndex, Value>{
                .value = std::forward<T>(value),
            };
        }
    }

    template<fixed_string Name, NamedPortKind Kind>
    template<class T>
    constexpr auto PortName<Name, Kind>::operator=(T&& value) const
    {
        using Value = std::remove_cvref_t<T>;
        // Preserve static typing while composing expressions, but normalize
        // every sample source at the GraphBuilder boundary.  Node calls only
        // use the named port's kind and name for compile-time validation; the
        // source layout is validated by the builder at runtime.
        if constexpr (Kind == NamedPortKind::sample &&
                      std::convertible_to<Value, SamplePortRef>) {
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
