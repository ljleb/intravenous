#pragma once

#include <intravenous/node/lifecycle.h>

#include <array>

namespace iv {

struct Constant {
    Sample _value;

    // A registered IV node has one explicit public construction interface.
    // This replaces the aggregate-only spelling that was usable only through
    // the deleted g.node<Constant>(...) direct-type API.
    constexpr explicit Constant(Sample value = 0.0)
        : _value(value)
    {}

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
