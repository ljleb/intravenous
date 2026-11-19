#include <intravenous/dsl.h>

#include <array>

struct ForwardReplayNode {
    static constexpr auto replay_mode = iv::TickReplayMode::input_union;
    static constexpr auto inputs() { return std::array{iv::input<iv::Sample>("input")}; }
    static constexpr auto outputs() { return std::array{iv::output<iv::Sample>("output")}; }
    void tick_block(iv::TickBlockContext<ForwardReplayNode> const&) const {}
    void propagate_forward_coverage(iv::PropagateForwardCoverageContext<ForwardReplayNode>&) const {}
};

struct ReverseReplayNode {
    static constexpr auto replay_mode = iv::TickReplayMode::full;
    static constexpr auto inputs() { return std::array{iv::input<iv::Sample>("input")}; }
    static constexpr auto outputs() { return std::array{iv::output<iv::Sample>("output")}; }
    void tick_block(iv::TickBlockContext<ReverseReplayNode> const&) const {}
    void propagate_reverse_coverage(iv::PropagateReverseCoverageContext<ReverseReplayNode>&) const {}
};

IV_NODE("iv.test.invalid_replay_forward_coverage", ForwardReplayNode, "Invalid replay forward coverage");
IV_NODE("iv.test.invalid_replay_reverse_coverage", ReverseReplayNode, "Invalid replay reverse coverage");
