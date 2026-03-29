#pragma once

#include "wiring.h"
#include "node_lifecycle.h"

#include <algorithm>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace iv {
    struct GraphPortDataNode {
        std::vector<InputConfig> _inputs;
        std::vector<PortBufferPlan> _input_buffer_plans;
        std::string _node_id;

        explicit GraphPortDataNode(
            std::span<InputConfig const> inputs,
            std::vector<PortBufferPlan> input_buffer_plans,
            std::string node_id
        ) :
            _inputs(inputs.begin(), inputs.end()),
            _input_buffer_plans(std::move(input_buffer_plans)),
            _node_id(std::move(node_id))
        {}

        struct State {
            std::span<SharedPortData> port_data;
            std::span<Sample> samples;
        };

        void declare(DeclarationContext<GraphPortDataNode> const& ctx) const
        {
            auto const& state = ctx.state();
            ctx.local_array(state.port_data, _inputs.size());
            auto const input_sample_sizes = resolve_port_buffer_sizes(ctx.max_block_size(), _input_buffer_plans);
            auto const input_sample_offsets = make_input_sample_offsets(input_sample_sizes);
            ctx.local_array(state.samples, input_sample_offsets.empty() ? 0 : input_sample_offsets.back());
            for (size_t input_i = 0; input_i < _inputs.size(); ++input_i) {
                ctx.export_array_slice(port_data_export_id(_node_id, input_i), state.port_data, input_i, 1);
            }
        }

        void initialize(InitializationContext<GraphPortDataNode> const& ctx) const
        {
            auto& state = ctx.state();
            auto const input_sample_sizes = resolve_port_buffer_sizes(ctx.max_block_size(), _input_buffer_plans);
            auto const input_sample_offsets = make_input_sample_offsets(input_sample_sizes);
            for (size_t input_i = 0; input_i < _inputs.size(); ++input_i) {
                auto samples = input_sample_buffer(state.samples, input_sample_offsets, input_i);
                std::fill(samples.begin(), samples.end(), _inputs[input_i].default_value);
                std::construct_at(&state.port_data[input_i], samples, 0);
            }
        }
    };
}
