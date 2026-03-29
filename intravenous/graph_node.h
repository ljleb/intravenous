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
        std::vector<GraphSccWrapper> _scc_wrappers;
        decltype(GraphBuildArtifact::edges) _edges;
        std::vector<InputConfig> _public_inputs;
        std::vector<OutputConfig> _public_outputs;
        std::vector<size_t> _public_output_sample_offsets;
        size_t _internal_latency;

        explicit Graph(GraphBuildArtifact artifact) :
            _scc_wrappers(std::move(artifact.scc_wrappers)),
            _edges(std::move(artifact.edges)),
            _public_inputs(std::move(artifact.public_inputs)),
            _public_outputs(std::move(artifact.public_outputs)),
            _public_output_sample_offsets(make_input_sample_offsets(artifact.public_output_sample_sizes)),
            _internal_latency(artifact.internal_latency)
        {}

        struct State {
            std::span<std::byte*> scc_states;
            std::span<OutputPort> ingress_outputs;
            std::span<SharedPortData> egress_port_data;
            std::span<Sample> egress_samples;
            std::span<InputPort> egress_inputs;
        };

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
            for (auto const& scc : _scc_wrappers) {
                do_declare(scc, ctx);
            }
            ctx.nested_node_states(state.scc_states);
            ctx.local_array(state.egress_port_data, num_outputs());
            ctx.local_array(state.egress_samples, _public_output_sample_offsets.empty() ? 0 : _public_output_sample_offsets.back());
            ctx.local_array(state.egress_inputs, num_outputs());
            ctx.export_array(graph_port_data_export_id(), state.egress_port_data);
        }

        void initialize(InitializationContext<Graph> const& ctx) const
        {
            auto& state = ctx.state();
            for (size_t output_i = 0; output_i < num_outputs(); ++output_i) {
                auto samples = input_sample_buffer(state.egress_samples, _public_output_sample_offsets, output_i);
                std::fill(samples.begin(), samples.end(), Sample(0));
                std::construct_at(&state.egress_port_data[output_i], samples, 0);
                std::construct_at(&state.egress_inputs[output_i], state.egress_port_data[output_i], 0);
            }

            for (GraphEdge const& edge : _edges) {
                if (edge.source.node == GRAPH_ID) {
                    auto consumer_port_data = ctx.template resolve_exported_array_storage<SharedPortData>(
                        port_data_export_id(std::to_string(edge.target.node))
                    );
                    std::construct_at(&state.ingress_outputs[edge.source.port], const_cast<SharedPortData&>(consumer_port_data[edge.target.port]), 0);
                }
            }
        }

        void tick_block(TickBlockContext<Graph> const& ctx) const
        {
            if (ctx.block_size == 0) {
                return;
            }
            validate_block_size(ctx.block_size, "Graph::tick_block requires a power-of-2 block size");

            auto& state = ctx.state();
            push_input_blocks_to_private_outputs(state.ingress_outputs, ctx.inputs, ctx.block_size);

            for (size_t scc_index = 0; scc_index < _scc_wrappers.size(); ++scc_index) {
                tick_scc(scc_index, ctx, ctx.block_size);
            }

            push_private_inputs_to_output_blocks(ctx.outputs, state.egress_inputs, ctx.block_size);
        }

    private:
        void tick_scc(
            size_t scc_index,
            TickBlockContext<Graph> const& ctx,
            size_t block_size
        ) const
        {
            do_tick_block(_scc_wrappers[scc_index], {
                TickContext<GraphSccWrapper> {
                    .inputs = {},
                    .outputs = {},
                    .buffer = remaining_buffer(ctx.buffer, ctx.state().scc_states[scc_index]),
                },
                ctx.index,
                block_size
            });
        }

    };
}
