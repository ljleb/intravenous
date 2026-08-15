#pragma once

#include <intravenous/graph/compiler.h>
#include <intravenous/graph/builder/identity.h>
#include <intravenous/graph/builder/node_call.h>
#include <intravenous/graph/builder/output_refs.h>
#include <intravenous/graph/builder/topology.h>
#include <intravenous/basic_nodes/routing.h>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace iv {
    class GraphBuilder;

    template<class ChannelType>
    SamplePortRef make_channel_pack(GraphBuilder&, size_t channel, SamplePortRef source);

    struct GraphBuilderPublicSamplePortChannel {
        std::vector<size_t> port_ordinals {};
        std::vector<SourceInfo> source_infos {};
    };

    struct GraphBuilderPublicSamplePortFamily {
        size_t family_ordinal = 0;
        std::string family_name {};
        InputConfig input_config {};
        OutputConfig output_config {};
        ChannelTypeId channel_type = ChannelTypeId::mono;
        std::vector<GraphBuilderPublicSamplePortChannel> channels {};
        std::vector<SourceInfo> source_infos {};
    };

    struct GraphBuilderPublicSamplePortFamilies {
        std::vector<GraphBuilderPublicSamplePortFamily> families {};
    };

    struct GraphBuilderPublicEventInput {
        size_t port_ordinal = 0;
        EventInputConfig config {};
    };

    struct GraphBuilderPublicEventOutput {
        size_t port_ordinal = 0;
        EventOutputConfig config {};
        std::vector<SourceInfo> source_infos {};
    };

    class GraphBuilderPublicPorts {
    public:
        SamplePortRef add_sample_input(
            GraphBuilder&,
            std::string_view name,
            Sample default_value,
            std::optional<Sample> min,
            std::optional<Sample> max);
        EventPortRef add_event_input(GraphBuilder&, std::string_view name, EventTypeId type);
        bool sample_outputs_defined() const;
        void define_sample_outputs(
            GraphBuilder const&,
            GraphBuilderTopology&,
            GraphBuilderIdentity const&,
            std::span<OutputRefConfig const> refs
        );
        void define_event_outputs(
            GraphBuilder const&,
            GraphBuilderTopology&,
            GraphBuilderIdentity const&,
            std::span<EventOutputRefConfig const> refs
        );
        template<class LiftSample, class... Refs>
        void define_sample_outputs_from_args(
            GraphBuilder&,
            GraphBuilderTopology&,
            GraphBuilderIdentity const&,
            LiftSample&& lift_sample,
            Refs&&... refs
        );
        template<class LiftSample>
        void define_sample_outputs_from_named_refs(
            GraphBuilder&,
            GraphBuilderTopology&,
            GraphBuilderIdentity const&,
            LiftSample&& lift_sample,
            std::span<NamedRef const> refs
        );
        template<class... Refs>
        void define_event_outputs_from_args(
            GraphBuilder&,
            GraphBuilderTopology&,
            GraphBuilderIdentity const&,
            Refs&&... refs
        );
        std::span<InputConfig const> sample_inputs() const;
        std::span<EventInputConfig const> event_inputs() const;
        std::span<OutputConfig const> sample_outputs() const;
        std::span<EventOutputConfig const> event_outputs() const;
        GraphBuilderPublicSamplePortFamilies sample_input_families() const;
        GraphBuilderPublicSamplePortFamilies sample_output_families() const;
        std::vector<GraphBuilderPublicEventInput> collected_event_inputs() const;
        std::vector<GraphBuilderPublicEventOutput> collected_event_outputs() const;
        void annotate_sample_input_source_info(
            size_t port_ordinal,
            std::string_view declaration_identity,
            std::string_view file_path,
            uint32_t begin,
            uint32_t end);
        std::span<SourceInfo const> sample_input_source_infos(size_t port_ordinal) const;
        void annotate_event_input_source_info(size_t port_ordinal, std::string_view declaration_identity,
            std::string_view file_path, uint32_t begin, uint32_t end);
        std::span<SourceInfo const> event_input_source_infos(size_t port_ordinal) const;
        void annotate_sample_output_source_info(size_t port_ordinal, SourceInfo info);
        void annotate_event_output_source_info(size_t port_ordinal, SourceInfo info);

    private:
        std::vector<InputConfig> _sample_inputs {};
        std::vector<std::vector<SourceInfo>> _sample_input_source_infos {};
        std::vector<EventInputConfig> _event_inputs {};
        std::vector<std::vector<SourceInfo>> _event_input_source_infos {};
        std::vector<OutputConfig> _sample_outputs {};
        // Index-aligned with _sample_outputs. This is public-declaration
        // metadata, not a property of a concrete port configuration.
        std::vector<PublicSamplePortMember> _sample_output_members {};
        std::vector<size_t> _last_sample_output_port_ordinals {};
        std::vector<std::vector<SourceInfo>> _sample_output_source_infos {};
        std::vector<EventOutputConfig> _event_outputs {};
        std::vector<std::vector<SourceInfo>> _event_output_source_infos {};
        bool _sample_outputs_defined = false;
    };

    template<class LiftSample, class... Refs>
    void GraphBuilderPublicPorts::define_sample_outputs_from_args(
        GraphBuilder& builder,
        GraphBuilderTopology& topology,
        GraphBuilderIdentity const& identity,
        LiftSample&& lift_sample,
        Refs&&... refs
    )
    {
        std::vector<OutputRefConfig> output_refs;
        output_refs.reserve(sizeof...(Refs));
        constexpr bool require_names = (sizeof...(Refs) > 1);
        auto const source_config = [&](SamplePortRef const& source) {
            if (source.node_index == GRAPH_ID) {
                return OutputConfig{};
            }
            return topology.node(source.node_index).outputs()[source.output_port];
        };
        auto const public_output_config = [&](SamplePortRef const& source, std::string_view name) {
            auto config = source_config(source);
            config.name = std::string(name);
            // Public graph outputs use the canonical lane-facing layout.
            // Contributors in another layout are adapted by their edge plan.
            config.channel_layout.sample_layout = SampleStreamLayout::planar;
            return config;
        };
        auto const append_ref = [&](auto&& ref) {
            using RefT = std::remove_cvref_t<decltype(ref)>;
            if constexpr (details::is_channel_named_arg_v<RefT>) {
                using ChannelType = typename RefT::channel_type;
                auto packed = make_channel_pack<ChannelType>(
                    builder, RefT::channel_ordinal, lift_sample(ref.value));
                output_refs.push_back(OutputRefConfig{
                    .ref = packed,
                    .config = OutputConfig{
                        .name = std::string(RefT::name.view()),
                        .channel_layout = ChannelLayout{
                            .channel_type = ChannelTypeTraits<ChannelType>::id,
                            .sample_layout = SampleStreamLayout::planar,
                        },
                    },
                    .public_member = PublicSamplePortMember{
                        .family_name = std::string(RefT::name.view()),
                        .channel_type = ChannelTypeTraits<ChannelType>::id,
                        .whole_stream = true,
                    },
                });
            } else if constexpr (details::is_default_channel_named_arg_v<RefT>) {
                using ChannelType = typename RefT::channel_type;
                auto packed = make_channel_pack<ChannelType>(
                    builder, RefT::channel_ordinal, lift_sample(ref.value));
                output_refs.push_back(OutputRefConfig{
                    .ref = packed,
                    .config = OutputConfig{
                        .name = "main",
                        .channel_layout = ChannelLayout{
                            .channel_type = ChannelTypeTraits<ChannelType>::id,
                            .sample_layout = SampleStreamLayout::planar,
                        },
                    },
                    .public_member = PublicSamplePortMember{
                        .family_name = "main",
                        .channel_type = ChannelTypeTraits<ChannelType>::id,
                        .whole_stream = true,
                    },
                });
            } else if constexpr (details::is_named_arg_v<RefT>) {
                if constexpr (RefT::name.view().starts_with("__")) {
                    details::error(
                        "builder " + identity.value
                        + ": generated channel assignments are not public outputs; use \"name\"_P[channel] = value"
                    );
                }
                auto source = lift_sample(ref.value);
                auto config = public_output_config(source, RefT::name.view());
                auto const channel_type = config.channel_layout.channel_type;
                output_refs.push_back(OutputRefConfig{
                    .ref = source,
                    .config = std::move(config),
                    .public_member = PublicSamplePortMember{
                        .family_name = std::string(RefT::name.view()),
                        .channel_type = channel_type,
                        .whole_stream = true,
                    },
                });
            } else {
                if constexpr (require_names) {
                    details::error(
                        "builder " + identity.value
                        + ": outputs(...) requires names when exposing more than one sample output"
                    );
                } else {
                    auto source = lift_sample(std::forward<decltype(ref)>(ref));
                    output_refs.push_back(OutputRefConfig{
                        .ref = source,
                        .config = public_output_config(source, {}),
                    });
                }
            }
        };
        (append_ref(std::forward<Refs>(refs)), ...);
        define_sample_outputs(builder, topology, identity, std::span<OutputRefConfig const>(output_refs.data(), output_refs.size()));
    }

    template<class LiftSample>
    void GraphBuilderPublicPorts::define_sample_outputs_from_named_refs(
        GraphBuilder& builder,
        GraphBuilderTopology& topology,
        GraphBuilderIdentity const& identity,
        LiftSample&& lift_sample,
        std::span<NamedRef const> refs
    )
    {
        std::vector<OutputRefConfig> output_refs;
        output_refs.reserve(refs.size());
        bool const require_names = refs.size() > 1;
        for (auto const& ref : refs) {
            if (require_names && ref.name.empty()) {
                details::error(
                    "builder " + identity.value
                    + ": outputs(...) requires names when exposing more than one sample output"
                );
            }
            auto source = lift_sample(ref);
            auto config = source.node_index == GRAPH_ID
                ? OutputConfig{}
                : topology.node(source.node_index).outputs()[source.output_port];
            config.name = std::string(ref.name);
            config.channel_layout.sample_layout = SampleStreamLayout::planar;
            output_refs.push_back(OutputRefConfig{ .ref = source, .config = std::move(config) });
        }
        define_sample_outputs(builder, topology, identity, std::span<OutputRefConfig const>(output_refs.data(), output_refs.size()));
    }

    template<class... Refs>
    void GraphBuilderPublicPorts::define_event_outputs_from_args(
        GraphBuilder& builder,
        GraphBuilderTopology& topology,
        GraphBuilderIdentity const& identity,
        Refs&&... refs
    )
    {
        std::vector<EventOutputRefConfig> output_refs;
        output_refs.reserve(sizeof...(Refs));
        constexpr bool require_names = (sizeof...(Refs) > 1);
        auto const append_ref = [&](auto&& ref) {
            using RefT = std::remove_cvref_t<decltype(ref)>;
            if constexpr (details::is_named_arg_v<RefT>) {
                output_refs.push_back(EventOutputRefConfig{
                    .ref = static_cast<EventPortRef>(ref.value),
                    .config = EventOutputConfig{ .name = std::string(RefT::name.view()) },
                });
            } else {
                if constexpr (require_names) {
                    details::error(
                        "builder " + identity.value + ": event_outputs(...) requires names when exposing more than one event output"
                    );
                } else {
                    output_refs.push_back(EventOutputRefConfig{
                        .ref = static_cast<EventPortRef>(ref),
                        .config = EventOutputConfig{},
                    });
                }
            }
        };
        (append_ref(std::forward<Refs>(refs)), ...);
        define_event_outputs(builder, topology, identity, std::span<EventOutputRefConfig const>(output_refs.data(), output_refs.size()));
    }
}
