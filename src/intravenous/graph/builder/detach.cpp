#include <intravenous/graph/builder/detach.h>

#include <algorithm>
#include <ranges>

namespace iv {
namespace {
bool same_source(
    AuthoredDetachedSamplePortInfo const& info,
    ChannelTypeId source_type,
    std::span<SampleOutputChannelId const> source_channels)
{
    return info.source_type == source_type
        && std::ranges::equal(info.source_channels, source_channels);
}
} // namespace

size_t GraphBuilderDetach::reserve_child_offset(GraphBuilderDetach const& child)
{
    size_t const child_detach_offset = _next_detach_id;
    _next_detach_id += child._next_detach_id;
    return child_detach_offset;
}

bool GraphBuilderDetach::reader_output_exists(
    ChannelTypeId source_type,
    std::span<SampleOutputChannelId const> source_channels) const
{
    if (source_type != ChannelTypeId::mono || source_channels.size() != 1) {
        return false;
    }
    return std::ranges::any_of(
        _authored_infos,
        [&](AuthoredDetachedSamplePortInfo const& info) {
            return info.reader_channel == source_channels.front();
        });
}

AuthoredDetachedSamplePortInfo const* GraphBuilderDetach::info_for_source(
    ChannelTypeId source_type,
    std::span<SampleOutputChannelId const> source_channels) const
{
    auto const it = std::ranges::find_if(
        _authored_infos,
        [&](AuthoredDetachedSamplePortInfo const& info) {
            return same_source(info, source_type, source_channels);
        });
    return it == _authored_infos.end() ? nullptr : &*it;
}

size_t GraphBuilderDetach::allocate_detach_id()
{
    return _next_detach_id++;
}

void GraphBuilderDetach::record_detached_source(
    AuthoredDetachedSamplePortInfo info)
{
    _authored_infos.push_back(std::move(info));
}

std::span<AuthoredDetachedSamplePortInfo const>
GraphBuilderDetach::authored_infos() const
{
    return _authored_infos;
}

void GraphBuilderDetach::clear_materialized()
{
    _materialized_info_by_source.clear();
    _materialized_reader_outputs.clear();
}

void GraphBuilderDetach::record_materialized_detached_source(
    TopologyPortId source, DetachedSamplePortInfo info)
{
    _materialized_reader_outputs.insert(info.reader_output);
    _materialized_info_by_source.emplace(source, std::move(info));
}

void GraphBuilderDetach::import_child(
    GraphBuilderDetach const& child,
    size_t child_node_bundle_offset,
    size_t child_detach_offset)
{
    for (auto info : child._authored_infos) {
        info.detach_id += child_detach_offset;
        for (auto& channel : info.source_channels) {
            channel.bundle += child_node_bundle_offset;
        }
        info.writer_bundle += child_node_bundle_offset;
        info.reader_bundle += child_node_bundle_offset;
        info.reader_channel.bundle += child_node_bundle_offset;
        _authored_infos.push_back(std::move(info));
    }
}
}
