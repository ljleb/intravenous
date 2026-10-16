#include <intravenous/dsl.h>

#include <array>

struct InvalidConstBackgroundState {
    using TockState = int const;

    static constexpr auto outputs()
    {
        return std::array {iv::output<iv::Sample>("output", iv::OutputProduction::coverage)};
    }

    void tock_coverage(iv::TockCoverageContext<InvalidConstBackgroundState>&) const {}
};

IV_NODE("iv.test.invalid_const_background_state", InvalidConstBackgroundState);
