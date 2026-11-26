#include <intravenous/dsl.h>

#include <array>

// A RandomAccess dependency cannot be replayed by a pointwise Tick node.
struct InvalidReplayWithRandomAccess {
    static constexpr iv::TickReplayMode replay_mode = iv::TickReplayMode::full;
    static constexpr auto inputs()
    {
        return std::array {iv::input<iv::Sample>({"input", "Input"}, iv::InputAccess::coverage, iv::InputContentAccess::complete_buffer)};
    }
    static constexpr auto outputs()
    {
        return std::array {iv::output<iv::Sample>({"output", "Output"}, iv::OutputProduction::sequential)};
    }
    void tick(iv::TickSampleContext<InvalidReplayWithRandomAccess> const&) const {}
};

IV_NODE("iv.test.invalid_replay_random_access", InvalidReplayWithRandomAccess, "Invalid replay random access");
