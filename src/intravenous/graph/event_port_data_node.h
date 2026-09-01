#pragma once

#include <intravenous/graph/static_storage.hpp>
#include <intravenous/node/lifecycle.h>

#include <span>
#include <string>
#include <utility>

namespace iv {
    struct GraphEventPortDataNode {
        StaticString _port_data_id;
        EventTypeId _type {};

        consteval explicit GraphEventPortDataNode(
            std::string port_data_id,
            EventTypeId type
        ) :
            _port_data_id(details::define_static_string(port_data_id)),
            _type(type)
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
                calculate_event_port_buffer_capacity(
                    ctx.event_port_buffer_base_multiplier(), _type)
            );
            ctx.export_array(
                std::string(_port_data_id.view()), state.port_data);
        }

        void initialize(InitializationContext<GraphEventPortDataNode> const& ctx) const
        {
            auto& state = ctx.state();
            std::construct_at(
                &state.port_data[0],
                state.events,
                0,
                0,
                _type
            );
        }
    };
}
