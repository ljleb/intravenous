#pragma once

#include <intravenous/graph/node.h>
#include <intravenous/graph/builder/node_bundles.h>
#include <intravenous/graph/compiler.h>  // details::error
#include <intravenous/channel_ports.h>

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

    // Explicit builder-boundary addresses. Their topology_port() projections
    // are used only when a logical boundary is recorded in GraphBuilderTopology;
    // the semantic identity itself never depends on a sentinel node index.
    struct GraphInputPortId {
        PortKind port_kind = PortKind::sample;
        size_t port_ordinal = 0;

        TopologyPortId topology_port() const noexcept
        {
            return {GRAPH_ID, port_ordinal};
        }

        bool operator==(GraphInputPortId const&) const = default;
    };

    struct ScopeBoundaryPortId {
        PortKind port_kind = PortKind::sample;
        size_t boundary_ordinal = 0;

        TopologyPortId topology_port() const noexcept
        {
            return {GRAPH_ID - 1 - boundary_ordinal, 0};
        }

        static ScopeBoundaryPortId from_topology(TopologyPortId port, PortKind kind) noexcept
        {
            return ScopeBoundaryPortId{
                .port_kind = kind,
                .boundary_ordinal = GRAPH_ID - 1 - port.node,
            };
        }

        bool operator==(ScopeBoundaryPortId const&) const = default;
    };

    // Internal result of crossing the logical sample-reference boundary.
    // It is intentionally distinct from SamplePortRef: callers that hold one
    // have already chosen a builder-topology edge source.
    struct MaterializedSamplePort {
        TopologyPortId port{};
    };

    struct SamplePortRef {
        GraphBuilder* graph_builder{};
        ChannelTypeId channel_type = ChannelTypeId::mono;
        std::vector<SampleOutputChannelId> channels{};
        std::optional<NodeBundlePortId> node_bundle_port{};
        std::optional<GraphInputPortId> graph_input_port{};
        std::optional<ScopeBoundaryPortId> scope_boundary_port{};

        SamplePortRef() = default;
        SamplePortRef(SamplePortRef const&) = default;
        SamplePortRef(SamplePortRef&&) noexcept = default;
        explicit SamplePortRef(GraphBuilder& graph_builder_, size_t node_index, size_t output_port);
        explicit SamplePortRef(GraphBuilder& graph_builder_, GraphInputPortId graph_input);
        explicit SamplePortRef(GraphBuilder& graph_builder_, ScopeBoundaryPortId scope_boundary);
        explicit SamplePortRef(GraphBuilder& graph_builder_, NodeBundlePortId bundle_port);
        explicit SamplePortRef(GraphBuilder& graph_builder_, ChannelTypeId channel_type_,
                               std::vector<SampleOutputChannelId> channels_);
        operator TopologyPortId() const;

        SamplePortRef& operator=(SamplePortRef const&) = default;
        SamplePortRef& operator=(SamplePortRef&& rhs) = default;
        SamplePortRef _clone_handle() const;
        SamplePortRef select_channel(size_t channel) const;

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
        SamplePortRef _port;

    public:
        using channel_type = ChannelType;
        using member_type = Member;
        static constexpr auto sample_layout = Layout;

        explicit TypedSamplePortChannelRef(TypedSamplePortRef<ChannelType, Layout> port)
            : _port(port.erased().select_channel(Member::channel_ordinal))
        {}

        operator SamplePortRef() const { return _port; }
        SamplePortRef const& erased() const { return _port; }
        SamplePortRef const& port() const { return _port; }
    };

    // A tiled node output keeps its promoted logical bundle port separate
    // from the per-channel refs used to preserve physical tiling. Member-only
    // construction represents an aggregate with no promoted bundle identity.
    template<class ChannelType, SampleStreamLayout Layout>
    class TypedSamplePortTileRef {
        SamplePortRef _promoted {};
        SamplePortRef _structural {};
        std::array<SamplePortRef, ChannelType::channel_count> _members {};

        static SamplePortRef make_structural_port(
            std::array<SamplePortRef, ChannelType::channel_count> const& members)
        {
            static_assert(ChannelType::channel_count > 0);
            auto* builder = members.front().graph_builder;
            if (!builder) {
                details::error("cannot tile an empty sample output");
            }

            std::vector<SampleOutputChannelId> channels;
            channels.reserve(ChannelType::channel_count);
            for (auto const& member : members) {
                if (member.graph_builder != builder) {
                    details::error("cannot tile sample outputs from different builders");
                }
                if (member.channel_type != ChannelTypeId::mono
                    || member.channels.size() != 1) {
                    details::error(
                        "each g.tile channel must be a scalar sample expression");
                }
                channels.push_back(member.channels.front());
            }
            return SamplePortRef(
                *builder, ChannelTypeTraits<ChannelType>::id, std::move(channels));
        }

    public:
        using channel_type = ChannelType;
        static constexpr auto sample_layout = Layout;

        TypedSamplePortTileRef() = default;

        explicit TypedSamplePortTileRef(
            std::array<SamplePortRef, ChannelType::channel_count> members)
            : _structural(make_structural_port(members)),
              _members(std::move(members)) {}

        TypedSamplePortTileRef(
            SamplePortRef promoted,
            std::array<SamplePortRef, ChannelType::channel_count> members)
            : _promoted(std::move(promoted)),
              _structural(make_structural_port(members)),
              _members(std::move(members))
        {
            if (_promoted.graph_builder != _structural.graph_builder) {
                details::error(
                    "promoted tiled output belongs to a different builder");
            }
            _structural.node_bundle_port = _promoted.node_bundle_port;
        }

        bool has_promoted_port() const { return _promoted.graph_builder != nullptr; }
        operator SamplePortRef() const { return _structural; }
        SamplePortRef const& erased() const { return _structural; }

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
        operator TopologyPortId() const { return static_cast<TopologyPortId>(port); }

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
        EventTypeId type = EventTypeId::empty;
        std::vector<EventOutputPortId> sources {};
        std::optional<GraphInputPortId> graph_input_port {};
        std::optional<ScopeBoundaryPortId> scope_boundary_port {};

        EventPortRef() = default;
        explicit EventPortRef(GraphBuilder& graph_builder_, size_t node_index, size_t output_port);
        explicit EventPortRef(GraphBuilder& graph_builder_, GraphInputPortId graph_input);
        explicit EventPortRef(GraphBuilder& graph_builder_, ScopeBoundaryPortId scope_boundary);
        explicit EventPortRef(
            GraphBuilder&, EventTypeId, std::vector<EventOutputPortId>,
            TopologyPortId topology_projection);
        operator TopologyPortId() const
        {
            if (graph_input_port) return graph_input_port->topology_port();
            if (scope_boundary_port) return scope_boundary_port->topology_port();
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
        operator TopologyPortId() const { return static_cast<TopologyPortId>(port); }
        void _annotate_source_info(
            std::string_view declaration_identity, std::string_view file_path,
            uint32_t begin, uint32_t end) const;
    };
}
