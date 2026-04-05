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
        };

        void declare(DeclarationContext<GraphEventPortDataNode> const& ctx) const
        {
            auto const& state = ctx.state();
            ctx.local_array(state.port_data, 1);
            ctx.export_array(_port_data_id, state.port_data);
        }

        void initialize(InitializationContext<GraphEventPortDataNode> const& ctx) const
        {
            auto& state = ctx.state();
            std::construct_at(
                &state.port_data[0],
                ctx.resources.event_stream_storage().allocate(_input.type),
                _input.type
            );
        }
    };
}
