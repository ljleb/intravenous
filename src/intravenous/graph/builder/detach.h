#pragma once

#include <intravenous/graph/builder/port_refs.h>
#include <intravenous/graph/types.h>

#include <limits>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace iv {
    // Authored detach identity. The source and reader are semantic channels;
    // topology addresses are created only in the completion copy.
    struct AuthoredDetachedSamplePortInfo {
        size_t detach_id = 0;
        ChannelTypeId source_type = ChannelTypeId::mono;
        std::vector<SampleOutputChannelId> source_channels {};
        NodeBundleHandle writer_bundle = 0;
        NodeBundleHandle reader_bundle = 0;
        SampleOutputChannelId reader_channel {};
        size_t loop_extra_latency = 1;
    };

    // Completion/finalizer compatibility projection.
    struct DetachedSamplePortInfo {
        size_t detach_id = 0;
        TopologyPortId original_source {};
        size_t writer_node = std::numeric_limits<size_t>::max();
        TopologyPortId reader_output {};
        size_t loop_extra_latency = 1;
    };

    class GraphBuilderDetach {
    public:
        size_t reserve_child_offset(GraphBuilderDetach const& child);
        void import_child(
            GraphBuilderDetach const& child,
            size_t child_node_bundle_offset,
            size_t child_detach_offset);
        bool reader_output_exists(
            ChannelTypeId source_type,
            std::span<SampleOutputChannelId const> source_channels) const;
        AuthoredDetachedSamplePortInfo const* info_for_source(
            ChannelTypeId source_type,
            std::span<SampleOutputChannelId const> source_channels) const;
        size_t allocate_detach_id();
        void record_detached_source(AuthoredDetachedSamplePortInfo info);
        std::span<AuthoredDetachedSamplePortInfo const> authored_infos() const;
        void clear_materialized();
        void record_materialized_detached_source(
            TopologyPortId source, DetachedSamplePortInfo info);
        template<class Fn>
        void for_each_info(Fn&& fn) const
        {
            for (auto const& [source, info] : _materialized_info_by_source) {
                fn(source, info);
            }
        }
        template<class Fn>
        void for_each_reader_output(Fn&& fn) const
        {
            for (TopologyPortId const output : _materialized_reader_outputs) {
                fn(output);
            }
        }

    private:
        size_t _next_detach_id = 0;
        std::vector<AuthoredDetachedSamplePortInfo> _authored_infos {};
        std::unordered_map<TopologyPortId, DetachedSamplePortInfo>
            _materialized_info_by_source {};
        std::unordered_set<TopologyPortId> _materialized_reader_outputs {};
    };
}
