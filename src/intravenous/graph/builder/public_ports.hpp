#pragma once

#include <intravenous/graph/build_types.h>
#include <intravenous/graph/error.h>
#include <intravenous/graph/builder/identity.h>
#include <intravenous/graph/builder/node_bundles.hpp>
#include <intravenous/graph/builder/output_refs.h>

#include <algorithm>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace iv {
class GraphBuilderState;

struct ConfiguredPublicPortsRecord {
  NodeBundleHandle boundary = 0;
  std::vector<std::vector<SourceInfo>> sample_input_source_infos{};
  std::vector<std::vector<SourceInfo>> event_input_source_infos{};
  std::vector<PublicSamplePortMember> sample_output_members{};
  std::vector<size_t> last_sample_output_port_ordinals{};
  std::vector<std::vector<SourceInfo>> sample_output_source_infos{};
  std::vector<std::vector<SourceInfo>> event_output_source_infos{};
  bool sample_outputs_defined = false;
};

class GraphBuilderPublicPorts {
public:
  constexpr explicit GraphBuilderPublicPorts(NodeBundleHandle boundary)
      : _boundary(boundary) {}
  constexpr NodeBundleHandle boundary_handle() const { return _boundary; }

  SamplePortRef add_sample_input(GraphBuilderState&, GraphBuilderNodeBundles&,
      std::string_view, Sample, std::optional<Sample>, std::optional<Sample>);
  SamplePortRef add_sample_input(
      GraphBuilderState&, GraphBuilderNodeBundles&, std::string_view,
      ChannelLayout, Sample, std::optional<Sample>, std::optional<Sample>);
  EventPortRef add_event_input(GraphBuilderState&, GraphBuilderNodeBundles&,
      std::string_view, EventTypeId);
  constexpr bool sample_outputs_defined() const;
  constexpr void define_sample_outputs(GraphBuilderState&, GraphBuilderNodeBundles&,
      GraphBuilderIdentity const&, std::span<OutputRefConfig const>);
  constexpr void define_event_outputs(GraphBuilderState&, GraphBuilderNodeBundles&,
      GraphBuilderIdentity const&, std::span<EventOutputRefConfig const>);

  template<class LiftSample>
  constexpr void define_sample_outputs_from_named_refs(GraphBuilderState&, GraphBuilderNodeBundles&,
      GraphBuilderIdentity const&, LiftSample&&, std::span<NamedRef const>);

  constexpr std::span<InputConfig const> sample_inputs(
      GraphBuilderNodeBundles const&) const;
  constexpr std::span<EventInputConfig const> event_inputs(
      GraphBuilderNodeBundles const&) const;
  constexpr std::span<OutputConfig const> sample_outputs(
      GraphBuilderNodeBundles const&) const;
  constexpr std::span<EventOutputConfig const> event_outputs(
      GraphBuilderNodeBundles const&) const;
  constexpr GraphBuilderPublicSamplePortFamilies sample_input_families(
      GraphBuilderNodeBundles const&) const;
  constexpr GraphBuilderPublicSamplePortFamilies sample_output_families(
      GraphBuilderNodeBundles const&) const;
  constexpr std::vector<GraphBuilderPublicEventInput> collected_event_inputs(
      GraphBuilderNodeBundles const&) const;
  constexpr std::vector<GraphBuilderPublicEventOutput> collected_event_outputs(
      GraphBuilderNodeBundles const&) const;

  constexpr void annotate_sample_input_source_info(size_t, std::string_view,
      std::string_view, uint32_t, uint32_t);
  constexpr std::span<SourceInfo const> sample_input_source_infos(size_t) const;
  constexpr void annotate_event_input_source_info(size_t, std::string_view,
      std::string_view, uint32_t, uint32_t);
  constexpr std::span<SourceInfo const> event_input_source_infos(size_t) const;
  constexpr void annotate_sample_output_source_info(size_t, SourceInfo);
  constexpr void annotate_event_output_source_info(size_t, SourceInfo);
  constexpr ConfiguredPublicPortsRecord configured_record() const;
  static constexpr GraphBuilderPublicPorts from_configured_record(
      ConfiguredPublicPortsRecord const&);

private:
  NodeBundleHandle _boundary = 0;
  std::vector<std::vector<SourceInfo>> _sample_input_source_infos{};
  std::vector<std::vector<SourceInfo>> _event_input_source_infos{};
  std::vector<PublicSamplePortMember> _sample_output_members{};
  std::vector<size_t> _last_sample_output_port_ordinals{};
  std::vector<std::vector<SourceInfo>> _sample_output_source_infos{};
  std::vector<std::vector<SourceInfo>> _event_output_source_infos{};
  bool _sample_outputs_defined = false;
};

template<class LiftSample>
constexpr void GraphBuilderPublicPorts::define_sample_outputs_from_named_refs(
    GraphBuilderState& builder, GraphBuilderNodeBundles& bundles,
    GraphBuilderIdentity const& identity, LiftSample&& lift_sample,
    std::span<NamedRef const> refs) {
  std::vector<OutputRefConfig> out; out.reserve(refs.size());
  for (auto const& ref : refs) {
    if (refs.size() > 1 && ref.name.empty()) details::error("builder " + identity.value + ": outputs(...) requires names when exposing more than one sample output");
    auto source = lift_sample(ref);
    auto logical = bundles.sample_output_port_for_channels(
        source.channel_type, source.channels());
    auto config = logical ? bundles.resolve_sample_output(*logical).config
                          : OutputConfig{.channel_layout = {.channel_type = source.channel_type,
                                                           .sample_layout = SampleStreamLayout::planar}};
    config.name = std::string(ref.name); config.channel_layout.sample_layout = SampleStreamLayout::planar;
    out.push_back({
        .ref = source,
        .config = std::move(config),
        .public_member = {
            .family_name = std::string(ref.name),
            .channel_type = source.channel_type,
            .whole_stream = true,
        },
    });
  }
  define_sample_outputs(builder, bundles, identity, out);
}

} // namespace iv
namespace iv {
namespace {
template<class Config>
constexpr GraphBuilderPublicSamplePortFamilies collect_sample_port_families(
    std::span<Config const> configs, std::span<PublicSamplePortMember const> members,
    bool input) {
  IV_ASSERT(configs.size() == members.size(), "public sample port metadata must align with configs");
  GraphBuilderPublicSamplePortFamilies result;
  for (size_t ordinal = 0; ordinal < configs.size(); ++ordinal) {
    auto const& config = configs[ordinal]; auto const& member = members[ordinal];
    auto family = std::find_if(result.families.begin(), result.families.end(),
        [&](auto const& f) { return !member.family_name.empty() && f.family_name == member.family_name; });
    if (family == result.families.end()) {
      GraphBuilderPublicSamplePortFamily f{
          .family_ordinal = ordinal,
          .family_name = member.family_name.empty() ? config.name : member.family_name,
          .channel_type = member.channel_type,
          .channels = std::vector<GraphBuilderPublicSamplePortChannel>(channel_count(member.channel_type))};
      if constexpr (std::is_same_v<Config, InputConfig>) f.input_config = config;
      else f.output_config = config;
      result.families.push_back(std::move(f)); family = std::prev(result.families.end());
    }
    if (family->channel_type != member.channel_type)
      details::error(input ? "conflicting public sample input channel types" : "conflicting public sample output channel types");
    if (member.whole_stream) {
      for (auto& channel : family->channels) channel.port_ordinals.push_back(ordinal);
    } else {
      if (member.channel_index >= family->channels.size()) details::error("public sample channel ordinal out of bounds");
      if (input && !family->channels[member.channel_index].port_ordinals.empty()) details::error("duplicate public sample input channel contributor");
      family->channels[member.channel_index].port_ordinals.push_back(ordinal);
    }
  }
  return result;
}
}

constexpr bool GraphBuilderPublicPorts::sample_outputs_defined() const {
  return _sample_outputs_defined;
}

constexpr std::span<InputConfig const> GraphBuilderPublicPorts::sample_inputs(GraphBuilderNodeBundles const& b) const { return b.bundle(_boundary).boundary_sample_inputs(); }
constexpr std::span<EventInputConfig const> GraphBuilderPublicPorts::event_inputs(GraphBuilderNodeBundles const& b) const { return b.bundle(_boundary).boundary_event_inputs(); }
constexpr std::span<OutputConfig const> GraphBuilderPublicPorts::sample_outputs(GraphBuilderNodeBundles const& b) const { return b.bundle(_boundary).boundary_sample_outputs(); }
constexpr std::span<EventOutputConfig const> GraphBuilderPublicPorts::event_outputs(GraphBuilderNodeBundles const& b) const { return b.bundle(_boundary).boundary_event_outputs(); }

constexpr void GraphBuilderPublicPorts::annotate_sample_input_source_info(size_t i, std::string_view id, std::string_view file, uint32_t begin, uint32_t end) {
  if (i >= _sample_input_source_infos.size() || id.empty()) return;
  SourceInfo info{.declaration_identity = std::string(id), .span = {.file_path = std::string(file), .begin = begin, .end = end}};
  auto& infos = _sample_input_source_infos[i]; if (!std::ranges::contains(infos, info)) infos.push_back(std::move(info));
}
constexpr std::span<SourceInfo const> GraphBuilderPublicPorts::sample_input_source_infos(size_t i) const { return i < _sample_input_source_infos.size() ? std::span<SourceInfo const>(_sample_input_source_infos[i]) : std::span<SourceInfo const>{}; }
constexpr void GraphBuilderPublicPorts::annotate_event_input_source_info(size_t i, std::string_view id, std::string_view file, uint32_t begin, uint32_t end) {
  if (i >= _event_input_source_infos.size() || id.empty()) return;
  SourceInfo info{.declaration_identity = std::string(id), .span = {.file_path = std::string(file), .begin = begin, .end = end}};
  auto& infos = _event_input_source_infos[i]; if (!std::ranges::contains(infos, info)) infos.push_back(std::move(info));
}
constexpr std::span<SourceInfo const> GraphBuilderPublicPorts::event_input_source_infos(size_t i) const { return i < _event_input_source_infos.size() ? std::span<SourceInfo const>(_event_input_source_infos[i]) : std::span<SourceInfo const>{}; }

constexpr GraphBuilderPublicSamplePortFamilies GraphBuilderPublicPorts::sample_input_families(GraphBuilderNodeBundles const& b) const {
  auto configs = sample_inputs(b); std::vector<PublicSamplePortMember> members;
  for (auto const& c : configs) {
    members.push_back({
        .family_name = c.name,
        .channel_type = c.channel_layout.channel_type,
        .whole_stream = true,
    });
  }
  auto result = collect_sample_port_families(configs, members, true);
  for (auto& family : result.families) for (auto& channel : family.channels) for (auto ordinal : channel.port_ordinals)
    for (auto const& info : sample_input_source_infos(ordinal)) if (!std::ranges::contains(family.source_infos, info)) family.source_infos.push_back(info);
  return result;
}
constexpr GraphBuilderPublicSamplePortFamilies GraphBuilderPublicPorts::sample_output_families(GraphBuilderNodeBundles const& b) const {
  auto result = collect_sample_port_families(sample_outputs(b), _sample_output_members, false);
  for (auto& family : result.families) for (auto& channel : family.channels) for (auto ordinal : channel.port_ordinals)
    for (auto const& info : _sample_output_source_infos[ordinal]) { if (!std::ranges::contains(channel.source_infos, info)) channel.source_infos.push_back(info); if (!std::ranges::contains(family.source_infos, info)) family.source_infos.push_back(info); }
  return result;
}
constexpr std::vector<GraphBuilderPublicEventInput> GraphBuilderPublicPorts::collected_event_inputs(GraphBuilderNodeBundles const& b) const {
  std::vector<GraphBuilderPublicEventInput> r;
  auto configs = event_inputs(b);
  for (size_t i = 0; i < configs.size(); ++i) {
    auto infos = event_input_source_infos(i);
    r.push_back({
        .port_ordinal = i,
        .config = configs[i],
        .source_infos = {infos.begin(), infos.end()},
    });
  }
  return r;
}
constexpr std::vector<GraphBuilderPublicEventOutput> GraphBuilderPublicPorts::collected_event_outputs(GraphBuilderNodeBundles const& b) const {
  std::vector<GraphBuilderPublicEventOutput> r; auto c = event_outputs(b); for (size_t i=0;i<c.size();++i) r.push_back({i,c[i], i<_event_output_source_infos.size()?_event_output_source_infos[i]:std::vector<SourceInfo>{}}); return r;
}
constexpr void GraphBuilderPublicPorts::annotate_sample_output_source_info(size_t i, SourceInfo info) { if (i>=_last_sample_output_port_ordinals.size()) return; auto& v=_sample_output_source_infos[_last_sample_output_port_ordinals[i]]; if(!std::ranges::contains(v,info))v.push_back(std::move(info)); }
constexpr void GraphBuilderPublicPorts::annotate_event_output_source_info(size_t i, SourceInfo info) { if(i>=_event_output_source_infos.size())return; auto&v=_event_output_source_infos[i]; if(!std::ranges::contains(v,info))v.push_back(std::move(info)); }
constexpr ConfiguredPublicPortsRecord
GraphBuilderPublicPorts::configured_record() const {
  return {
      .boundary = _boundary,
      .sample_input_source_infos = _sample_input_source_infos,
      .event_input_source_infos = _event_input_source_infos,
      .sample_output_members = _sample_output_members,
      .last_sample_output_port_ordinals = _last_sample_output_port_ordinals,
      .sample_output_source_infos = _sample_output_source_infos,
      .event_output_source_infos = _event_output_source_infos,
      .sample_outputs_defined = _sample_outputs_defined,
  };
}
constexpr GraphBuilderPublicPorts
GraphBuilderPublicPorts::from_configured_record(
    ConfiguredPublicPortsRecord const& record) {
  GraphBuilderPublicPorts result(record.boundary);
  result._sample_input_source_infos = record.sample_input_source_infos;
  result._event_input_source_infos = record.event_input_source_infos;
  result._sample_output_members = record.sample_output_members;
  result._last_sample_output_port_ordinals =
      record.last_sample_output_port_ordinals;
  result._sample_output_source_infos = record.sample_output_source_infos;
  result._event_output_source_infos = record.event_output_source_infos;
  result._sample_outputs_defined = record.sample_outputs_defined;
  return result;
}
} // namespace iv
