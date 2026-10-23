#include <intravenous/dsl.h>

#include <array>

// A RandomAccess dependency cannot be replayed by a pointwise Tick node.
struct InvalidReplayWithRandomAccess {
    static constexpr bool intrinsically_replayable = true;
    static constexpr auto inputs()
    {
        return std::array {iv::input<iv::Sample>("input", iv::InputAccess::coverage)};
    }
    static constexpr auto outputs()
    {
        return std::array {iv::output<iv::Sample>("output", iv::OutputProduction::tick)};
    }
    void tick(iv::TickSampleContext<InvalidReplayWithRandomAccess> const&) const {}
};

IV_NODE("iv.test.invalid_replay_random_access", InvalidReplayWithRandomAccess);
