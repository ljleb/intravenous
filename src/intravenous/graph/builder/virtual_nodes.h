#pragma once

#include <intravenous/graph/builder/stored_node.h>
#include <intravenous/graph/builder/node_bundles.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace iv {
class GraphBuilderTopology;

using VirtualNodeHandle = size_t;

struct VirtualSamplePortMapping {
  std::string name{};
  size_t ordinal = 0;
  ChannelLayout channel_layout{};
  std::vector<PortId> concrete_ports{};
  bool operator==(VirtualSamplePortMapping const &) const = default;
};

struct VirtualEventPortMapping {
  std::string name{};
  size_t ordinal = 0;
  EventTypeId type = EventTypeId::empty;
  std::vector<PortId> concrete_ports{};
  bool operator==(VirtualEventPortMapping const &) const = default;
};

struct VirtualNodeRecord {
  std::string id{};
  std::vector<SourceInfo> source_infos{};
  // Membership is in builder-visible NodeBundles. Concrete ports below are
  // lowering details, not the authored membership relation.
  std::vector<NodeBundleHandle> node_bundle_handles{};
  std::vector<size_t> concrete_node_indices{};
  std::vector<VirtualSamplePortMapping> sample_inputs{};
  std::vector<VirtualSamplePortMapping> sample_outputs{};
  std::vector<VirtualEventPortMapping> event_inputs{};
  std::vector<VirtualEventPortMapping> event_outputs{};
};

// Authoritative nominal relation between an authored virtual node and its
// concrete builder-node members. ConcreteNode keeps only inverse handles.
class GraphBuilderVirtualNodes {
public:
  void attach_bundle_member(GraphBuilderTopology &topology,
                            GraphBuilderNodeBundles &node_bundles,
                            NodeBundleHandle node_bundle_handle,
                            std::string_view virtual_node_id,
                            SourceInfo const *source_info = nullptr);
  void attach_tiled_members(GraphBuilderTopology &topology,
                            GraphBuilderNodeBundles &node_bundles,
                            VirtualNodeRecord mapping,
                            SourceInfo const *source_info = nullptr);
  void import_child(GraphBuilderTopology &topology,
                    GraphBuilderNodeBundles &node_bundles,
                    GraphBuilderVirtualNodes const &child, size_t node_offset,
                    size_t node_bundle_offset);

  std::vector<VirtualNodeRecord> const &records() const;
  VirtualNodeRecord const &record(VirtualNodeHandle) const;
  std::vector<std::string> ids_for_bundle(NodeBundle const &) const;

private:
  VirtualNodeHandle get_or_create(std::string_view virtual_node_id);
  void attach_member(GraphBuilderTopology &, GraphBuilderNodeBundles &,
                     VirtualNodeHandle, NodeBundleHandle, SourceInfo const *);

  std::vector<VirtualNodeRecord> _records{};
  std::unordered_map<std::string, VirtualNodeHandle> _handles_by_id{};
};
} // namespace iv
