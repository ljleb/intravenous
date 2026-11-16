#include <intravenous/dsl.h>

#include <array>

// The boolean replay opt-in was replaced by TickReplayMode. A node still
// declaring it must not silently become non-replayable.
struct InvalidReplayBoolean {
    static constexpr bool intrinsically_replayable = true;
    static constexpr auto outputs()
    {
        return std::array {iv::output<iv::Sample>("output", iv::OutputProduction::sequential)};
    }
    void tick(iv::TickSampleContext<InvalidReplayBoolean> const&) const {}
};

IV_NODE("iv.test.invalid_replay_boolean", InvalidReplayBoolean);
