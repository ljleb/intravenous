#pragma once

#include <intravenous/graph/node.h>
#include <intravenous/channel_ports.h>

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace iv {
    class GraphBuilder;
    template<class ChannelType, SampleStreamLayout Layout>
    class TypedSamplePortRef;
    template<class ChannelType, SampleStreamLayout Layout, class Member>
    class TypedSamplePortChannelRef;
    template<class ChannelType>
    class TypedSamplePortTileRef;
    template<class ChannelType, class Member>
    class TypedSamplePortTileChannelRef;

    struct SamplePortRef {
        GraphBuilder* graph_builder{};
        size_t node_index{};
        size_t output_port{};

        SamplePortRef() = default;
        SamplePortRef(SamplePortRef const&) = default;
        SamplePortRef(SamplePortRef&&) noexcept = default;
        explicit SamplePortRef(GraphBuilder& graph_builder_, size_t node_index, size_t output_port);
        operator PortId() const { return { node_index, output_port }; }

        SamplePortRef& operator=(SamplePortRef const&) = default;
        SamplePortRef& operator=(SamplePortRef&& rhs) = default;
        SamplePortRef _clone_handle() const;

        SamplePortRef detach(size_t loop_extra_latency = 1) const;
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
    template<class ChannelType>
    class TypedSamplePortTileRef {
        std::array<SamplePortRef, ChannelType::channel_count> _members {};

    public:
        using channel_type = ChannelType;

        TypedSamplePortTileRef() = default;
        explicit TypedSamplePortTileRef(
            std::array<SamplePortRef, ChannelType::channel_count> members)
            : _members(std::move(members)) {}

        template<class Member>
        auto operator[](Member) const
        requires std::same_as<typename std::remove_cvref_t<Member>::channel_type, ChannelType>;

    private:
        template<class, class>
        friend class TypedSamplePortTileChannelRef;
    };

    template<class ChannelType, class Member>
    class TypedSamplePortTileChannelRef {
        static_assert(std::same_as<typename Member::channel_type, ChannelType>);

        SamplePortRef _port {};

    public:
        using channel_type = ChannelType;
        using member_type = Member;

        explicit TypedSamplePortTileChannelRef(SamplePortRef port) : _port(std::move(port)) {}

        operator SamplePortRef() const { return _port; }
        SamplePortRef const& erased() const { return _port; }
    };

    template<class ChannelType>
    template<class Member>
    auto TypedSamplePortTileRef<ChannelType>::operator[](Member) const
    requires std::same_as<typename std::remove_cvref_t<Member>::channel_type, ChannelType>
    {
        using MemberType = std::remove_cvref_t<Member>;
        return TypedSamplePortTileChannelRef<ChannelType, MemberType>{
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
        operator PortId() const { return static_cast<PortId>(port); }

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

        EventPortRef() = default;
        explicit EventPortRef(GraphBuilder& graph_builder_, size_t node_index, size_t output_port);
        operator PortId() const { return { node_index, output_port }; }

        std::string to_string() const;
    };

    struct PublicEventInputRef {
        EventPortRef port {};
        PublicEventInputRef() = default;
        explicit PublicEventInputRef(EventPortRef port_) : port(std::move(port_)) {}
        operator EventPortRef() const { return port; }
        operator PortId() const { return static_cast<PortId>(port); }
        void _annotate_source_info(
            std::string_view declaration_identity, std::string_view file_path,
            uint32_t begin, uint32_t end) const;
    };
}
