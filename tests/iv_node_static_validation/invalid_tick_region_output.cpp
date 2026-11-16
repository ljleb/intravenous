#include <intravenous/dsl.h>

#include <array>

struct InvalidTickRegionOutput {
    static constexpr auto outputs() {
        return std::array{iv::output<iv::Sample>("out", iv::OutputProduction::region)};
    }
    void tick_block(iv::TickBlockContext<InvalidTickRegionOutput> const&) const {}
    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<InvalidTickRegionOutput>&) const {}
};

static_assert(iv::details::has_tick_family_v<InvalidTickRegionOutput>);

IV_NODE("iv.test.invalid_tick_region_output", InvalidTickRegionOutput);
