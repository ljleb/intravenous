#pragma once

#include <intravenous/lane_node/generate.h>
#include <intravenous/wav.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace iv {

struct LaneCreationContext;
class TypeErasedLaneNode;

// A compiled audio clip which optionally records a live realtime input. While
// recording, input is monitored through the compiled output; otherwise that
// output plays the captured timeline back without requiring an input edge.
class AudioFileCaptureLaneNode {
    std::string path_ = "timeline-capture.wav";
    size_t sample_rate_ = 48000;
    std::uint64_t revision_ = 1;
    bool ui_dirty_ = true;
    mutable std::vector<std::vector<Sample>> channels_;
    mutable bool capture_active_ = false;
    mutable size_t capture_start_index_ = 0;

    void load_existing_capture()
    {
        auto const decoded = read_pcm16_wav(path_);
        if (!decoded.has_value()) return;
        channels_ = decoded->channels;
        capture_start_index_ = 0;
    }

public:
    explicit AudioFileCaptureLaneNode(size_t sample_rate = 48000) : sample_rate_(sample_rate)
    {
        load_existing_capture();
    }
    AudioFileCaptureLaneNode(std::string path, size_t sample_rate) :
        path_(std::move(path)), sample_rate_(sample_rate)
    {
        load_existing_capture();
    }

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

    static CompiledSampleLaneOutputConfig output() { return {.name = "audio"}; }

    std::vector<CompiledSupportRange> compiled_support_ranges(
        CompiledSupportContext<AudioFileCaptureLaneNode>&) const
    {
        return {{.start_index = 0, .end_index = std::numeric_limits<size_t>::max()}};
    }

    void tick_block_compiled(CompiledLaneTickContext<AudioFileCaptureLaneNode>& ctx) const
    {
        auto const &input_port = ctx.compiled_sample_input(0);
        auto const input = input_port.block_view();
        auto const recording = ctx.transport_playing() && input_port.connected();
        if (!recording) {
            auto const output_layout = ctx.out().channel_layout;
            auto const frame_count = ctx.sample_count();
            std::vector<Sample> playback(sample_storage_size(output_layout, frame_count), Sample {});
            SampleBlockView<Sample> playback_view(playback, output_layout, frame_count);
            if (!channels_.empty() && ctx.start_index() >= capture_start_index_) {
                auto const capture_offset = ctx.start_index() - capture_start_index_;
                for (size_t frame = 0; frame < frame_count; ++frame) {
                    auto const captured_frame = capture_offset + frame;
                    for (size_t channel = 0; channel < playback_view.channels(); ++channel) {
                        if (channel < channels_.size() && captured_frame < channels_[channel].size()) {
                            playback_view.set(frame, channel, channels_[channel][captured_frame]);
                        }
                    }
                }
            }
            ctx.out().write_block(ctx.start_index(), SampleBlockView<Sample const>(
                std::span<Sample const>(playback), output_layout, frame_count));
            return;
        }
        if (!capture_active_) {
            channels_.clear();
            channels_.resize(input.channels());
            capture_active_ = true;
            capture_start_index_ = ctx.start_index();
        }
        if (channels_.size() != input.channels()) {
            channels_.clear();
            channels_.resize(input.channels());
            capture_start_index_ = ctx.start_index();
        }
        ctx.out().write_block(ctx.start_index(), input);
        auto const capture_offset = ctx.start_index() - capture_start_index_;
        for (size_t channel = 0; channel < input.channels(); ++channel) {
            auto& captured = channels_[channel];
            auto const required = capture_offset + input.frames();
            if (captured.capacity() < required) {
                captured.reserve(std::max(required, std::max<size_t>(input.frames(), captured.capacity() * 2)));
            }
            if (captured.size() < required) captured.resize(required, Sample {});
            for (size_t frame = 0; frame < input.frames(); ++frame) {
                captured[capture_offset + frame] = input.get(frame, channel);
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
            channels_.clear();
            capture_start_index_ = 0;
            load_existing_capture();
            ++revision_;
            ui_dirty_ = true;
            return {.accepted = true, .revision = revision_};
        } catch (std::exception const& error) {
            return {.error_message = error.what()};
        }
    }
};

} // namespace iv
