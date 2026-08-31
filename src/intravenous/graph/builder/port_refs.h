#pragma once

#include <intravenous/graph/builder/node_bundles.hpp>
#include <intravenous/graph/error.h>
#include <intravenous/channel_ports.h>

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace iv {
class GraphBuilder;

template<class ChannelType>
class TypedSamplePortRef;
template<class ChannelType, class Member>
class TypedSamplePortChannelRef;
template<class ChannelType>
class TypedSamplePortTileRef;
template<class ChannelType, class Member>
class TypedSamplePortTileChannelRef;

// A sample expression is exactly the ordered semantic source channels that
// participate in an authored connection. It has no execution/topology address.
struct SamplePortRef {
  GraphBuilder* graph_builder{};
  ChannelTypeId channel_type = ChannelTypeId::mono;
  std::vector<SampleOutputChannelId> channels{};

  constexpr SamplePortRef() = default;
  constexpr SamplePortRef(SamplePortRef const&) = default;
  constexpr SamplePortRef(SamplePortRef&&) noexcept = default;
  constexpr explicit SamplePortRef(
      GraphBuilder&, NodeBundlePortId bundle_port);
  constexpr explicit SamplePortRef(GraphBuilder&, ChannelTypeId,
                                    std::vector<SampleOutputChannelId>);

  constexpr SamplePortRef& operator=(SamplePortRef const&) = default;
  constexpr SamplePortRef& operator=(SamplePortRef&&) noexcept = default;
  constexpr SamplePortRef _clone_handle() const;
  constexpr SamplePortRef select_channel(size_t channel) const;
  consteval SamplePortRef detach(size_t loop_extra_latency = 1) const;
  constexpr void _annotate_source_info(
      std::string_view, std::string_view, uint32_t, uint32_t) const;
  std::string to_string() const;
};

template<class ChannelType>
class TypedSamplePortRef {
  SamplePortRef _port;

public:
  using channel_type = ChannelType;

  constexpr TypedSamplePortRef() = default;
  constexpr explicit TypedSamplePortRef(SamplePortRef port)
      : _port(std::move(port)) {}

  constexpr operator SamplePortRef() const { return _port; }
  constexpr SamplePortRef const& erased() const { return _port; }
  constexpr void _annotate_source_info(
      std::string_view id, std::string_view file,
      uint32_t begin, uint32_t end) const {
    _port._annotate_source_info(id, file, begin, end);
  }

  template<class Channel>
  constexpr auto operator[](Channel) const
  requires std::same_as<typename std::remove_cvref_t<Channel>::channel_type,
                        ChannelType>;
};

template<class ChannelType, class Member>
class TypedSamplePortChannelRef {
  static_assert(std::same_as<typename Member::channel_type, ChannelType>);
  SamplePortRef _port;

public:
  using channel_type = ChannelType;
  using member_type = Member;
  constexpr explicit TypedSamplePortChannelRef(
      TypedSamplePortRef<ChannelType> port)
      : _port(port.erased().select_channel(Member::channel_ordinal)) {}

  constexpr operator SamplePortRef() const { return _port; }
  constexpr SamplePortRef const& erased() const { return _port; }
  constexpr SamplePortRef const& port() const { return _port; }
  constexpr void _annotate_source_info(
      std::string_view id, std::string_view file,
      uint32_t begin, uint32_t end) const {
    _port._annotate_source_info(id, file, begin, end);
  }
};

// Both a native tiled-node output and g.tile(...) have the same erased form:
// one semantic SamplePortRef. Channel selection is therefore just selection
// from that expression; there is no second structural/member representation.
template<class ChannelType>
class TypedSamplePortTileRef {
  SamplePortRef _port{};

  static constexpr SamplePortRef make_port(
      std::array<SamplePortRef, ChannelType::channel_count> const& members) {
    static_assert(ChannelType::channel_count > 0);
    auto* builder = members.front().graph_builder;
    if (!builder) details::error("cannot tile an empty sample output");

    std::vector<SampleOutputChannelId> channels;
    channels.reserve(ChannelType::channel_count);
    for (auto const& member : members) {
      if (member.graph_builder != builder)
        details::error("cannot tile sample outputs from different builders");
      if (member.channel_type != ChannelTypeId::mono ||
          member.channels.size() != 1)
        details::error("each g.tile channel must be a scalar sample expression");
      channels.push_back(member.channels.front());
    }
    return SamplePortRef(*builder, ChannelTypeTraits<ChannelType>::id,
                         std::move(channels));
  }

public:
  using channel_type = ChannelType;

  constexpr TypedSamplePortTileRef() = default;
  constexpr explicit TypedSamplePortTileRef(
      std::array<SamplePortRef, ChannelType::channel_count> members)
      : _port(make_port(members)) {}
  constexpr explicit TypedSamplePortTileRef(SamplePortRef port)
      : _port(std::move(port)) {
    if (!_port.graph_builder ||
        _port.channel_type != ChannelTypeTraits<ChannelType>::id ||
        _port.channels.size() != ChannelType::channel_count)
      details::error("typed tiled sample output does not match its channel type");
  }

  constexpr operator SamplePortRef() const { return _port; }
  constexpr SamplePortRef const& erased() const { return _port; }
  constexpr void _annotate_source_info(
      std::string_view id, std::string_view file,
      uint32_t begin, uint32_t end) const {
    _port._annotate_source_info(id, file, begin, end);
  }

  template<class Member>
  constexpr auto operator[](Member) const
  requires std::same_as<typename std::remove_cvref_t<Member>::channel_type,
                        ChannelType>;
};

template<class ChannelType, class Member>
class TypedSamplePortTileChannelRef {
  static_assert(std::same_as<typename Member::channel_type, ChannelType>);
  SamplePortRef _port{};

public:
  using channel_type = ChannelType;
  using member_type = Member;
  constexpr explicit TypedSamplePortTileChannelRef(SamplePortRef port)
      : _port(std::move(port)) {}

  constexpr operator SamplePortRef() const { return _port; }
  constexpr SamplePortRef const& erased() const { return _port; }
  constexpr void _annotate_source_info(
      std::string_view id, std::string_view file,
      uint32_t begin, uint32_t end) const {
    _port._annotate_source_info(id, file, begin, end);
  }
};

template<class ChannelType>
template<class Member>
constexpr auto TypedSamplePortTileRef<ChannelType>::operator[](Member) const
requires std::same_as<typename std::remove_cvref_t<Member>::channel_type,
                      ChannelType> {
  using MemberType = std::remove_cvref_t<Member>;
  return TypedSamplePortTileChannelRef<ChannelType, MemberType>{
      _port.select_channel(MemberType::channel_ordinal)};
}

template<class ChannelType>
template<class Channel>
constexpr auto TypedSamplePortRef<ChannelType>::operator[](Channel) const
requires std::same_as<typename std::remove_cvref_t<Channel>::channel_type,
                      ChannelType> {
  using Member = std::remove_cvref_t<Channel>;
  return TypedSamplePortChannelRef<ChannelType, Member>{*this};
}

struct PublicSampleInputRef {
  SamplePortRef port{};

  constexpr PublicSampleInputRef() = default;
  constexpr explicit PublicSampleInputRef(SamplePortRef port_)
      : port(std::move(port_)) {}
  constexpr operator SamplePortRef() const { return port; }

  constexpr void _annotate_source_info(
      std::string_view declaration_identity,
      std::string_view file_path,
      uint32_t begin, uint32_t end) const;
};

// Event expressions mirror SamplePortRef: only semantic source ports survive
// authoring. A tiled source is one TiledNodeBundle event port; lowering expands
// it to member execution ports when compiling the graph.
struct EventPortRef {
  GraphBuilder* graph_builder{};
  EventTypeId type = EventTypeId::empty;
  std::vector<EventOutputPortId> sources{};

  constexpr EventPortRef() = default;
  constexpr explicit EventPortRef(GraphBuilder&, NodeBundlePortId bundle_port);
  constexpr explicit EventPortRef(GraphBuilder&, EventTypeId,
                                   std::vector<EventOutputPortId>);
  constexpr void _annotate_source_info(
      std::string_view, std::string_view, uint32_t, uint32_t) const;
  std::string to_string() const;
};

struct PublicEventInputRef {
  EventPortRef port{};
  constexpr PublicEventInputRef() = default;
  constexpr explicit PublicEventInputRef(EventPortRef port_)
      : port(std::move(port_)) {}
  constexpr operator EventPortRef() const { return port; }
  constexpr void _annotate_source_info(
      std::string_view declaration_identity,
      std::string_view file_path,
      uint32_t begin, uint32_t end) const;
};
} // namespace iv
