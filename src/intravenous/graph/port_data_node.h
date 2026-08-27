#pragma once

#include <intravenous/graph/static_storage.hpp>
#include <intravenous/graph/wiring.h>
#include <intravenous/node/lifecycle.h>

#include <algorithm>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace iv {
    struct GraphPortDataNode {
        StaticString _port_data_id;
        StaticInputConfig _input;
        PortBufferPlan _input_buffer_plan;

        consteval explicit GraphPortDataNode(
            std::string port_data_id,
            InputConfig input,
            PortBufferPlan input_buffer_plan
        ) :
            _port_data_id(details::define_static_string(port_data_id)),
            _input(details::define_static_config(input)),
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
                sample_storage_size(
                    _input.channel_layout,
                    calculate_port_buffer_size(ctx.max_block_size(), _input_buffer_plan)
                )
            );
            ctx.export_array(
                std::string(_port_data_id.view()), state.port_data);
        }

        void initialize(InitializationContext<GraphPortDataNode> const& ctx) const
        {
            auto& state = ctx.state();
            std::fill(state.samples.begin(), state.samples.end(), _input.default_value);
            auto const layout = _input.channel_layout;
            std::construct_at(
                &state.port_data[0],
                state.samples,
                0,
                layout,
                state.samples.size() / channel_count(layout)
            );
        }
    };
}
