#pragma once

#include <intravenous/lane_node/generate.h>

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace iv {

struct LaneCreationContext;
class TypeErasedLaneNode;

struct BeatTriggerLaneSettings {
    double bpm = 140.0;
    std::uint32_t beats_per_bar = 4;
    std::uint32_t beat_unit = 4;
    std::uint32_t events_per_beat = 1;
};

class BeatTriggerLaneNode {
    BeatTriggerLaneSettings settings_ {};
    size_t sample_rate_ = 48000;
    std::uint64_t revision_ = 1;
    bool ui_dirty_ = true;

    static bool valid(BeatTriggerLaneSettings const &settings)
    {
        auto const denominator_is_power_of_two = settings.beat_unit != 0
            && (settings.beat_unit & (settings.beat_unit - 1)) == 0;
        return std::isfinite(settings.bpm) && settings.bpm >= 1.0 && settings.bpm <= 1000.0
            && settings.beats_per_bar >= 1 && settings.beats_per_bar <= 32
            && denominator_is_power_of_two && settings.beat_unit <= 64
            && settings.events_per_beat >= 1 && settings.events_per_beat <= 64;
    }

public:
    explicit BeatTriggerLaneNode(size_t sample_rate = 48000) : sample_rate_(sample_rate) {}

    BeatTriggerLaneNode(BeatTriggerLaneSettings settings, size_t sample_rate = 48000) :
        settings_(settings), sample_rate_(sample_rate)
    {}

    static constexpr std::string_view lane_model_type_id()
    {
        return "iv.timeline.beat-trigger";
    }

    static constexpr std::string_view lane_creation_category() { return "Timing"; }
    static constexpr std::string_view lane_creation_label() { return "Beat Trigger"; }
    static constexpr std::string_view lane_creation_description()
    {
        return "Compiled trigger events at a configurable tempo and subdivision";
    }

    static std::string default_lane_ui_state()
    {
        return BeatTriggerLaneNode{}.snapshot_lane_ui_state().serialized_state;
    }

    static TypeErasedLaneNode from_lane_ui_state(
        std::string_view serialized_state,
        LaneCreationContext const& context);

    static CompiledEventLaneOutputConfig output()
    {
        return {.name = "beats", .event_type = EventTypeId::trigger};
    }

    std::vector<CompiledSupportRange> compiled_support_ranges(
        CompiledSupportContext<BeatTriggerLaneNode>&) const
    {
        return {{.start_index = 0, .end_index = std::numeric_limits<size_t>::max()}};
    }

    void tick_block_compiled(CompiledLaneTickContext<BeatTriggerLaneNode>& ctx) const
    {
        auto const samples_per_event =
            (static_cast<double>(sample_rate_) * 60.0) / (settings_.bpm * settings_.events_per_beat);
        if (!(samples_per_event > 0.0)) return;

        auto const first = static_cast<size_t>(std::ceil(
            static_cast<double>(ctx.start_index()) / samples_per_event));
        for (size_t event_index = first;; ++event_index) {
            auto const sample_index = static_cast<size_t>(std::llround(event_index * samples_per_event));
            if (sample_index >= ctx.end_index()) break;
            if (sample_index >= ctx.start_index()) {
                ctx.out().push(TimedEvent{.time = sample_index, .value = TriggerEvent{}});
            }
        }
    }

    bool take_lane_ui_state_dirty()
    {
        auto const dirty = ui_dirty_;
        ui_dirty_ = false;
        return dirty;
    }

    LaneUiStateSnapshot snapshot_lane_ui_state() const
    {
        // This is the only execution-derived value the beat presentation
        // needs. Keeping it in the UI snapshot lets the client reconstruct
        // the regular event sequence without subscribing to event changes.
        auto const samples_per_event =
            (static_cast<double>(sample_rate_) * 60.0) / (settings_.bpm * settings_.events_per_beat);
        return {.revision = revision_, .serialized_state = nlohmann::json{
            {"bpm", settings_.bpm},
            {"beatsPerBar", settings_.beats_per_bar},
            {"beatUnit", settings_.beat_unit},
            {"eventsPerBeat", settings_.events_per_beat},
            {"eventIntervalSamples", samples_per_event},
        }.dump()};
    }

    LaneUiStateApplyResult apply_lane_ui_state(LaneUiStateWrite const &write)
    {
        if (write.expected_revision.has_value() && *write.expected_revision != revision_) {
            return {.error_message = "stale lane UI state revision"};
        }
        try {
            auto const json = nlohmann::json::parse(write.serialized_state);
            BeatTriggerLaneSettings next{
                .bpm = json.at("bpm").get<double>(),
                .beats_per_bar = json.at("beatsPerBar").get<std::uint32_t>(),
                .beat_unit = json.at("beatUnit").get<std::uint32_t>(),
                .events_per_beat = json.at("eventsPerBeat").get<std::uint32_t>(),
            };
            if (!valid(next)) return {.error_message = "invalid beat-trigger settings"};
            settings_ = next;
            ++revision_;
            ui_dirty_ = true;
            return {.accepted = true, .revision = revision_,
                    .effect = LaneUiStateEffect::execution_content_changed};
        } catch (std::exception const &error) {
            return {.error_message = error.what()};
        }
    }
};

} // namespace iv
