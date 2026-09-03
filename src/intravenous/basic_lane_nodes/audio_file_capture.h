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

class AudioFileCaptureLaneNode {
    std::string path_ = "timeline-capture.wav";
    size_t sample_rate_ = 48000;
    std::uint64_t revision_ = 1;
    mutable bool ui_dirty_ = true;
    mutable std::vector<std::vector<Sample>> channels_;
    mutable bool capture_active_ = false;
    mutable size_t capture_start_index_ = 0;
    mutable std::vector<std::vector<Sample>> recording_channels_;
    mutable size_t recording_start_index_ = 0;
    std::string recording_mode_ = "reset";

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

    static std::array<LanePortConfig, 2> ports()
    {
        return {
            sample_input_port("audio", LanePortDomain::realtime),
            sample_output_port("audio", LanePortDomain::compiled),
        };
    }

    std::vector<CompiledSupportRange> compiled_support_ranges(
        CompiledSupportContext<AudioFileCaptureLaneNode>&) const
    {
        return {{.start_index = 0, .end_index = std::numeric_limits<size_t>::max()}};
    }

    void tick_block_compiled(CompiledLaneTickContext<AudioFileCaptureLaneNode>& ctx) const
    {
        auto& output = std::get<CompiledSampleLaneOutput>(ctx.out());
        auto const &input_port = ctx.compiled_sample_input(0);
        auto const input = input_port.block_view();
        auto const recording = ctx.transport_playing() && input_port.connected();
        if (!recording) {
            auto const output_layout = output.channel_layout;
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
            output.write_block(ctx.start_index(), SampleBlockView<Sample const>(
                std::span<Sample const>(playback), output_layout, frame_count));
            return;
        }
        if (!capture_active_) {
            capture_active_ = true;
            recording_channels_.assign(input.channels(), {});
            recording_start_index_ = recording_mode_ == "append"
                ? capture_start_index_ + (channels_.empty() ? 0 : channels_.front().size())
                : ctx.start_index();
        }
        if (recording_channels_.size() != input.channels()) {
            recording_channels_.assign(input.channels(), {});
            recording_start_index_ = ctx.start_index();
        }
        output.write_block(ctx.start_index(), input);
        auto const capture_offset = ctx.start_index() - recording_start_index_;
        for (size_t channel = 0; channel < input.channels(); ++channel) {
            auto& captured = recording_channels_[channel];
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
        if (recording_channels_.empty()) return;
        auto const recording_frames = recording_channels_.front().size();
        if (recording_mode_ == "reset" || channels_.empty()) {
            channels_ = std::move(recording_channels_);
            capture_start_index_ = recording_start_index_;
        } else if (recording_mode_ == "insert") {
            auto const old_start = capture_start_index_;
            auto const old_frames = channels_.front().size();
            auto const insert = recording_start_index_ <= old_start ? 0 : std::min(recording_start_index_ - old_start, old_frames);
            if (recording_start_index_ < old_start) capture_start_index_ = recording_start_index_;
            for (size_t channel = 0; channel < channels_.size(); ++channel) {
                auto& clip = channels_[channel];
                auto const& recorded = recording_channels_[std::min(channel, recording_channels_.size() - 1)];
                clip.insert(clip.begin() + static_cast<std::ptrdiff_t>(insert), recorded.begin(), recorded.end());
            }
        } else {
            auto const old_start = capture_start_index_;
            auto const old_end = old_start + channels_.front().size();
            auto const new_start = std::min(old_start, recording_start_index_);
            auto const new_end = std::max(old_end, recording_start_index_ + recording_frames);
            std::vector<std::vector<Sample>> merged(channels_.size(), std::vector<Sample>(new_end - new_start));
            for (size_t channel = 0; channel < merged.size(); ++channel) {
                std::copy(channels_[channel].begin(), channels_[channel].end(), merged[channel].begin() + (old_start - new_start));
                auto const& recorded = recording_channels_[std::min(channel, recording_channels_.size() - 1)];
                std::copy(recorded.begin(), recorded.end(), merged[channel].begin() + (recording_start_index_ - new_start));
            }
            channels_ = std::move(merged);
            capture_start_index_ = new_start;
        }
        recording_channels_.clear();
        write_wav(path_, std::span<std::vector<Sample> const>{channels_.data(), channels_.size()},
            static_cast<std::uint32_t>(sample_rate_));
        ui_dirty_ = true;
    }

    bool take_lane_ui_state_dirty()
    {
        auto const dirty = ui_dirty_;
        ui_dirty_ = false;
        return dirty;
    }

    LaneUiStateSnapshot snapshot_lane_ui_state() const
    {
        size_t frame_count = 0;
        for (auto const& channel : channels_) frame_count = std::max(frame_count, channel.size());
        return {.revision = revision_, .serialized_state = nlohmann::json{
            {"path", path_},
            {"captureStartIndex", capture_start_index_},
            {"captureFrameCount", frame_count},
            {"recordingMode", recording_mode_},
        }.dump()};
    }

    LaneUiStateApplyResult apply_lane_ui_state(LaneUiStateWrite const& write)
    {
        if (write.expected_revision.has_value() && *write.expected_revision != revision_) {
            return {.error_message = "stale lane UI state revision"};
        }
        try {
            auto const json = nlohmann::json::parse(write.serialized_state);
            auto const path = json.at("path").get<std::string>();
            if (path.empty()) return {.error_message = "audio-file-capture path must not be empty"};
            path_ = path;
            channels_.clear();
            capture_start_index_ = 0;
            load_existing_capture();
            if (json.contains("captureStartIndex")) {
                capture_start_index_ = json.at("captureStartIndex").get<size_t>();
            }
            if (json.contains("recordingMode")) {
                auto const mode = json.at("recordingMode").get<std::string>();
                if (mode == "reset" || mode == "overwrite" || mode == "insert" || mode == "append") recording_mode_ = mode;
            }
            ++revision_;
            ui_dirty_ = true;
            return {.accepted = true, .revision = revision_};
        } catch (std::exception const& error) {
            return {.error_message = error.what()};
        }
    }
};

} // namespace iv
