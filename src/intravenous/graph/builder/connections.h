#pragma once

#include <intravenous/graph/builder/port_refs.h>
#include <intravenous/channel_layout.h>

#include <span>
#include <string>
#include <vector>

namespace iv {
class GraphBuilderNodeBundles;
class GraphBuilderVirtualNodes;

struct GraphBuilderVacantSampleInput {
  NodeBundlePortId target{};
  std::string virtual_node_id{};
  size_t member_ordinal = 0;
  InputConfig config{};
};
struct GraphBuilderVacantEventInput {
  NodeBundlePortId target{};
  std::string virtual_node_id{};
  size_t member_ordinal = 0;
  EventInputConfig config{};
};
struct GraphBuilderVacantInputs {
  std::vector<GraphBuilderVacantSampleInput> sample{};
  std::vector<GraphBuilderVacantEventInput> event{};
};

struct GraphBuilderVirtualSampleInput {
  NodeBundlePortId target{};
  std::string virtual_node_id{};
  size_t member_ordinal = 0;
  InputConfig config{};
  bool has_existing_connection = false;
};
struct GraphBuilderVirtualEventInput {
  NodeBundlePortId target{};
  std::string virtual_node_id{};
  size_t member_ordinal = 0;
  EventInputConfig config{};
  bool has_existing_connection = false;
};
struct GraphBuilderVirtualInputs {
  std::vector<GraphBuilderVirtualSampleInput> sample{};
  std::vector<GraphBuilderVirtualEventInput> event{};
};

struct GraphBuilderVirtualSampleInputChannel {
  std::vector<SampleInputChannelId> targets{};
  bool has_existing_connection = false;
};
struct GraphBuilderVirtualSampleInputFamily {
  std::string virtual_node_id{};
  size_t member_ordinal = 0;
  size_t family_ordinal = 0;
  std::string family_name{};
  InputConfig config{};
  ChannelTypeId channel_type = ChannelTypeId::mono;
  std::vector<GraphBuilderVirtualSampleInputChannel> channels{};
};
struct GraphBuilderVirtualSampleInputFamilies {
  std::vector<GraphBuilderVirtualSampleInputFamily> families{};
};

struct GraphBuilderVirtualSampleOutput {
  NodeBundlePortId source{};
  std::string virtual_node_id{};
  size_t member_ordinal = 0;
  OutputConfig config{};
  bool has_existing_downstream_connection = false;
};
struct GraphBuilderVirtualEventOutput {
  NodeBundlePortId source{};
  std::string virtual_node_id{};
  size_t member_ordinal = 0;
  EventOutputConfig config{};
  bool has_existing_downstream_connection = false;
};
struct GraphBuilderVirtualOutputs {
  std::vector<GraphBuilderVirtualSampleOutput> sample{};
  std::vector<GraphBuilderVirtualEventOutput> event{};
};

struct GraphBuilderVirtualSampleOutputChannel {
  std::vector<SampleOutputChannelId> sources{};
  bool has_existing_downstream_connection = false;
};
struct GraphBuilderVirtualSampleOutputFamily {
  std::string virtual_node_id{};
  size_t member_ordinal = 0;
  size_t family_ordinal = 0;
  std::string family_name{};
  OutputConfig config{};
  ChannelTypeId channel_type = ChannelTypeId::mono;
  std::vector<GraphBuilderVirtualSampleOutputChannel> channels{};
};
struct GraphBuilderVirtualSampleOutputFamilies {
  std::vector<GraphBuilderVirtualSampleOutputFamily> families{};
};

struct AuthoredSampleConnection {
  ChannelTypeId source_type = ChannelTypeId::mono;
  std::vector<SampleOutputChannelId> source_channels{};
  ChannelTypeId target_type = ChannelTypeId::mono;
  std::vector<SampleInputChannelId> target_channels{};
  bool operator==(AuthoredSampleConnection const&) const = default;
};

struct AuthoredEventConnection {
  EventTypeId source_type = EventTypeId::empty;
  std::vector<EventOutputPortId> sources{};
  EventTypeId target_type = EventTypeId::empty;
  std::vector<EventInputPortId> targets{};
  bool operator==(AuthoredEventConnection const&) const = default;
};

class GraphBuilderConnections {
public:
  void record_authored_sample_connection(AuthoredSampleConnection);
  std::span<AuthoredSampleConnection const> authored_sample_connections() const;
  void record_authored_event_connection(AuthoredEventConnection);
  std::span<AuthoredEventConnection const> authored_event_connections() const;

  bool sample_input_is_connected(SampleInputChannelId) const;
  bool sample_output_is_connected(SampleOutputChannelId) const;
  bool event_input_is_connected(EventInputPortId) const;
  bool event_output_is_connected(EventOutputPortId) const;

  GraphBuilderVacantInputs collect_vacant_inputs(
      GraphBuilderNodeBundles const&, GraphBuilderVirtualNodes const&) const;
  GraphBuilderVirtualInputs collect_virtual_inputs(
      GraphBuilderNodeBundles const&, GraphBuilderVirtualNodes const&) const;
  GraphBuilderVirtualSampleInputFamilies collect_virtual_sample_input_families(
      GraphBuilderNodeBundles const&, GraphBuilderVirtualNodes const&) const;
  GraphBuilderVirtualOutputs collect_virtual_outputs(
      GraphBuilderNodeBundles const&, GraphBuilderVirtualNodes const&) const;
  GraphBuilderVirtualSampleOutputFamilies collect_virtual_sample_output_families(
      GraphBuilderNodeBundles const&, GraphBuilderVirtualNodes const&) const;

  void import_child(GraphBuilderConnections const&, size_t node_bundle_offset);

private:
  std::vector<AuthoredSampleConnection> _authored_sample_connections{};
  std::vector<AuthoredEventConnection> _authored_event_connections{};
};
} // namespace iv
