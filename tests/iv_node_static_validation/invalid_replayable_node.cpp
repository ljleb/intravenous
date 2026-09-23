#include <intravenous/dsl.h>

#include <array>

// A RandomAccess dependency cannot be replayed by a pointwise Tick node.
struct InvalidReplayWithRandomAccess {
    static constexpr bool intrinsically_replayable = true;
    static constexpr auto inputs()
    {
        return std::array {iv::random_access_sample_input("input")};
    }
    static constexpr auto outputs()
    {
        return std::array {iv::tick_sample_output("output")};
    }
    void tick(iv::TickSampleContext<InvalidReplayWithRandomAccess> const&) const {}
};

// An authored block callback cannot be substituted by synthesized pointwise
// replay even if the type also happens to provide tick().
struct InvalidReplayWithBlock {
    static constexpr bool intrinsically_replayable = true;
    static constexpr auto outputs()
    {
        return std::array {iv::tick_sample_output("output")};
    }
    void tick(iv::TickSampleContext<InvalidReplayWithBlock> const&) const {}
    void tick_block(iv::TickBlockContext<InvalidReplayWithBlock> const&) const {}
};

IV_NODE("iv.test.invalid_replay_random_access", InvalidReplayWithRandomAccess);
IV_NODE("iv.test.invalid_replay_with_block", InvalidReplayWithBlock);
