#include <intravenous/dsl.h>

#include <array>

struct ReverseReplayNode {
    static constexpr auto replay_mode = iv::TickReplayMode::enabled;
    static constexpr auto inputs() { return std::array{iv::input<iv::Sample>({"input", "Input"})}; }
    static constexpr auto outputs() { return std::array{iv::output<iv::Sample>({"output", "Output"})}; }
    void tick_block(iv::TickBlockContext<ReverseReplayNode> const&) const {}
    void propagate_reverse_coverage(iv::PropagateReverseCoverageContext<ReverseReplayNode>&) const {}
};

IV_NODE("iv.test.invalid_replay_reverse_coverage", ReverseReplayNode, "Invalid replay reverse coverage");
