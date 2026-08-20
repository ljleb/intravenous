#include <intravenous/graph/builder/public_ports.h>
#include <intravenous/graph/builder.h>

#include <algorithm>
#include <limits>
#include <type_traits>

namespace iv {
namespace {
template<class Config>
GraphBuilderPublicSamplePortFamilies collect_sample_port_families(
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

SamplePortRef GraphBuilderPublicPorts::add_sample_input(
    GraphBuilder& builder, GraphBuilderNodeBundles& bundles, std::string_view name,
    Sample value, std::optional<Sample> min, std::optional<Sample> max) {
  auto ordinal = bundles.bundle(_boundary).append_boundary_sample_input({
      .name = std::string(name), .default_value = value,
      .min = min.value_or(-std::numeric_limits<Sample::storage>::infinity()),
      .max = max.value_or(std::numeric_limits<Sample::storage>::infinity())});
  _sample_input_source_infos.emplace_back();
  return SamplePortRef(builder, NodeBundlePortId{_boundary, PortKind::sample, ordinal});
}
EventPortRef GraphBuilderPublicPorts::add_event_input(
    GraphBuilder& builder, GraphBuilderNodeBundles& bundles, std::string_view name,
    EventTypeId type) {
  auto ordinal = bundles.bundle(_boundary).append_boundary_event_input(
      {.name = std::string(name), .type = type});
  _event_input_source_infos.emplace_back();
  return EventPortRef(builder, NodeBundlePortId{_boundary, PortKind::event, ordinal});
}
bool GraphBuilderPublicPorts::sample_outputs_defined() const { return _sample_outputs_defined; }

void GraphBuilderPublicPorts::define_sample_outputs(
    GraphBuilder& builder, GraphBuilderNodeBundles& bundles,
    GraphBuilderIdentity const& identity, std::span<OutputRefConfig const> refs) {
  _last_sample_output_port_ordinals.clear();
  bool require_names = refs.size() > 1;
  for (size_t i = 0; i < refs.size(); ++i) {
    auto const& ref = refs[i].ref; auto const& config = refs[i].config;
    if (!ref.graph_builder) details::error("builder " + identity.value + ": outputs(...): empty SamplePortRef");
    if (ref.graph_builder != &builder) details::error("builder " + identity.value + ": outputs(...): SamplePortRef belongs to another builder");
    if (require_names && config.name.empty()) details::error("builder " + identity.value + ": outputs(...) requires names when exposing more than one sample output");
    auto existing = !refs[i].public_member.family_name.empty()
      ? std::find_if(_sample_output_members.begin(), _sample_output_members.end(), [&](auto const& m) {
          auto const& n = refs[i].public_member; return m.family_name == n.family_name && m.channel_type == n.channel_type && m.channel_index == n.channel_index && m.whole_stream == n.whole_stream; })
      : _sample_output_members.end();
    size_t output = existing == _sample_output_members.end()
      ? bundles.bundle(_boundary).boundary_sample_outputs().size()
      : static_cast<size_t>(existing - _sample_output_members.begin());
    if (existing == _sample_output_members.end()) {
      IV_ASSERT(bundles.bundle(_boundary).append_boundary_sample_output(config) == output, "public output metadata mismatch");
      _sample_output_members.push_back(refs[i].public_member); _sample_output_source_infos.emplace_back();
    }
    NodeBundlePortId const target{_boundary, PortKind::sample, output};
    if (refs[i].target_channel_ordinal) {
      auto channels = bundles.sample_input_channels(target); auto channel = *refs[i].target_channel_ordinal;
      if (channel >= channels.size()) details::error("public sample output channel ordinal is out of bounds");
      builder.record_authored_sample_connection(channels[channel], ref);
    } else builder.record_authored_sample_connection(target, ref);
    _last_sample_output_port_ordinals.push_back(output);
  }
  _sample_outputs_defined = true;
}

void GraphBuilderPublicPorts::define_event_outputs(
    GraphBuilder& builder, GraphBuilderNodeBundles& bundles,
    GraphBuilderIdentity const& identity, std::span<EventOutputRefConfig const> refs) {
  auto& boundary = bundles.bundle(_boundary); boundary.clear_boundary_event_outputs();
  _event_output_source_infos.resize(refs.size()); bool require_names = refs.size() > 1;
  for (size_t i = 0; i < refs.size(); ++i) {
    auto const& ref = refs[i].ref;
    if (!ref.graph_builder) details::error("builder " + identity.value + ": event_outputs(...): empty EventPortRef");
    if (ref.graph_builder != &builder) details::error("builder " + identity.value + ": event_outputs(...): EventPortRef belongs to another builder");
    if (require_names && refs[i].config.name.empty()) details::error("builder " + identity.value + ": event_outputs(...) requires names when exposing more than one event output");
    auto config = refs[i].config; config.type = ref.type;
    auto output = boundary.append_boundary_event_output(std::move(config));
    builder.record_authored_event_connection({_boundary, PortKind::event, output}, ref);
  }
}

std::span<InputConfig const> GraphBuilderPublicPorts::sample_inputs(GraphBuilderNodeBundles const& b) const { return b.bundle(_boundary).boundary_sample_inputs(); }
std::span<EventInputConfig const> GraphBuilderPublicPorts::event_inputs(GraphBuilderNodeBundles const& b) const { return b.bundle(_boundary).boundary_event_inputs(); }
std::span<OutputConfig const> GraphBuilderPublicPorts::sample_outputs(GraphBuilderNodeBundles const& b) const { return b.bundle(_boundary).boundary_sample_outputs(); }
std::span<EventOutputConfig const> GraphBuilderPublicPorts::event_outputs(GraphBuilderNodeBundles const& b) const { return b.bundle(_boundary).boundary_event_outputs(); }

void GraphBuilderPublicPorts::annotate_sample_input_source_info(size_t i, std::string_view id, std::string_view file, uint32_t begin, uint32_t end) {
  if (i >= _sample_input_source_infos.size() || id.empty()) return;
  SourceInfo info{.declaration_identity = std::string(id), .span = {.file_path = std::string(file), .begin = begin, .end = end}};
  auto& infos = _sample_input_source_infos[i]; if (!std::ranges::contains(infos, info)) infos.push_back(std::move(info));
}
std::span<SourceInfo const> GraphBuilderPublicPorts::sample_input_source_infos(size_t i) const { return i < _sample_input_source_infos.size() ? std::span<SourceInfo const>(_sample_input_source_infos[i]) : std::span<SourceInfo const>{}; }
void GraphBuilderPublicPorts::annotate_event_input_source_info(size_t i, std::string_view id, std::string_view file, uint32_t begin, uint32_t end) {
  if (i >= _event_input_source_infos.size() || id.empty()) return;
  SourceInfo info{.declaration_identity = std::string(id), .span = {.file_path = std::string(file), .begin = begin, .end = end}};
  auto& infos = _event_input_source_infos[i]; if (!std::ranges::contains(infos, info)) infos.push_back(std::move(info));
}
std::span<SourceInfo const> GraphBuilderPublicPorts::event_input_source_infos(size_t i) const { return i < _event_input_source_infos.size() ? std::span<SourceInfo const>(_event_input_source_infos[i]) : std::span<SourceInfo const>{}; }

GraphBuilderPublicSamplePortFamilies GraphBuilderPublicPorts::sample_input_families(GraphBuilderNodeBundles const& b) const {
  auto configs = sample_inputs(b); std::vector<PublicSamplePortMember> members;
  for (auto const& c : configs) members.push_back({.channel_type = c.channel_layout.channel_type});
  auto result = collect_sample_port_families(configs, members, true);
  for (auto& family : result.families) for (auto& channel : family.channels) for (auto ordinal : channel.port_ordinals)
    for (auto const& info : sample_input_source_infos(ordinal)) if (!std::ranges::contains(family.source_infos, info)) family.source_infos.push_back(info);
  return result;
}
GraphBuilderPublicSamplePortFamilies GraphBuilderPublicPorts::sample_output_families(GraphBuilderNodeBundles const& b) const {
  auto result = collect_sample_port_families(sample_outputs(b), _sample_output_members, false);
  for (auto& family : result.families) for (auto& channel : family.channels) for (auto ordinal : channel.port_ordinals)
    for (auto const& info : _sample_output_source_infos[ordinal]) { if (!std::ranges::contains(channel.source_infos, info)) channel.source_infos.push_back(info); if (!std::ranges::contains(family.source_infos, info)) family.source_infos.push_back(info); }
  return result;
}
std::vector<GraphBuilderPublicEventInput> GraphBuilderPublicPorts::collected_event_inputs(GraphBuilderNodeBundles const& b) const {
  std::vector<GraphBuilderPublicEventInput> r; auto c = event_inputs(b); for (size_t i=0;i<c.size();++i) r.push_back({i,c[i]}); return r;
}
std::vector<GraphBuilderPublicEventOutput> GraphBuilderPublicPorts::collected_event_outputs(GraphBuilderNodeBundles const& b) const {
  std::vector<GraphBuilderPublicEventOutput> r; auto c = event_outputs(b); for (size_t i=0;i<c.size();++i) r.push_back({i,c[i], i<_event_output_source_infos.size()?_event_output_source_infos[i]:std::vector<SourceInfo>{}}); return r;
}
void GraphBuilderPublicPorts::annotate_sample_output_source_info(size_t i, SourceInfo info) { if (i>=_last_sample_output_port_ordinals.size()) return; auto& v=_sample_output_source_infos[_last_sample_output_port_ordinals[i]]; if(!std::ranges::contains(v,info))v.push_back(std::move(info)); }
void GraphBuilderPublicPorts::annotate_event_output_source_info(size_t i, SourceInfo info) { if(i>=_event_output_source_infos.size())return; auto&v=_event_output_source_infos[i]; if(!std::ranges::contains(v,info))v.push_back(std::move(info)); }
} // namespace iv
