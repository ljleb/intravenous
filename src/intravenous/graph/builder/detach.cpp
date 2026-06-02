#include "detach.h"

namespace iv {
size_t GraphBuilderDetach::reserve_child_offset(GraphBuilderDetach const& child)
{
    size_t const child_detach_offset = _next_detach_id;
    _next_detach_id += child._next_detach_id;
    return child_detach_offset;
}

bool GraphBuilderDetach::reader_output_exists(PortId source) const
{
    return _reader_outputs.contains(source);
}

DetachedSamplePortInfo const* GraphBuilderDetach::info_for_source(PortId source) const
{
    if (auto it = _info_by_source.find(source); it != _info_by_source.end()) {
        return &it->second;
    }
    return nullptr;
}

size_t GraphBuilderDetach::allocate_detach_id()
{
    return _next_detach_id++;
}

void GraphBuilderDetach::record_detached_source(PortId source, DetachedSamplePortInfo info)
{
    _reader_outputs.insert(info.reader_output);
    _info_by_source.emplace(source, std::move(info));
}

void GraphBuilderDetach::import_child(GraphBuilderDetach const& child, size_t child_node_offset, size_t child_detach_offset)
{
    auto remap_child_port = [&](PortId port) {
        if (port.node == GRAPH_ID) {
            return PortId{ child_node_offset - 1, port.port };
        }
        return PortId{ child_node_offset + port.node, port.port };
    };

    for (auto const& [source, info] : child._info_by_source) {
        _info_by_source.emplace(
            remap_child_port(source),
            DetachedSamplePortInfo{
                .detach_id = info.detach_id + child_detach_offset,
                .original_source = remap_child_port(info.original_source),
                .writer_node = child_node_offset + info.writer_node,
                .reader_output = remap_child_port(info.reader_output),
                .loop_extra_latency = info.loop_extra_latency,
            }
        );
    }

    for (PortId const reader_output : child._reader_outputs) {
        _reader_outputs.insert(remap_child_port(reader_output));
    }
}
}
