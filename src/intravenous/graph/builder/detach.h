#pragma once

#include <intravenous/graph/builder/port_refs.h>

#include <span>
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
  size_t reserve_child_offset(GraphBuilderDetach const& child);
  void import_child(GraphBuilderDetach const&, size_t node_bundle_offset,
                    size_t detach_id_offset);
  bool reader_output_exists(ChannelTypeId,
      std::span<SampleOutputChannelId const>) const;
  AuthoredDetachedSamplePortInfo const* info_for_source(
      ChannelTypeId, std::span<SampleOutputChannelId const>) const;
  size_t allocate_detach_id();
  void record_detached_source(AuthoredDetachedSamplePortInfo);
  std::span<AuthoredDetachedSamplePortInfo const> authored_infos() const;

private:
  size_t _next_detach_id = 0;
  std::vector<AuthoredDetachedSamplePortInfo> _authored_infos{};
};
} // namespace iv
