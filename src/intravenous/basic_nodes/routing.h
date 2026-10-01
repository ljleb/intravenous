#pragma once

#include <intravenous/node/lifecycle.h>

#include <array>

namespace iv {
    struct DummySink {
        static constexpr auto inputs()
        {
            return std::array<InputConfig, 1>{};
        }

        void tick(TickSampleContext<DummySink> const&) const
        {}
    };

    struct DummyEventSink {
        static constexpr auto inputs()
        {
            return std::array { sequential_event_input({}, EventTypeId::empty) };
        }

        void tick_block(TickBlockContext<DummyEventSink> const&) const
        {}
    };
}
