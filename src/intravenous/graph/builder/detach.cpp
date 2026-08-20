#include <intravenous/graph/builder/detach.h>

#include <algorithm>
#include <ranges>

namespace iv {
namespace {
bool same_source(AuthoredDetachedSamplePortInfo const& info, ChannelTypeId type,
                 std::span<SampleOutputChannelId const> channels) {
  return info.source_type == type && std::ranges::equal(info.source_channels, channels);
}
}
size_t GraphBuilderDetach::reserve_child_offset(GraphBuilderDetach const& child) {
  size_t const offset = _next_detach_id;
  _next_detach_id += child._next_detach_id;
  return offset;
}
bool GraphBuilderDetach::reader_output_exists(
    ChannelTypeId type, std::span<SampleOutputChannelId const> channels) const {
  if (type != ChannelTypeId::mono || channels.size() != 1) return false;
  return std::ranges::any_of(_authored_infos,
      [&](auto const& info) { return info.reader_channel == channels.front(); });
}
AuthoredDetachedSamplePortInfo const* GraphBuilderDetach::info_for_source(
    ChannelTypeId type, std::span<SampleOutputChannelId const> channels) const {
  auto const it = std::ranges::find_if(_authored_infos,
      [&](auto const& info) { return same_source(info, type, channels); });
  return it == _authored_infos.end() ? nullptr : &*it;
}
size_t GraphBuilderDetach::allocate_detach_id() { return _next_detach_id++; }
void GraphBuilderDetach::record_detached_source(AuthoredDetachedSamplePortInfo info) {
  _authored_infos.push_back(std::move(info));
}
std::span<AuthoredDetachedSamplePortInfo const> GraphBuilderDetach::authored_infos() const {
  return _authored_infos;
}
void GraphBuilderDetach::import_child(GraphBuilderDetach const& child,
                                      size_t bundle_offset,
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
