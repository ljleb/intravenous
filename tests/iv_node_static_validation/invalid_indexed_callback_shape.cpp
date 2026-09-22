#include <intravenous/dsl.h>

#include <array>

struct InvalidInputOnlyTockCallback {
    static constexpr auto inputs()
    {
        return std::array {iv::indexed_sample_input("input")};
    }

    void tick_block(iv::TickBlockContext<InvalidInputOnlyTockCallback> const&) const {}
    void tock_coverage(iv::TockCoverageContext<InvalidInputOnlyTockCallback>&) const {}
};

IV_NODE("iv.test.invalid_input_only_tock_callback", InvalidInputOnlyTockCallback);

struct InvalidInputOnlyForwardCallback {
    static constexpr auto inputs()
    {
        return std::array {iv::indexed_sample_input("input")};
    }

    void tick_block(
        iv::TickBlockContext<InvalidInputOnlyForwardCallback> const&) const {}
    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<InvalidInputOnlyForwardCallback>&) const {}
};

IV_NODE(
    "iv.test.invalid_input_only_forward_callback",
    InvalidInputOnlyForwardCallback);

struct InvalidOutputOnlyPropagationCallback {
    static constexpr auto outputs()
    {
        return std::array {iv::indexed_sample_output("output")};
    }

    void tick_block(iv::TickBlockContext<InvalidOutputOnlyPropagationCallback> const&) const {}
    void tock_coverage(
        iv::TockCoverageContext<InvalidOutputOnlyPropagationCallback>&) const {}
    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<
            InvalidOutputOnlyPropagationCallback>&) const {}
    void propagate_reverse_coverage(
        iv::PropagateReverseCoverageContext<InvalidOutputOnlyPropagationCallback>&) const {}
};

IV_NODE("iv.test.invalid_output_only_propagation_callback", InvalidOutputOnlyPropagationCallback);
