#include <intravenous/dsl.h>

#include <array>

struct InvalidTickCoverageOutput {
    static constexpr auto outputs() {
        return std::array{iv::output<iv::Sample>({"out", "Out"}, iv::OutputProduction::coverage)};
    }
    void tick_block(iv::TickBlockContext<InvalidTickCoverageOutput> const&) const {}
    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<InvalidTickCoverageOutput>&) const {}
};

static_assert(iv::details::has_tick_family_v<InvalidTickCoverageOutput>);

IV_NODE("iv.test.invalid_tick_coverage_output", InvalidTickCoverageOutput, "Invalid tick coverage output");
