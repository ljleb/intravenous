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
        decltype(GraphBuildArtifact::edges) _edges;
        std::vector<InputConfig> _public_inputs;
        std::vector<OutputConfig> _public_outputs;
        std::vector<PortBufferPlan> _public_output_buffer_plans;
        size_t _internal_latency;
        std::vector<std::string> _node_ids;

        explicit Graph(GraphBuildArtifact artifact) :
            _graph_id(std::move(artifact.graph_id)),
            _scc_wrappers(std::move(artifact.scc_wrappers)),
            _edges(std::move(artifact.edges)),
            _public_inputs(std::move(artifact.public_inputs)),
            _public_outputs(std::move(artifact.public_outputs)),
            _public_output_buffer_plans(std::move(artifact.public_output_buffer_plans)),
            _internal_latency(artifact.internal_latency),
            _node_ids(std::move(artifact.node_ids))
        {}

        struct State {
            std::span<std::span<std::byte>> scc_states;
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
            ctx.nested_node_states(state.scc_states);
            for (auto const& scc : _scc_wrappers) {
                do_declare(scc, ctx);
            }
            ctx.local_array(state.egress_inputs, num_outputs());
            ctx.local_array(state.egress_port_data, num_outputs());
            auto const output_sample_sizes = resolve_port_buffer_sizes(ctx.max_block_size(), _public_output_buffer_plans);
            auto const output_sample_offsets = make_input_sample_offsets(output_sample_sizes);
            ctx.local_array(state.egress_samples, output_sample_offsets.empty() ? 0 : output_sample_offsets.back());

            ctx.export_array(graph_port_data_export_id(_graph_id), state.egress_port_data);
            for (GraphEdge const& edge : _edges) {
                if (edge.source.node == GRAPH_ID) {
                    ctx.require_export_array<SharedPortData>(
                        port_data_export_id(_node_ids[edge.target.node])
                    );
                }
            }
        }

        void initialize(InitializationContext<Graph> const& ctx) const
        {
            auto& state = ctx.state();
            auto const output_sample_sizes = resolve_port_buffer_sizes(ctx.max_block_size(), _public_output_buffer_plans);
            auto const output_sample_offsets = make_input_sample_offsets(output_sample_sizes);
            for (size_t output_i = 0; output_i < num_outputs(); ++output_i) {
                auto samples = input_sample_buffer(state.egress_samples, output_sample_offsets, output_i);
                std::fill(samples.begin(), samples.end(), Sample(0));
                std::construct_at(&state.egress_port_data[output_i], samples, 0);
                std::construct_at(&state.egress_inputs[output_i], state.egress_port_data[output_i], 0);
            }

            for (GraphEdge const& edge : _edges) {
                if (edge.source.node == GRAPH_ID) {
                    auto consumer_port_data = ctx.template resolve_exported_array_storage<SharedPortData>(
                        port_data_export_id(_node_ids[edge.target.node])
                    );
                    IV_ASSERT(edge.target.port < consumer_port_data.size(), "graph ingress wiring must resolve the requested SharedPortData entry");
                    std::construct_at(&state.ingress_outputs[edge.source.port], const_cast<SharedPortData&>(consumer_port_data[edge.target.port]), 0);
                }
            }
        }

        void tick_block(TickBlockContext<Graph> const& ctx) const
        {
            // if (ctx.block_size == 0) {
            //     return;
            // }
            // validate_block_size(ctx.block_size, "Graph::tick_block requires a power-of-2 block size");

            // auto const start_time = std::chrono::steady_clock::now();
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
