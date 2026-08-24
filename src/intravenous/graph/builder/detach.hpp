#pragma once

#include <intravenous/graph/builder/port_refs.h>

#include <algorithm>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

namespace iv {
struct AuthoredDetachedSamplePortInfo {
  size_t detach_id = 0;
  ChannelTypeId source_type = ChannelTypeId::mono;
  std::vector<SampleOutputChannelId> source_channels{};
  NodeBundleHandle writer_bundle = 0;
  NodeBundleHandle reader_bundle = 0;
  SampleOutputChannelId reader_channel{};
  size_t loop_extra_latency = 1;
};

class GraphBuilderDetach {
public:
  constexpr size_t reserve_child_offset(GraphBuilderDetach const& child);
  constexpr void import_child(
      GraphBuilderDetach const&, size_t node_bundle_offset,
      size_t detach_id_offset);
  constexpr bool reader_output_exists(ChannelTypeId,
      std::span<SampleOutputChannelId const>) const;
  constexpr AuthoredDetachedSamplePortInfo const* info_for_source(
      ChannelTypeId, std::span<SampleOutputChannelId const>) const;
  constexpr size_t allocate_detach_id();
  constexpr void record_detached_source(AuthoredDetachedSamplePortInfo);
  constexpr std::span<AuthoredDetachedSamplePortInfo const>
      authored_infos() const;

private:
  size_t _next_detach_id = 0;
  std::vector<AuthoredDetachedSamplePortInfo> _authored_infos{};
};
} // namespace iv
namespace iv {
namespace {
constexpr bool same_source(
    AuthoredDetachedSamplePortInfo const& info, ChannelTypeId type,
    std::span<SampleOutputChannelId const> channels) {
  return info.source_type == type && std::ranges::equal(info.source_channels, channels);
}
}
constexpr size_t GraphBuilderDetach::reserve_child_offset(
    GraphBuilderDetach const& child) {
  size_t const offset = _next_detach_id;
  _next_detach_id += child._next_detach_id;
  return offset;
}
constexpr bool GraphBuilderDetach::reader_output_exists(
    ChannelTypeId type, std::span<SampleOutputChannelId const> channels) const {
  if (type != ChannelTypeId::mono || channels.size() != 1) return false;
  return std::ranges::any_of(_authored_infos,
      [&](auto const& info) { return info.reader_channel == channels.front(); });
}
constexpr AuthoredDetachedSamplePortInfo const*
GraphBuilderDetach::info_for_source(
    ChannelTypeId type, std::span<SampleOutputChannelId const> channels) const {
  auto const it = std::ranges::find_if(_authored_infos,
      [&](auto const& info) { return same_source(info, type, channels); });
  return it == _authored_infos.end() ? nullptr : &*it;
}
constexpr size_t GraphBuilderDetach::allocate_detach_id() {
  return _next_detach_id++;
}
constexpr void GraphBuilderDetach::record_detached_source(
    AuthoredDetachedSamplePortInfo info) {
  _authored_infos.push_back(std::move(info));
}
constexpr std::span<AuthoredDetachedSamplePortInfo const>
GraphBuilderDetach::authored_infos() const {
  return _authored_infos;
}
constexpr void GraphBuilderDetach::import_child(
    GraphBuilderDetach const& child, size_t bundle_offset,
    size_t detach_offset) {
  for (auto info : child._authored_infos) {
    info.detach_id += detach_offset;
    for (auto& channel : info.source_channels) channel.bundle += bundle_offset;
    info.writer_bundle += bundle_offset;
    info.reader_bundle += bundle_offset;
    info.reader_channel.bundle += bundle_offset;
    _authored_infos.push_back(std::move(info));
  }
}
} // namespace iv
