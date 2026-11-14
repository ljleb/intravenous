#pragma once

#include <intravenous/node/lifecycle.h>

#include <array>

namespace iv {
    struct DummySink {
        static constexpr auto inputs()
        {
            return std::array<SequentialInputConfig<Sample>, 1>{};
        }

        void tick(TickSampleContext<DummySink> const&) const
        {}
    };

    struct DummyEventSink {
        static constexpr auto inputs()
        {
            return std::array { event_input({}, EventTypeId::empty, InputAccess::sequential) };
        }

        void tick_block(TickBlockContext<DummyEventSink> const&) const
        {}
    };
}
