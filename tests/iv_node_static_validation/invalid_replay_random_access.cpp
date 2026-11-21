#include <intravenous/dsl.h>

#include <array>

struct RandomAccessReplayNode {
    static constexpr auto replay_mode = iv::TickReplayMode::full;
    static constexpr auto inputs()
    {
        return std::array{iv::event_input("input", iv::EventTypeId::trigger, iv::InputAccess::region,
            iv::InputContentAccess::complete_buffer)};
    }
    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>("output")};
    }
    void tick_block(iv::TickBlockContext<RandomAccessReplayNode> const&) const {}
};

IV_NODE("iv.test.invalid_replay_random_access_event", RandomAccessReplayNode, "Invalid replay random access event");
