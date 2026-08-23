#pragma once

#include <intravenous/graph/compiler.h>
#include <intravenous/graph/builder/identity.h>
#include <intravenous/graph/builder/node_call.h>
#include <intravenous/graph/builder/node_bundles.h>
#include <intravenous/graph/builder/output_refs.h>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace iv {
class GraphBuilder;

class GraphBuilderPublicPorts {
public:
  explicit GraphBuilderPublicPorts(NodeBundleHandle boundary) : _boundary(boundary) {}
  NodeBundleHandle boundary_handle() const { return _boundary; }

  SamplePortRef add_sample_input(GraphBuilder&, GraphBuilderNodeBundles&,
      std::string_view, Sample, std::optional<Sample>, std::optional<Sample>);
  EventPortRef add_event_input(GraphBuilder&, GraphBuilderNodeBundles&,
      std::string_view, EventTypeId);
  bool sample_outputs_defined() const;
  void define_sample_outputs(GraphBuilder&, GraphBuilderNodeBundles&,
      GraphBuilderIdentity const&, std::span<OutputRefConfig const>);
  void define_event_outputs(GraphBuilder&, GraphBuilderNodeBundles&,
      GraphBuilderIdentity const&, std::span<EventOutputRefConfig const>);

  template<class LiftSample, class... Refs>
  void define_sample_outputs_from_args(GraphBuilder&, GraphBuilderNodeBundles&,
      GraphBuilderIdentity const&, LiftSample&&, Refs&&...);
  template<class LiftSample>
  void define_sample_outputs_from_named_refs(GraphBuilder&, GraphBuilderNodeBundles&,
      GraphBuilderIdentity const&, LiftSample&&, std::span<NamedRef const>);
  template<class... Refs>
  void define_event_outputs_from_args(GraphBuilder&, GraphBuilderNodeBundles&,
      GraphBuilderIdentity const&, Refs&&...);

  std::span<InputConfig const> sample_inputs(GraphBuilderNodeBundles const&) const;
  std::span<EventInputConfig const> event_inputs(GraphBuilderNodeBundles const&) const;
  std::span<OutputConfig const> sample_outputs(GraphBuilderNodeBundles const&) const;
  std::span<EventOutputConfig const> event_outputs(GraphBuilderNodeBundles const&) const;
  GraphBuilderPublicSamplePortFamilies sample_input_families(GraphBuilderNodeBundles const&) const;
  GraphBuilderPublicSamplePortFamilies sample_output_families(GraphBuilderNodeBundles const&) const;
  std::vector<GraphBuilderPublicEventInput> collected_event_inputs(GraphBuilderNodeBundles const&) const;
  std::vector<GraphBuilderPublicEventOutput> collected_event_outputs(GraphBuilderNodeBundles const&) const;

  void annotate_sample_input_source_info(size_t, std::string_view,
      std::string_view, uint32_t, uint32_t);
  std::span<SourceInfo const> sample_input_source_infos(size_t) const;
  void annotate_event_input_source_info(size_t, std::string_view,
      std::string_view, uint32_t, uint32_t);
  std::span<SourceInfo const> event_input_source_infos(size_t) const;
  void annotate_sample_output_source_info(size_t, SourceInfo);
  void annotate_event_output_source_info(size_t, SourceInfo);

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

template<class LiftSample, class... Refs>
void GraphBuilderPublicPorts::define_sample_outputs_from_args(
    GraphBuilder& builder, GraphBuilderNodeBundles& bundles,
    GraphBuilderIdentity const& identity, LiftSample&& lift_sample, Refs&&... refs) {
  std::vector<OutputRefConfig> out;
  out.reserve(sizeof...(Refs));
  constexpr bool require_names = sizeof...(Refs) > 1;
  auto source_config = [&](SamplePortRef const& source) {
    if (auto logical = bundles.sample_output_port_for_channels(source.channel_type, source.channels))
      return bundles.resolve_sample_output(*logical).config;
    if (source.channels.empty()) details::error("sample output source has no logical address");
    return OutputConfig{.channel_layout = {.channel_type = source.channel_type,
                                           .sample_layout = SampleStreamLayout::planar}};
  };
  auto public_config = [&](SamplePortRef const& source, std::string_view name) {
    auto config = source_config(source); config.name = std::string(name);
    config.channel_layout.sample_layout = SampleStreamLayout::planar; return config;
  };
  auto append = [&](auto&& ref) {
    using Ref = std::remove_cvref_t<decltype(ref)>;
    if constexpr (details::is_channel_named_arg_v<Ref>) {
      using C = typename Ref::channel_type;
      out.push_back({.ref = lift_sample(ref.value),
                     .config = {.name = std::string(Ref::name.view()),
                                .channel_layout = {.channel_type = ChannelTypeTraits<C>::id,
                                                   .sample_layout = SampleStreamLayout::planar}},
                     .public_member = {.family_name = std::string(Ref::name.view()),
                                       .channel_type = ChannelTypeTraits<C>::id,
                                       .whole_stream = true},
                     .target_channel_ordinal = Ref::channel_ordinal});
    } else if constexpr (details::is_default_channel_named_arg_v<Ref>) {
      using C = typename Ref::channel_type;
      out.push_back({.ref = lift_sample(ref.value),
                     .config = {.name = "main",
                                .channel_layout = {.channel_type = ChannelTypeTraits<C>::id,
                                                   .sample_layout = SampleStreamLayout::planar}},
                     .public_member = {.family_name = "main",
                                       .channel_type = ChannelTypeTraits<C>::id,
                                       .whole_stream = true},
                     .target_channel_ordinal = Ref::channel_ordinal});
    } else if constexpr (details::is_named_arg_v<Ref>) {
      if constexpr (Ref::name.view().starts_with("__"))
        details::error("builder " + identity.value + ": generated channel assignments are not public outputs");
      auto source = lift_sample(ref.value); auto config = public_config(source, Ref::name.view());
      auto type = config.channel_layout.channel_type;
      out.push_back({.ref = source, .config = std::move(config),
                     .public_member = {.family_name = std::string(Ref::name.view()),
                                       .channel_type = type, .whole_stream = true}});
    } else {
      if constexpr (require_names) details::error("builder " + identity.value + ": outputs(...) requires names when exposing more than one sample output");
      auto source = lift_sample(std::forward<decltype(ref)>(ref));
      out.push_back({.ref = source, .config = public_config(source, {})});
    }
  };
  (append(std::forward<Refs>(refs)), ...);
  define_sample_outputs(builder, bundles, identity, out);
}

template<class LiftSample>
void GraphBuilderPublicPorts::define_sample_outputs_from_named_refs(
    GraphBuilder& builder, GraphBuilderNodeBundles& bundles,
    GraphBuilderIdentity const& identity, LiftSample&& lift_sample,
    std::span<NamedRef const> refs) {
  std::vector<OutputRefConfig> out; out.reserve(refs.size());
  for (auto const& ref : refs) {
    if (refs.size() > 1 && ref.name.empty()) details::error("builder " + identity.value + ": outputs(...) requires names when exposing more than one sample output");
    auto source = lift_sample(ref);
    auto logical = bundles.sample_output_port_for_channels(source.channel_type, source.channels);
    auto config = logical ? bundles.resolve_sample_output(*logical).config
                          : OutputConfig{.channel_layout = {.channel_type = source.channel_type,
                                                           .sample_layout = SampleStreamLayout::planar}};
    config.name = std::string(ref.name); config.channel_layout.sample_layout = SampleStreamLayout::planar;
    out.push_back({.ref = source, .config = std::move(config)});
  }
  define_sample_outputs(builder, bundles, identity, out);
}

template<class... Refs>
void GraphBuilderPublicPorts::define_event_outputs_from_args(
    GraphBuilder& builder, GraphBuilderNodeBundles& bundles,
    GraphBuilderIdentity const& identity, Refs&&... refs) {
  std::vector<EventOutputRefConfig> out; out.reserve(sizeof...(Refs));
  constexpr bool require_names = sizeof...(Refs) > 1;
  auto append = [&](auto&& ref) {
    using Ref = std::remove_cvref_t<decltype(ref)>;
    if constexpr (details::is_named_arg_v<Ref>)
      out.push_back({.ref = static_cast<EventPortRef>(ref.value),
                     .config = {.name = std::string(Ref::name.view())}});
    else {
      if constexpr (require_names) details::error("builder " + identity.value + ": event_outputs(...) requires names when exposing more than one event output");
      out.push_back({.ref = static_cast<EventPortRef>(ref), .config = {}});
    }
  };
  (append(std::forward<Refs>(refs)), ...);
  define_event_outputs(builder, bundles, identity, out);
}
} // namespace iv
