#include <intravenous/dsl.h>

#include <array>

struct WindowedReplayNode {
    static constexpr auto replay_mode = iv::TickReplayMode::enabled;
    static constexpr auto inputs()
    {
        return std::array{iv::input<iv::Sample>({"input", "Input"}, iv::InputAccess::coverage,
            iv::InputContentAccess::windowed)};
    }
    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>({"output", "Output"})};
    }
    void tick_block(iv::TickBlockContext<WindowedReplayNode> const&) const {}
};

IV_NODE("iv.test.invalid_replay_windowed_input", WindowedReplayNode, "Invalid replay windowed input");
