#pragma once

#include <intravenous/lane_node/generate.h>
#include <intravenous/wav.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace iv {

struct LaneCreationContext;
class TypeErasedLaneNode;

// A transparent realtime sink used to capture the exact samples travelling
// through a lane. Capturing is cheap copying during transport; disk I/O is
// deliberately deferred to TimelineExecution::pause().
class AudioFileCaptureLaneNode {
    std::string path_ = "timeline-capture.wav";
    size_t sample_rate_ = 48000;
    std::uint64_t revision_ = 1;
    bool ui_dirty_ = true;
    mutable std::vector<std::vector<Sample>> channels_;
    mutable bool capture_active_ = false;

public:
    explicit AudioFileCaptureLaneNode(size_t sample_rate = 48000) : sample_rate_(sample_rate) {}
    AudioFileCaptureLaneNode(std::string path, size_t sample_rate) :
        path_(std::move(path)), sample_rate_(sample_rate) {}

    static constexpr std::string_view lane_model_type_id() { return "iv.timeline.audio-file-capture"; }
    static constexpr std::string_view lane_creation_category() { return "Audio"; }
    static constexpr std::string_view lane_creation_label() { return "Audio File Capture"; }
    static constexpr std::string_view lane_creation_description()
    {
        return "Passes audio through and writes the captured playback to a WAV file when paused";
    }

    static std::string default_lane_ui_state()
    {
        return nlohmann::json{{"path", "timeline-capture.wav"}}.dump();
    }

    static TypeErasedLaneNode from_lane_ui_state(
        std::string_view serialized_state,
        LaneCreationContext const& context);

    std::array<RealtimeSampleLaneInputConfig, 1> realtime_sample_inputs() const
    {
        return {{{.name = "audio"}}};
    }

    static RealtimeSampleLaneOutputConfig output() { return {.name = "audio"}; }

    void tick_block_realtime(RealtimeLaneTickContext<AudioFileCaptureLaneNode>& ctx) const
    {
        auto const input = ctx.realtime_sample_input(0).block_view();
        ctx.out().write_block(input);
        if (!capture_active_) {
            channels_.clear();
            channels_.resize(input.channels());
            capture_active_ = true;
        }
        if (channels_.size() != input.channels()) {
            channels_.clear();
            channels_.resize(input.channels());
        }
        for (size_t channel = 0; channel < input.channels(); ++channel) {
            auto& captured = channels_[channel];
            auto const required = captured.size() + input.frames();
            if (captured.capacity() < required) {
                captured.reserve(std::max(required, std::max<size_t>(input.frames(), captured.capacity() * 2)));
            }
            for (size_t frame = 0; frame < input.frames(); ++frame) {
                captured.push_back(input.get(frame, channel));
            }
        }
    }

    void flush_on_pause() const
    {
        if (!capture_active_) return;
        capture_active_ = false;
        if (channels_.empty()) return;
        write_wav(path_, std::span<std::vector<Sample> const>{channels_.data(), channels_.size()},
            static_cast<std::uint32_t>(sample_rate_));
    }

    bool take_lane_ui_state_dirty()
    {
        auto const dirty = ui_dirty_;
        ui_dirty_ = false;
        return dirty;
    }

    LaneUiStateSnapshot snapshot_lane_ui_state() const
    {
        return {.revision = revision_, .serialized_state = nlohmann::json{{"path", path_}}.dump()};
    }

    LaneUiStateApplyResult apply_lane_ui_state(LaneUiStateWrite const& write)
    {
        if (write.expected_revision.has_value() && *write.expected_revision != revision_) {
            return {.error_message = "stale lane UI state revision"};
        }
        try {
            auto const path = nlohmann::json::parse(write.serialized_state).at("path").get<std::string>();
            if (path.empty()) return {.error_message = "audio-file-capture path must not be empty"};
            path_ = path;
            ++revision_;
            ui_dirty_ = true;
            return {.accepted = true, .revision = revision_};
        } catch (std::exception const& error) {
            return {.error_message = error.what()};
        }
    }
};

} // namespace iv
