#pragma once

#include <intravenous/graph/builder/port_refs.h>
#include <intravenous/graph/types.h>

#include <unordered_map>
#include <unordered_set>

namespace iv {
    using DetachedSamplePortInfo = DetachedInfo;

    class GraphBuilderDetach {
    public:
        size_t reserve_child_offset(GraphBuilderDetach const& child);
        void import_child(GraphBuilderDetach const& child, size_t child_node_offset, size_t child_detach_offset);
        bool reader_output_exists(PortId source) const;
        DetachedSamplePortInfo const* info_for_source(PortId source) const;
        size_t allocate_detach_id();
        void record_detached_source(PortId source, DetachedSamplePortInfo info);
        template<class Fn>
        void for_each_info(Fn&& fn) const
        {
            for (auto const& [source, info] : _info_by_source) {
                fn(source, info);
            }
        }
        template<class Fn>
        void for_each_reader_output(Fn&& fn) const
        {
            for (PortId const output : _reader_outputs) {
                fn(output);
            }
        }

    private:
        size_t _next_detach_id = 0;
        std::unordered_map<PortId, DetachedSamplePortInfo> _info_by_source {};
        std::unordered_set<PortId> _reader_outputs {};
    };
}
