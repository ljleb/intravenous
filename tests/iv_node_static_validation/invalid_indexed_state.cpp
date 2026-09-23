#include <intravenous/dsl.h>

#include <array>

struct InvalidConstIndexedState {
    using IndexedState = int const;

    static constexpr auto outputs()
    {
        return std::array {iv::tock_sample_output("output")};
    }

    void tock_coverage(iv::TockCoverageContext<InvalidConstIndexedState>&) const {}
};

IV_NODE("iv.test.invalid_const_indexed_state", InvalidConstIndexedState);
