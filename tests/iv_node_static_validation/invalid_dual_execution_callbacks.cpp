#include <intravenous/dsl.h>

#include <tuple>

struct InvalidDualExecutionCallbacks {
    static constexpr auto outputs()
    {
        return std::tuple {
            iv::output<iv::Sample>(
                "realtime", iv::OutputProduction::tick),
            iv::output<iv::Sample>(
                "background", iv::OutputProduction::coverage),
        };
    }

    void tick_block(
        iv::TickBlockContext<InvalidDualExecutionCallbacks> const&) const {}
    void tock_coverage(
        iv::TockCoverageContext<InvalidDualExecutionCallbacks>&) const {}
    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<
            InvalidDualExecutionCallbacks>&) const {}
};

IV_NODE(
    "iv.test.invalid_dual_execution_callbacks",
    InvalidDualExecutionCallbacks);
