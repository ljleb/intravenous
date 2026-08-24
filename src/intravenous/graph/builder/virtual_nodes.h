#pragma once

#include <intravenous/graph/builder/stored_node.h>
#include <intravenous/graph/builder/node_bundles.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace iv {
using VirtualNodeHandle = size_t;

template<class ChannelId>
struct VirtualSamplePortMapping {
  std::string name{};
  size_t ordinal = 0;
  ChannelLayout channel_layout{};
  std::vector<ChannelId> channels{};
  bool operator==(VirtualSamplePortMapping const&) const = default;
};
using VirtualSampleInputPortMapping = VirtualSamplePortMapping<SampleInputChannelId>;
using VirtualSampleOutputPortMapping = VirtualSamplePortMapping<SampleOutputChannelId>;

struct VirtualEventPortMapping {
  std::string name{};
  size_t ordinal = 0;
  EventTypeId type = EventTypeId::empty;
  std::vector<NodeBundlePortId> node_bundle_ports{};
  bool operator==(VirtualEventPortMapping const&) const = default;
};

struct VirtualNodeRecord {
  std::string id{};
  std::string source_identity {};
  std::string type_identity {};
  std::vector<SourceInfo> source_infos{};
  std::vector<NodeBundleHandle> node_bundle_handles{};
  std::vector<VirtualSampleInputPortMapping> sample_inputs{};
  std::vector<VirtualSampleOutputPortMapping> sample_outputs{};
  std::vector<VirtualEventPortMapping> event_inputs{};
  std::vector<VirtualEventPortMapping> event_outputs{};
};

struct GraphBuilderVirtualSampleInputPort {
  VirtualPortId id{};
  InputConfig config{};
  std::vector<SampleInputChannelId> channels{};
  std::vector<NodeBundlePortId> node_bundle_ports{};
};
struct GraphBuilderVirtualSampleOutputPort {
  VirtualPortId id{};
  OutputConfig config{};
  std::vector<SampleOutputChannelId> channels{};
  std::vector<NodeBundlePortId> node_bundle_ports{};
};
struct GraphBuilderVirtualEventInputPort {
  VirtualPortId id{};
  EventInputConfig config{};
  std::vector<NodeBundlePortId> node_bundle_ports{};
};
struct GraphBuilderVirtualEventOutputPort {
  VirtualPortId id{};
  EventOutputConfig config{};
  std::vector<NodeBundlePortId> node_bundle_ports{};
};
struct GraphBuilderVirtualPorts {
  std::vector<GraphBuilderVirtualSampleInputPort> sample_inputs{};
  std::vector<GraphBuilderVirtualSampleOutputPort> sample_outputs{};
  std::vector<GraphBuilderVirtualEventInputPort> event_inputs{};
  std::vector<GraphBuilderVirtualEventOutputPort> event_outputs{};
};

class GraphBuilderVirtualNodes {
public:
  void attach_bundle_member(GraphBuilderNodeBundles&, NodeBundleHandle,
                            std::string_view virtual_node_id,
                            SourceInfo const* source_info = nullptr);
  void import_child(GraphBuilderNodeBundles&, GraphBuilderVirtualNodes const&,
                    size_t node_bundle_offset);

  std::vector<VirtualNodeRecord> const& records() const;
  GraphBuilderVirtualPorts ports(GraphBuilderNodeBundles const&) const;
  VirtualNodeRecord const& record(VirtualNodeHandle) const;
  std::vector<std::string> ids_for_bundle(NodeBundle const&) const;

private:
    VirtualNodeHandle get_or_create(std::string_view source_identity, std::string_view type_identity);
    void attach_member(GraphBuilderNodeBundles&, VirtualNodeHandle, NodeBundleHandle, SourceInfo const*);

    std::vector<VirtualNodeRecord> _records {};
    std::unordered_map<std::string, std::vector<VirtualNodeHandle>>
        _handles_by_source_identity {};
};
} // namespace iv
