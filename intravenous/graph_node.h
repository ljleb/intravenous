#pragma once

#include "graph/build_types.h"
#include "graph/wiring.h"

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace iv {
    struct Graph {
        std::string _graph_id;
        std::vector<GraphSccWrapper> _scc_wrappers;
        std::vector<GraphPortDataNode> _egress_port_data_nodes;
        decltype(GraphBuildArtifact::edges) _edges;
        std::vector<InputConfig> _public_inputs;
        std::vector<OutputConfig> _public_outputs;
        size_t _internal_latency;
        std::vector<std::string> _node_ids;

        explicit Graph(GraphBuildArtifact artifact) :
            _graph_id(std::move(artifact.graph_id)),
            _scc_wrappers(std::move(artifact.scc_wrappers)),
            _egress_port_data_nodes(make_egress_port_data_nodes(
                _graph_id,
                artifact.public_outputs.size(),
                artifact.public_output_buffer_plans
            )),
            _edges(std::move(artifact.edges)),
            _public_inputs(std::move(artifact.public_inputs)),
            _public_outputs(std::move(artifact.public_outputs)),
            _internal_latency(artifact.internal_latency),
            _node_ids(std::move(artifact.node_ids))
        {}

        struct State {
            std::span<std::span<std::byte>> scc_states;
            std::span<OutputPort> ingress_outputs;
            std::span<InputPort> egress_inputs;
        };

        static std::vector<GraphPortDataNode> make_egress_port_data_nodes(
            std::string const& graph_id,
            size_t num_outputs,
            std::span<PortBufferPlan const> output_buffer_plans
        )
        {
            IV_ASSERT(num_outputs == output_buffer_plans.size(), "graph egress port data must have one buffer plan per output");

            std::vector<GraphPortDataNode> port_data_nodes;
            port_data_nodes.reserve(num_outputs);
            for (size_t output_i = 0; output_i < num_outputs; ++output_i) {
                port_data_nodes.emplace_back(
                    graph_port_data_export_id(graph_id, output_i),
                    InputConfig{},
                    output_buffer_plans[output_i]
                );
            }
            return port_data_nodes;
        }

        std::string ingress_target_export_id(PortId target) const
        {
            if (target.node == GRAPH_ID) {
                return graph_port_data_export_id(_graph_id, target.port);
            }
            return port_data_export_id(_node_ids[target.node], target.port);
        }

        auto inputs() const
        {
            return std::span<InputConfig const>(_public_inputs);
        }

        auto outputs() const
        {
            return std::span<OutputConfig const>(_public_outputs);
        }

        auto num_inputs() const
        {
            return _public_inputs.size();
        }

        auto num_outputs() const
        {
            return _public_outputs.size();
        }

        size_t internal_latency() const
        {
            return _internal_latency;
        }

        size_t max_block_size() const
        {
            return MAX_BLOCK_SIZE;
        }

        void declare(DeclarationContext<Graph> const& ctx) const
        {
            auto const& state = ctx.state();
            ctx.local_array(state.ingress_outputs, num_inputs());
            ctx.nested_node_states(state.scc_states);
            for (auto const& scc : _scc_wrappers) {
                do_declare(scc, ctx);
            }
            for (auto const& port_data_node : _egress_port_data_nodes) {
                do_declare(port_data_node, ctx);
            }
            ctx.local_array(state.egress_inputs, num_outputs());
            for (size_t output_i = 0; output_i < num_outputs(); ++output_i) {
                ctx.require_export_array<SharedPortData>(
                    graph_port_data_export_id(_graph_id, output_i)
                );
            }
            for (GraphEdge const& edge : _edges) {
                if (edge.source.node == GRAPH_ID) {
                    ctx.require_export_array<SharedPortData>(
                        ingress_target_export_id(edge.target)
                    );
                }
            }
        }

        void initialize(InitializationContext<Graph> const& ctx) const
        {
            auto& state = ctx.state();
            for (size_t output_i = 0; output_i < num_outputs(); ++output_i) {
                auto egress_port_data = ctx.template resolve_exported_array_storage<SharedPortData>(
                    graph_port_data_export_id(_graph_id, output_i)
                );
                IV_ASSERT(!egress_port_data.empty(), "graph egress wiring must resolve the requested SharedPortData entry");
                std::construct_at(&state.egress_inputs[output_i], const_cast<SharedPortData&>(egress_port_data[0]), 0);
            }

            for (GraphEdge const& edge : _edges) {
                if (edge.source.node == GRAPH_ID) {
                    auto consumer_port_data = ctx.template resolve_exported_array_storage<SharedPortData>(
                        ingress_target_export_id(edge.target)
                    );
                    IV_ASSERT(!consumer_port_data.empty(), "graph ingress wiring must resolve the requested SharedPortData entry");
                    std::construct_at(&state.ingress_outputs[edge.source.port], const_cast<SharedPortData&>(consumer_port_data[0]), 0);
                }
            }
        }

        void tick_block(TickBlockContext<Graph> const& ctx) const
        {
            auto& state = ctx.state();
            push_input_blocks_to_private_outputs(state.ingress_outputs, ctx.inputs, ctx.block_size);

            for (size_t scc_index = 0; scc_index < _scc_wrappers.size(); ++scc_index) {
                do_tick_block(_scc_wrappers[scc_index], {
                    TickContext<GraphSccWrapper> { .inputs = {}, .outputs = {}, .buffer = state.scc_states[scc_index] },
                    ctx.index,
                    ctx.block_size,
                });
            }

            push_private_inputs_to_output_blocks(ctx.outputs, state.egress_inputs, ctx.block_size);
        }
    };
}
