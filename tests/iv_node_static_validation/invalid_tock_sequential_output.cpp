#include <intravenous/dsl.h>

#include <array>

struct InvalidTockSequentialOutput {
    static constexpr auto outputs() {
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::sequential)};
    }
    void tock_coverage(iv::TockCoverageContext<InvalidTockSequentialOutput>&) const {}
    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<InvalidTockSequentialOutput>&) const {}
};

static_assert(iv::details::has_tock_family_v<InvalidTockSequentialOutput>);

IV_NODE("iv.test.invalid_tock_sequential_output", InvalidTockSequentialOutput);
