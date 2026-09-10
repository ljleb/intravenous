#pragma once

#include <intravenous/node/lifecycle.h>

#include <array>

namespace iv {

struct Constant {
    Sample _value;

    static constexpr auto outputs()
    {
        return std::array<OutputConfig, 1>{};
    }

    void tick(TickSampleContext<Constant> const& state) const
    {
        state.outputs[0].push(_value);
    }
};

} // namespace iv
