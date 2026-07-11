#pragma once

#include <intravenous/graph/builder/identity.h>
#include <intravenous/graph/builder/node_refs.h>
#include <intravenous/graph/builder/topology.h>

#include <concepts>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace iv {
    class GraphBuilder;

    struct ScopedSubgraph {
        size_t start_node_index = 0;
        std::string kind {};
        std::vector<InputConfig> input_configs {};
        std::vector<size_t> input_placeholder_nodes {};
        std::vector<OutputConfig> output_configs {};
        std::vector<PortId> output_sources {};
        std::vector<EventInputConfig> event_input_configs {};
        std::vector<size_t> event_input_placeholder_nodes {};
        std::vector<EventOutputConfig> event_output_configs {};
        std::vector<PortId> event_output_sources {};
        bool outputs_defined = false;
        bool event_outputs_defined = false;
    };

    class SubgraphScopeManager {
    public:
        bool active() const;
        ScopedSubgraph& current();
        void begin(size_t start_node_index, std::string_view kind);
        ScopedSubgraph finish();
        void abandon_top();
        SamplePortRef add_scope_sample_input(
            GraphBuilder&,
            GraphBuilderTopology&,
            std::string_view name,
            Sample default_value,
            bool has_name
        );
        EventPortRef add_scope_event_input(
            GraphBuilder&,
            GraphBuilderTopology&,
            std::string_view name,
            EventTypeId type,
            bool has_name
        );
        void define_sample_outputs(
            std::span<OutputRefConfig const> refs,
            GraphBuilder const&,
            GraphBuilderTopology const&,
            GraphBuilderIdentity const&
        );
        template<class LiftSample>
        void define_sample_outputs_from_named_refs(
            GraphBuilder const&,
            GraphBuilderTopology const&,
            GraphBuilderIdentity const&,
            LiftSample&& lift_sample,
            std::span<NamedRef const> refs
        );
        void define_event_outputs(
            std::span<EventOutputRefConfig const> refs,
            GraphBuilder const&,
            GraphBuilderTopology const&,
            GraphBuilderIdentity const&,
            std::span<EventInputConfig const> graph_event_inputs
        );
        template<class Fn>
        NodeRef run(GraphBuilder& builder, GraphBuilderTopology& topology, Fn&& fn, std::string_view kind);
        size_t current_start_node_index() const;

    private:
        NodeRef finalize_scope(GraphBuilder& builder, GraphBuilderTopology& topology, ScopedSubgraph scope);
        std::vector<ScopedSubgraph> _stack {};
    };

    template<class Fn>
    NodeRef SubgraphScopeManager::run(GraphBuilder& builder, GraphBuilderTopology& topology, Fn&& fn, std::string_view kind)
    {
        static_assert(
            std::invocable<Fn&> && !std::invocable<Fn&, GraphBuilder&>,
            "iv::GraphBuilder::subgraph(Fn) requires a zero-argument callback; use embed_subgraph(GraphBuilder) for isolated nested builders"
        );

        begin(topology.node_count(), kind);

        struct ScopePopGuard {
            SubgraphScopeManager* subgraphs;
            bool active = true;

            ~ScopePopGuard()
            {
                if (active) {
                    subgraphs->abandon_top();
                }
            }
        } guard { this };

        std::forward<Fn>(fn)();

        ScopedSubgraph scope = finish();
        guard.active = false;

        return finalize_scope(builder, topology, std::move(scope));
    }

    template<class LiftSample>
    void SubgraphScopeManager::define_sample_outputs_from_named_refs(
        GraphBuilder const& builder,
        GraphBuilderTopology const& topology,
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
            output_refs.push_back(OutputRefConfig{
                .ref = lift_sample(ref),
                .config = OutputConfig{ .name = std::string(ref.name) },
            });
        }
        define_sample_outputs(std::span<OutputRefConfig const>(output_refs.data(), output_refs.size()), builder, topology, identity);
    }
}
