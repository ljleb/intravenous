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
        std::vector<size_t> _input_sample_sizes;
        std::vector<size_t> _input_sample_offsets;
        std::string _node_id;

        explicit GraphPortDataNode(
            std::span<InputConfig const> inputs,
            std::vector<size_t> input_sample_sizes,
            std::string node_id
        ) :
            _inputs(inputs.begin(), inputs.end()),
            _input_sample_sizes(std::move(input_sample_sizes)),
            _input_sample_offsets(make_input_sample_offsets(_input_sample_sizes)),
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
            ctx.local_array(state.samples, _input_sample_offsets.empty() ? 0 : _input_sample_offsets.back());
            ctx.export_array(port_data_export_id(_node_id), state.port_data);
        }

        void initialize(InitializationContext<GraphPortDataNode> const& ctx) const
        {
            auto& state = ctx.state();
            for (size_t input_i = 0; input_i < _inputs.size(); ++input_i) {
                auto samples = input_sample_buffer(state.samples, _input_sample_offsets, input_i);
                std::fill(samples.begin(), samples.end(), _inputs[input_i].default_value);
                std::construct_at(&state.port_data[input_i], samples, 0);
            }
        }
    };
}
