#pragma once

#include "../compiler.h"
#include "identity.h"
#include "node_call.h"
#include "output_refs.h"

#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace iv {
    class GraphBuilder;
    class GraphBuilderTopology;

    class GraphBuilderPublicPorts {
    public:
        SamplePortRef add_sample_input(GraphBuilder&, std::string_view name, Sample default_value);
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

    private:
        std::vector<InputConfig> _sample_inputs {};
        std::vector<EventInputConfig> _event_inputs {};
        std::vector<OutputConfig> _sample_outputs {};
        std::vector<EventOutputConfig> _event_outputs {};
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
        if (sample_outputs_defined()) {
            details::error("outputs(...) was already called on builder " + identity.value);
        }

        std::vector<OutputRefConfig> output_refs;
        output_refs.reserve(sizeof...(Refs));
        constexpr bool require_names = (sizeof...(Refs) > 1);
        auto const append_ref = [&](auto&& ref) {
            using RefT = std::remove_cvref_t<decltype(ref)>;
            if constexpr (details::is_named_arg_v<RefT>) {
                output_refs.push_back(OutputRefConfig{
                    .ref = lift_sample(ref.value),
                    .config = OutputConfig{ .name = std::string(RefT::name.view()) },
                });
            } else {
                if constexpr (require_names) {
                    details::error(
                        "builder " + identity.value
                        + ": outputs(...) requires names when exposing more than one sample output"
                    );
                } else {
                    output_refs.push_back(OutputRefConfig{
                        .ref = lift_sample(std::forward<decltype(ref)>(ref)),
                        .config = OutputConfig{},
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
        if (sample_outputs_defined()) {
            details::error("outputs(...) was already called on builder " + identity.value);
        }

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
            output_refs.push_back(OutputRefConfig{
                .ref = lift_sample(ref),
                .config = OutputConfig{ .name = std::string(ref.name) },
            });
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
