#pragma once

#include <intravenous/node/lifecycle.h>

#include <array>

namespace iv {

struct Constant {
    Sample _value;

    // The same default applies to construction by type or by definition ID.
    constexpr explicit Constant(Sample value = 0.0)
        : _value(value)
    {}

    static constexpr auto outputs()
    {
        return std::array<SequentialOutputConfig<Sample>, 1>{};
    }

    void tick(TickSampleContext<Constant> const& state) const
    {
        state.template output<0>().push(_value);
    }
};

} // namespace iv
