#pragma once

#include "node_lifecycle.h"

#include <span>
#include <string>
#include <utility>

namespace iv {
    struct GraphEventPortDataNode {
        std::string _port_data_id;
        EventInputConfig _input;

        explicit GraphEventPortDataNode(
            std::string port_data_id,
            EventInputConfig input
        ) :
            _port_data_id(std::move(port_data_id)),
            _input(std::move(input))
        {}

        struct State {
            std::span<EventSharedPortData> port_data;
            std::span<TimedEvent> events;
        };

        void declare(DeclarationContext<GraphEventPortDataNode> const& ctx) const
        {
            auto const& state = ctx.state();
            ctx.local_array(state.port_data, 1);
            ctx.local_array(
                state.events,
                calculate_event_port_buffer_capacity(ctx.event_port_buffer_base_multiplier(), _input.type)
            );
            ctx.export_array(_port_data_id, state.port_data);
        }

        void initialize(InitializationContext<GraphEventPortDataNode> const& ctx) const
        {
            auto& state = ctx.state();
            std::construct_at(
                &state.port_data[0],
                state.events,
                0,
                0,
                _input.type
            );
        }
    };
}
