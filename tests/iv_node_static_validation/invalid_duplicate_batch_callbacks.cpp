#include <intravenous/dsl.h>

#include <array>
#include <tuple>

struct InvalidScalarAndBatchTick {
    static constexpr auto inputs() { return std::tuple{}; }
    static constexpr auto outputs() { return std::tuple{}; }

    void tick_block(
        iv::TickBlockContext<InvalidScalarAndBatchTick> const&) const {}
    static void tick_block_batch(
        iv::TickBlockBatchContext<InvalidScalarAndBatchTick> const&) {}
};

struct InvalidScalarAndBatchSkip {
    static constexpr auto inputs() { return std::tuple{}; }
    static constexpr auto outputs() { return std::tuple{}; }

    void tick_block(
        iv::TickBlockContext<InvalidScalarAndBatchSkip> const&) const {}
    void skip_block(
        iv::SkipBlockContext<InvalidScalarAndBatchSkip> const&) const {}
    static void skip_block_batch(
        iv::SkipBlockBatchContext<InvalidScalarAndBatchSkip> const&) {}
};

struct InvalidScalarAndBatchTock {
    static constexpr auto inputs() { return std::tuple{}; }
    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>(
            "output", iv::OutputProduction::coverage)};
    }

    void tock_coverage(
        iv::TockCoverageContext<InvalidScalarAndBatchTock>&) const {}
    static void tock_coverage_batch(
        iv::TockCoverageBatchContext<InvalidScalarAndBatchTock>&) {}
    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<
            InvalidScalarAndBatchTock>&) const {}
};

struct InvalidScalarAndBatchForward {
    static constexpr auto inputs() { return std::tuple{}; }
    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>(
            "output", iv::OutputProduction::coverage)};
    }

    void tock_coverage(
        iv::TockCoverageContext<InvalidScalarAndBatchForward>&) const {}
    void propagate_forward_coverage(
        iv::PropagateForwardCoverageContext<
            InvalidScalarAndBatchForward>&) const {}
    static void propagate_forward_coverage_batch(
        iv::PropagateForwardCoverageBatchContext<
            InvalidScalarAndBatchForward>&) {}
};

IV_NODE("iv.test.invalid_scalar_and_batch_tick", InvalidScalarAndBatchTick);
IV_NODE("iv.test.invalid_scalar_and_batch_skip", InvalidScalarAndBatchSkip);
IV_NODE("iv.test.invalid_scalar_and_batch_tock", InvalidScalarAndBatchTock);
IV_NODE("iv.test.invalid_scalar_and_batch_forward", InvalidScalarAndBatchForward);
