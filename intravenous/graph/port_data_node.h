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
        std::string _port_data_id;
        InputConfig _input;
        PortBufferPlan _input_buffer_plan;

        explicit GraphPortDataNode(
            std::string port_data_id,
            InputConfig input,
            PortBufferPlan input_buffer_plan
        ) :
            _port_data_id(std::move(port_data_id)),
            _input(std::move(input)),
            _input_buffer_plan(input_buffer_plan)
        {}

        struct State {
            std::span<SharedPortData> port_data;
            std::span<Sample> samples;
        };

        void declare(DeclarationContext<GraphPortDataNode> const& ctx) const
        {
            auto const& state = ctx.state();
            ctx.local_array(state.port_data, 1);
            ctx.local_array(
                state.samples,
                calculate_port_buffer_size(ctx.max_block_size(), _input_buffer_plan)
            );
            ctx.export_array(_port_data_id, state.port_data);
        }

        void initialize(InitializationContext<GraphPortDataNode> const& ctx) const
        {
            auto& state = ctx.state();
            std::fill(state.samples.begin(), state.samples.end(), _input.default_value);
            std::construct_at(&state.port_data[0], state.samples, 0);
        }
    };
}
