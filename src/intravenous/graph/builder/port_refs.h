#pragma once

#include <intravenous/graph/error.h>
#include <intravenous/graph/port_ids.h>
#include <intravenous/channel_ports.h>

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

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
template<class ChannelType>
class TypedPublicSampleInputRef;

// A sample expression is exactly the ordered semantic source channels that
// participate in an authored connection. It has no execution/topology address.
struct SamplePortRef {
  GraphBuilder* graph_builder{};
  ChannelTypeId channel_type = ChannelTypeId::mono;
  // The semantic channel list belongs to the builder graph. Keeping only a
  // handle here makes copied and typed DSL references cheap and is the same
  // ownership model the module-facing builder-session ABI uses.
  size_t handle = std::numeric_limits<size_t>::max();

  constexpr SamplePortRef() = default;
  constexpr SamplePortRef(SamplePortRef const&) = default;
  constexpr SamplePortRef(SamplePortRef&&) noexcept = default;
  explicit SamplePortRef(
      GraphBuilder&, NodeBundlePortId bundle_port);
  explicit SamplePortRef(
      GraphBuilder&, ChannelTypeId,
      std::span<SampleOutputChannelId const>);

  constexpr SamplePortRef& operator=(SamplePortRef const&) = default;
  constexpr SamplePortRef& operator=(SamplePortRef&&) noexcept = default;
  SamplePortRef _clone_handle() const;
  std::span<SampleOutputChannelId const> channels() const;
  SamplePortRef select_channel(size_t channel) const;
  // An erased port can still use a named channel member.  The member supplies
  // the requested static type; this validates that request against the
  // runtime semantic channel type before selecting the channel.
  template<class Member>
  auto operator[](Member) const
  requires requires {
    typename std::remove_cvref_t<Member>::channel_type;
    std::remove_cvref_t<Member>::channel_ordinal;
  };
  SamplePortRef detach(size_t loop_extra_latency = 1) const;
  void _annotate_source_info(
      std::string_view, std::string_view, uint32_t, uint32_t) const;
  std::string to_string() const;
};

// Module-facing references need value semantics: they are copied by
// typed projections and passed through the ABI, while the session owns the
// variable-length expression data.
static_assert(std::is_trivially_copyable_v<SamplePortRef>);

namespace details {
SamplePortRef make_tiled_sample_port(
    ChannelTypeId, SamplePortRef const*, size_t);
}

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
  auto operator[](Channel) const
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
  explicit TypedSamplePortChannelRef(
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

public:
  using channel_type = ChannelType;

  constexpr TypedSamplePortTileRef() = default;
  explicit TypedSamplePortTileRef(
      std::array<SamplePortRef, ChannelType::channel_count> members)
      : _port(details::make_tiled_sample_port(
            ChannelTypeTraits<ChannelType>::id,
            members.data(), members.size())) {}
  explicit TypedSamplePortTileRef(SamplePortRef port)
      : _port(std::move(port)) {
    if (!_port.graph_builder ||
        _port.channel_type != ChannelTypeTraits<ChannelType>::id ||
        _port.channels().size() != ChannelType::channel_count)
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
  auto operator[](Member) const
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
auto TypedSamplePortTileRef<ChannelType>::operator[](Member) const
requires std::same_as<typename std::remove_cvref_t<Member>::channel_type,
                      ChannelType> {
  using MemberType = std::remove_cvref_t<Member>;
  return TypedSamplePortTileChannelRef<ChannelType, MemberType>{
      _port.select_channel(MemberType::channel_ordinal)};
}

template<class ChannelType>
template<class Channel>
auto TypedSamplePortRef<ChannelType>::operator[](Channel) const
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

  void _annotate_source_info(
      std::string_view declaration_identity,
      std::string_view file_path,
      uint32_t begin, uint32_t end) const;
};

// A named public input has a compile-time channel type when it was declared
// through GraphBuilder::input<Name, ChannelType>.  Keep its public-input
// identity so source annotations still attach to the graph interface, while
// exposing the same channel-selection API as a TypedSamplePortRef.
template<class ChannelType>
class TypedPublicSampleInputRef {
  PublicSampleInputRef _input;

public:
  using channel_type = ChannelType;

  constexpr TypedPublicSampleInputRef() = default;
  constexpr explicit TypedPublicSampleInputRef(PublicSampleInputRef input)
      : _input(std::move(input)) {}

  constexpr operator SamplePortRef() const { return _input.port; }
  constexpr SamplePortRef const& erased() const { return _input.port; }
  constexpr void _annotate_source_info(
      std::string_view id, std::string_view file,
      uint32_t begin, uint32_t end) const {
    _input._annotate_source_info(id, file, begin, end);
  }

  template<class Channel>
  auto operator[](Channel channel) const
  requires std::same_as<typename std::remove_cvref_t<Channel>::channel_type,
                        ChannelType>
  {
    return TypedSamplePortRef<ChannelType>{_input.port}[channel];
  }
};

template<class Member>
auto SamplePortRef::operator[](Member member) const
requires requires {
  typename std::remove_cvref_t<Member>::channel_type;
  std::remove_cvref_t<Member>::channel_ordinal;
} {
  using ChannelType = typename std::remove_cvref_t<Member>::channel_type;
  if (!graph_builder ||
      channel_type != ChannelTypeTraits<ChannelType>::id ||
      channels().size() != ChannelType::channel_count) {
    details::error("sample port does not match the requested channel type");
  }
  return TypedSamplePortRef<ChannelType>{*this}[member];
}

// Event expressions mirror SamplePortRef: only semantic source ports survive
// authoring. A tiled source is one TiledNodeBundle event port; lowering expands
// it to member execution ports when compiling the graph.
struct EventPortRef {
  GraphBuilder* graph_builder{};
  EventTypeId type = EventTypeId::empty;
  // Like SamplePortRef, this is a session-owned semantic expression, not a
  // container embedded in every DSL value.
  size_t handle = std::numeric_limits<size_t>::max();

  constexpr EventPortRef() = default;
  constexpr EventPortRef(EventPortRef const&) = default;
  constexpr EventPortRef(EventPortRef&&) noexcept = default;
  explicit EventPortRef(GraphBuilder&, NodeBundlePortId bundle_port);
  explicit EventPortRef(
      GraphBuilder&, EventTypeId, std::span<EventOutputPortId const>);

  constexpr EventPortRef& operator=(EventPortRef const&) = default;
  constexpr EventPortRef& operator=(EventPortRef&&) noexcept = default;
  std::span<EventOutputPortId const> sources() const;
  void _annotate_source_info(
      std::string_view, std::string_view, uint32_t, uint32_t) const;
  std::string to_string() const;
};

static_assert(std::is_trivially_copyable_v<EventPortRef>);

struct PublicEventInputRef {
  EventPortRef port{};
  constexpr PublicEventInputRef() = default;
  constexpr explicit PublicEventInputRef(EventPortRef port_)
      : port(std::move(port_)) {}
  constexpr operator EventPortRef() const { return port; }
  void _annotate_source_info(
      std::string_view declaration_identity,
      std::string_view file_path,
      uint32_t begin, uint32_t end) const;
};
} // namespace iv
