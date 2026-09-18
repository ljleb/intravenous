#pragma once

#include <intravenous/graph/builder/port_refs.h>

#include <algorithm>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

namespace iv {
struct ConfiguredDetachedSamplePortInfo {
  size_t detach_id = 0;
  ChannelTypeId source_type = ChannelTypeId::mono;
  std::vector<SampleOutputChannelId> source_channels{};
  NodeBundleHandle writer_bundle = 0;
  NodeBundleHandle reader_bundle = 0;
  SampleOutputChannelId reader_channel{};
  size_t loop_extra_latency = 1;
};

struct ConfiguredDetachedEventPortInfo {
  size_t detach_id = 0;
  EventTypeId source_type = EventTypeId::empty;
  std::vector<EventOutputPortId> sources{};
  NodeBundleHandle writer_bundle = 0;
  NodeBundleHandle reader_bundle = 0;
  EventOutputPortId reader_port{};
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
  constexpr bool reader_output_exists(EventTypeId,
      std::span<EventOutputPortId const>) const;
  constexpr ConfiguredDetachedSamplePortInfo const* info_for_source(
      ChannelTypeId, std::span<SampleOutputChannelId const>) const;
  constexpr ConfiguredDetachedEventPortInfo const* info_for_source(
      EventTypeId, std::span<EventOutputPortId const>) const;
  constexpr size_t allocate_detach_id();
  constexpr void record_detached_source(ConfiguredDetachedSamplePortInfo);
  constexpr void record_detached_source(ConfiguredDetachedEventPortInfo);
  constexpr std::span<ConfiguredDetachedSamplePortInfo const>
      configured_infos() const;
  constexpr std::span<ConfiguredDetachedEventPortInfo const>
      configured_event_infos() const;
  constexpr size_t next_detach_id() const { return _next_detach_id; }
  static constexpr GraphBuilderDetach from_configured_infos(
      size_t next_detach_id,
      std::span<ConfiguredDetachedSamplePortInfo const>,
      std::span<ConfiguredDetachedEventPortInfo const> = {});

private:
  size_t _next_detach_id = 0;
  std::vector<ConfiguredDetachedSamplePortInfo> _configured_infos{};
  std::vector<ConfiguredDetachedEventPortInfo> _configured_event_infos{};
};
} // namespace iv
namespace iv {
namespace {
constexpr bool same_source(
    ConfiguredDetachedSamplePortInfo const& info, ChannelTypeId type,
    std::span<SampleOutputChannelId const> channels) {
  return info.source_type == type && std::ranges::equal(info.source_channels, channels);
}
constexpr bool same_source(
    ConfiguredDetachedEventPortInfo const& info, EventTypeId type,
    std::span<EventOutputPortId const> sources) {
  return info.source_type == type && std::ranges::equal(info.sources, sources);
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
  return std::ranges::any_of(_configured_infos,
      [&](auto const& info) { return info.reader_channel == channels.front(); });
}
constexpr bool GraphBuilderDetach::reader_output_exists(
    EventTypeId type, std::span<EventOutputPortId const> sources) const {
  if (sources.size() != 1) return false;
  return std::ranges::any_of(_configured_event_infos,
      [&](auto const& info) {
        return info.source_type == type && info.reader_port == sources.front();
      });
}
constexpr ConfiguredDetachedSamplePortInfo const*
GraphBuilderDetach::info_for_source(
    ChannelTypeId type, std::span<SampleOutputChannelId const> channels) const {
  auto const it = std::ranges::find_if(_configured_infos,
      [&](auto const& info) { return same_source(info, type, channels); });
  return it == _configured_infos.end() ? nullptr : &*it;
}
constexpr ConfiguredDetachedEventPortInfo const*
GraphBuilderDetach::info_for_source(
    EventTypeId type, std::span<EventOutputPortId const> sources) const {
  auto const it = std::ranges::find_if(_configured_event_infos,
      [&](auto const& info) { return same_source(info, type, sources); });
  return it == _configured_event_infos.end() ? nullptr : &*it;
}
constexpr size_t GraphBuilderDetach::allocate_detach_id() {
  return _next_detach_id++;
}
constexpr void GraphBuilderDetach::record_detached_source(
    ConfiguredDetachedSamplePortInfo info) {
  _configured_infos.push_back(std::move(info));
}
constexpr void GraphBuilderDetach::record_detached_source(
    ConfiguredDetachedEventPortInfo info) {
  _configured_event_infos.push_back(std::move(info));
}
constexpr std::span<ConfiguredDetachedSamplePortInfo const>
GraphBuilderDetach::configured_infos() const {
  return _configured_infos;
}
constexpr std::span<ConfiguredDetachedEventPortInfo const>
GraphBuilderDetach::configured_event_infos() const {
  return _configured_event_infos;
}
constexpr GraphBuilderDetach GraphBuilderDetach::from_configured_infos(
    size_t next_detach_id,
    std::span<ConfiguredDetachedSamplePortInfo const> infos,
    std::span<ConfiguredDetachedEventPortInfo const> event_infos) {
  GraphBuilderDetach result;
  result._next_detach_id = next_detach_id;
  result._configured_infos.assign(infos.begin(), infos.end());
  result._configured_event_infos.assign(event_infos.begin(), event_infos.end());
  return result;
}
constexpr void GraphBuilderDetach::import_child(
    GraphBuilderDetach const& child, size_t bundle_offset,
    size_t detach_offset) {
  for (auto info : child._configured_infos) {
    info.detach_id += detach_offset;
    for (auto& channel : info.source_channels) channel.bundle += bundle_offset;
    info.writer_bundle += bundle_offset;
    info.reader_bundle += bundle_offset;
    info.reader_channel.bundle += bundle_offset;
    _configured_infos.push_back(std::move(info));
  }
  for (auto info : child._configured_event_infos) {
    info.detach_id += detach_offset;
    for (auto& source : info.sources) source.bundle += bundle_offset;
    info.writer_bundle += bundle_offset;
    info.reader_bundle += bundle_offset;
    info.reader_port.bundle += bundle_offset;
    _configured_event_infos.push_back(std::move(info));
  }
}
} // namespace iv
