#pragma once

#include <intravenous/devices/audio_device.h>
#include <intravenous/runtime/sample_stream_blocks.h>
#include <intravenous/third_party/miniaudio/miniaudio.h>

#include <mutex>
#include <vector>

namespace iv {

class AudioInputSynchronizer {
    size_t timeline_sample_rate_ = 48000;
    size_t timeline_block_size_ = 256;
    size_t input_sample_rate_ = 48000;
    size_t target_buffer_frames_ = 512;
    double nominal_rate_ratio_ = 1.0;
    double integral_error_ = 0.0;
    double proportional_gain_ = 0.08;
    double integral_gain_ = 0.002;
    double max_rate_correction_ = 0.02;
    double last_rate_ratio_ = 1.0;
    bool discontinuity_pending_ = true;
    ma_resampler resampler_ {};
    bool resampler_initialized_ = false;
    mutable std::mutex mutex_;
    std::vector<Sample::storage> fifo_ {};
    size_t fifo_read_frame_offset_ = 0;

    void compact_fifo_locked();

public:
    AudioInputSynchronizer(
        size_t timeline_sample_rate,
        size_t timeline_block_size,
        size_t input_sample_rate,
        size_t target_buffer_frames);
    ~AudioInputSynchronizer();

    AudioInputSynchronizer(AudioInputSynchronizer const &) = delete;
    AudioInputSynchronizer &operator=(AudioInputSynchronizer const &) = delete;
    AudioInputSynchronizer(AudioInputSynchronizer &&) = delete;
    AudioInputSynchronizer &operator=(AudioInputSynchronizer &&) = delete;

    void push_captured_block(AudioInputBlock const &block);
    OwnedSampleBlock render_timeline_block();
    size_t buffered_frames() const;
    double last_rate_ratio() const;
};

} // namespace iv
