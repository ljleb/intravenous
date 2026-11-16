#include <intravenous/dsl.h>

#include <array>

struct InvalidReplayPersistedSample {
    static constexpr iv::TickReplayMode replay_mode = iv::TickReplayMode::full;
    static constexpr auto outputs()
    {
        return std::array{iv::output<iv::Sample>(
            "audio", iv::OutputProduction::sequential, iv::OutputRealization::buffered)};
    }
    void tick(iv::TickSampleContext<InvalidReplayPersistedSample> const&) const {}
};

struct InvalidReplayPersistedEvent {
    static constexpr iv::TickReplayMode replay_mode = iv::TickReplayMode::full;
    static constexpr auto outputs()
    {
        return std::array{iv::event_output(
            "events", iv::EventTypeId::trigger,
            iv::OutputProduction::sequential, iv::OutputRealization::buffered)};
    }
    void tick(iv::TickSampleContext<InvalidReplayPersistedEvent> const&) const {}
};

static_assert(!iv::details::replay_declaration_is_valid_v<InvalidReplayPersistedSample>);
static_assert(!iv::details::replay_declaration_is_valid_v<InvalidReplayPersistedEvent>);

IV_NODE("iv.test.invalid_replay_persisted_sample", InvalidReplayPersistedSample);
IV_NODE("iv.test.invalid_replay_persisted_event", InvalidReplayPersistedEvent);
