#pragma once

#include <intravenous/graph/node.h>
#include <intravenous/graph/builder/node_bundles.h>
#include <intravenous/channel_ports.h>

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace iv {
    class GraphBuilder;
    template<class ChannelType, SampleStreamLayout Layout>
    class TypedSamplePortRef;
    template<class ChannelType, SampleStreamLayout Layout, class Member>
    class TypedSamplePortChannelRef;
    template<class ChannelType, SampleStreamLayout Layout = SampleStreamLayout::planar>
    class TypedSamplePortTileRef;
    template<class ChannelType, class Member,
             SampleStreamLayout Layout = SampleStreamLayout::planar>
    class TypedSamplePortTileChannelRef;

    // Explicit builder-boundary addresses. Their legacy_port() projections are
    // temporary bridges while GraphBuilderTopology still stores boundary edges
    // in ConcretePortId. The semantic identity no longer depends on sentinel
    // node indices in port-ref consumers.
    struct GraphInputPortId {
        PortKind port_kind = PortKind::sample;
        size_t port_ordinal = 0;

        ConcretePortId legacy_port() const noexcept
        {
            return {GRAPH_ID, port_ordinal};
        }

        bool operator==(GraphInputPortId const&) const = default;
    };

    struct ScopeBoundaryPortId {
        PortKind port_kind = PortKind::sample;
        size_t boundary_ordinal = 0;

        ConcretePortId legacy_port() const noexcept
        {
            return {GRAPH_ID - 1 - boundary_ordinal, 0};
        }

        static ScopeBoundaryPortId from_legacy(ConcretePortId port, PortKind kind) noexcept
        {
            return ScopeBoundaryPortId{
                .port_kind = kind,
                .boundary_ordinal = GRAPH_ID - 1 - port.node,
            };
        }

        bool operator==(ScopeBoundaryPortId const&) const = default;
    };

    struct SamplePortRef {
        GraphBuilder* graph_builder{};
        size_t node_index{};
        size_t output_port{};
        std::optional<NodeBundlePortId> node_bundle_port{};
        std::optional<GraphInputPortId> graph_input_port{};
        std::optional<ScopeBoundaryPortId> scope_boundary_port{};

        SamplePortRef() = default;
        SamplePortRef(SamplePortRef const&) = default;
        SamplePortRef(SamplePortRef&&) noexcept = default;
        explicit SamplePortRef(GraphBuilder& graph_builder_, size_t node_index, size_t output_port);
        explicit SamplePortRef(GraphBuilder& graph_builder_, NodeBundlePortId bundle_port);
        operator ConcretePortId() const;

        SamplePortRef& operator=(SamplePortRef const&) = default;
        SamplePortRef& operator=(SamplePortRef&& rhs) = default;
        SamplePortRef _clone_handle() const;

        SamplePortRef detach(size_t loop_extra_latency = 1) const;
        bool is_graph_input() const { return graph_input_port.has_value(); }
        bool is_scope_boundary() const { return scope_boundary_port.has_value(); }
        std::string to_string() const;
    };

    template<class ChannelType, SampleStreamLayout Layout>
    class TypedSamplePortRef {
        SamplePortRef _port;

    public:
        using channel_type = ChannelType;
        static constexpr auto sample_layout = Layout;

        TypedSamplePortRef() = default;
        explicit TypedSamplePortRef(SamplePortRef port) : _port(std::move(port)) {}

        operator SamplePortRef() const { return _port; }
        SamplePortRef const& erased() const { return _port; }

        template<class Channel>
        auto operator[](Channel) const
        requires std::same_as<typename std::remove_cvref_t<Channel>::channel_type, ChannelType>;
    };

    template<class ChannelType, SampleStreamLayout Layout, class Member>
    class TypedSamplePortChannelRef {
        static_assert(std::same_as<typename Member::channel_type, ChannelType>);
        TypedSamplePortRef<ChannelType, Layout> _port;

    public:
        using channel_type = ChannelType;
        using member_type = Member;
        static constexpr auto sample_layout = Layout;

        explicit TypedSamplePortChannelRef(TypedSamplePortRef<ChannelType, Layout> port) :
            _port(std::move(port))
        {}

        TypedSamplePortRef<ChannelType, Layout> const& port() const { return _port; }
    };

    // A promoted tiled port. Unlike TypedSamplePortRef<C, Layout>, this is
    // not one native C-channel concrete port: every member names a distinct
    // mono concrete endpoint.
    template<class ChannelType, SampleStreamLayout Layout>
    class TypedSamplePortTileRef {
        SamplePortRef _promoted {};
        std::array<SamplePortRef, ChannelType::channel_count> _members {};

    public:
        using channel_type = ChannelType;
        static constexpr auto sample_layout = Layout;

        TypedSamplePortTileRef() = default;

        explicit TypedSamplePortTileRef(
            std::array<SamplePortRef, ChannelType::channel_count> members)
            : _members(std::move(members)) {}

            TypedSamplePortTileRef(
            SamplePortRef promoted,
            std::array<SamplePortRef, ChannelType::channel_count> members)
            : _promoted(std::move(promoted)), _members(std::move(members)) {}

        operator SamplePortRef() const { return _promoted; }
        SamplePortRef const& erased() const { return _promoted; }

        std::array<SamplePortRef, ChannelType::channel_count> const& members() const
        {
            return _members;
        }

        template<class Member>
        auto operator[](Member) const
        requires std::same_as<typename std::remove_cvref_t<Member>::channel_type, ChannelType>;

    private:
        template<class, class, SampleStreamLayout>
        friend class TypedSamplePortTileChannelRef;
    };

    template<class ChannelType, class Member, SampleStreamLayout Layout>
    class TypedSamplePortTileChannelRef {
        static_assert(std::same_as<typename Member::channel_type, ChannelType>);
        SamplePortRef _port {};

    public:
        using channel_type = ChannelType;
        using member_type = Member;
        static constexpr auto sample_layout = Layout;

        explicit TypedSamplePortTileChannelRef(SamplePortRef port) : _port(std::move(port)) {}

        operator SamplePortRef() const { return _port; }
        SamplePortRef const& erased() const { return _port; }
    };

    template<class ChannelType, SampleStreamLayout Layout>
    template<class Member>
    auto TypedSamplePortTileRef<ChannelType, Layout>::operator[](Member) const
    requires std::same_as<typename std::remove_cvref_t<Member>::channel_type, ChannelType>
    {
        using MemberType = std::remove_cvref_t<Member>;
        return TypedSamplePortTileChannelRef<ChannelType, MemberType, Layout>{
            _members[MemberType::channel_ordinal]};
    }

    template<class ChannelType, SampleStreamLayout Layout>
    template<class Channel>
    auto TypedSamplePortRef<ChannelType, Layout>::operator[](Channel) const
    requires std::same_as<typename std::remove_cvref_t<Channel>::channel_type, ChannelType>
    {
        using Member = std::remove_cvref_t<Channel>;
        return TypedSamplePortChannelRef<ChannelType, Layout, Member>{*this};
    }

    struct PublicSampleInputRef {
        SamplePortRef port {};

        PublicSampleInputRef() = default;
        explicit PublicSampleInputRef(SamplePortRef port_) : port(std::move(port_)) {}

        operator SamplePortRef() const { return port; }
        operator ConcretePortId() const { return static_cast<ConcretePortId>(port); }

        void _annotate_source_info(
            std::string_view declaration_identity,
            std::string_view file_path,
            uint32_t begin,
            uint32_t end) const;
    };

    struct EventPortRef {
        GraphBuilder* graph_builder {};
        size_t node_index {};
        size_t output_port {};
        std::optional<GraphInputPortId> graph_input_port {};
        std::optional<ScopeBoundaryPortId> scope_boundary_port {};

        EventPortRef() = default;
        explicit EventPortRef(GraphBuilder& graph_builder_, size_t node_index, size_t output_port);
        operator ConcretePortId() const
        {
            if (graph_input_port) return graph_input_port->legacy_port();
            if (scope_boundary_port) return scope_boundary_port->legacy_port();
            return {node_index, output_port};
        }

        bool is_graph_input() const { return graph_input_port.has_value(); }
        bool is_scope_boundary() const { return scope_boundary_port.has_value(); }
        std::string to_string() const;
    };

    struct PublicEventInputRef {
        EventPortRef port {};
        PublicEventInputRef() = default;
        explicit PublicEventInputRef(EventPortRef port_) : port(std::move(port_)) {}
        operator EventPortRef() const { return port; }
        operator ConcretePortId() const { return static_cast<ConcretePortId>(port); }
        void _annotate_source_info(
            std::string_view declaration_identity, std::string_view file_path,
            uint32_t begin, uint32_t end) const;
    };
}
