#include <intravenous/dsl.h>

#include <array>

struct InvalidReplayWithState {
    static constexpr iv::TickReplayMode replay_mode = iv::TickReplayMode::enabled;
    struct State { iv::Sample previous{}; };

    static constexpr auto outputs()
    {
        return std::array {iv::output<iv::Sample>({"output", "Output"}, iv::OutputProduction::sequential)};
    }
    void tick(iv::TickSampleContext<InvalidReplayWithState> const&) const {}
};

IV_NODE("iv.test.invalid_replay_with_state", InvalidReplayWithState, "Invalid replay with state");
