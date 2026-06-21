#include <intravenous/runtime/audio_input_synchronizer.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace iv {
namespace {
constexpr ChannelLayout kStereoInterleaved {
    .channel_type = ChannelTypeId::stereo,
    .sample_layout = SampleStreamLayout::interleaved,
};
}

AudioInputSynchronizer::AudioInputSynchronizer(
    size_t timeline_sample_rate,
    size_t timeline_block_size,
    size_t input_sample_rate,
    size_t target_buffer_frames)
    : timeline_sample_rate_(timeline_sample_rate),
      timeline_block_size_(timeline_block_size),
      input_sample_rate_(input_sample_rate),
      target_buffer_frames_(std::max(target_buffer_frames, timeline_block_size))
{
    if (timeline_sample_rate_ == 0 || timeline_block_size_ == 0 || input_sample_rate_ == 0) {
        throw std::invalid_argument("audio input synchronizer requires non-zero rates and block size");
    }

    nominal_rate_ratio_ =
        static_cast<double>(input_sample_rate_) / static_cast<double>(timeline_sample_rate_);
    last_rate_ratio_ = nominal_rate_ratio_;

    auto config = ma_resampler_config_init(
        ma_format_f32,
        static_cast<ma_uint32>(channel_count(kStereoInterleaved)),
        static_cast<ma_uint32>(input_sample_rate_),
        static_cast<ma_uint32>(timeline_sample_rate_),
        ma_resample_algorithm_linear);
    if (ma_resampler_init(&config, nullptr, &resampler_) != MA_SUCCESS) {
        throw std::runtime_error("failed to initialize audio input resampler");
    }
    resampler_initialized_ = true;
}

AudioInputSynchronizer::~AudioInputSynchronizer()
{
    if (resampler_initialized_) {
        ma_resampler_uninit(&resampler_, nullptr);
    }
}

void AudioInputSynchronizer::compact_fifo_locked()
{
    if (fifo_read_frame_offset_ == 0) {
        return;
    }

    size_t const remaining_frames =
        fifo_.size() / channel_count(kStereoInterleaved) - fifo_read_frame_offset_;
    if (remaining_frames == 0) {
        fifo_.clear();
        fifo_read_frame_offset_ = 0;
        return;
    }

    auto const read_sample_offset = fifo_read_frame_offset_ * channel_count(kStereoInterleaved);
    std::move(
        fifo_.begin() + static_cast<std::ptrdiff_t>(read_sample_offset),
        fifo_.end(),
        fifo_.begin());
    fifo_.resize(remaining_frames * channel_count(kStereoInterleaved));
    fifo_read_frame_offset_ = 0;
}

void AudioInputSynchronizer::push_captured_block(AudioInputBlock const &block)
{
    if (block.samples.empty()) {
        return;
    }

    std::scoped_lock lock(mutex_);
    if (block.discontinuous) {
        fifo_.clear();
        fifo_read_frame_offset_ = 0;
        discontinuity_pending_ = true;
        integral_error_ = 0.0;
    }

    auto const start = fifo_.size();
    fifo_.resize(start + block.samples.size());
    for (size_t i = 0; i < block.samples.size(); ++i) {
        fifo_[start + i] = block.samples[i].value;
    }
}

OwnedSampleBlock AudioInputSynchronizer::render_timeline_block()
{
    std::vector<Sample::storage> output_storage(
        timeline_block_size_ * channel_count(kStereoInterleaved),
        0.0f);

    {
        std::scoped_lock lock(mutex_);
        if (discontinuity_pending_) {
            ma_resampler_reset(&resampler_);
            discontinuity_pending_ = false;
        }

        size_t const available_frames =
            fifo_.size() / channel_count(kStereoInterleaved) - fifo_read_frame_offset_;
        if (available_frames > 0) {
            auto const normalized_error =
                (static_cast<double>(available_frames) - static_cast<double>(target_buffer_frames_))
                / static_cast<double>(target_buffer_frames_);
            integral_error_ = std::clamp(integral_error_ + normalized_error, -32.0, 32.0);
            auto const correction =
                std::clamp(
                    proportional_gain_ * normalized_error + integral_gain_ * integral_error_,
                    -max_rate_correction_,
                    max_rate_correction_);
            last_rate_ratio_ = nominal_rate_ratio_ * (1.0 + correction);
            if (ma_resampler_set_rate_ratio(
                    &resampler_,
                    static_cast<float>(last_rate_ratio_))
                != MA_SUCCESS) {
                throw std::runtime_error("failed to update audio input resampler ratio");
            }

            auto *input =
                fifo_.data() + fifo_read_frame_offset_ * channel_count(kStereoInterleaved);
            ma_uint64 input_frames = available_frames;
            ma_uint64 output_frames = timeline_block_size_;
            if (ma_resampler_process_pcm_frames(
                    &resampler_,
                    input,
                    &input_frames,
                    output_storage.data(),
                    &output_frames)
                != MA_SUCCESS) {
                throw std::runtime_error("failed to resample captured audio input");
            }

            fifo_read_frame_offset_ += static_cast<size_t>(input_frames);
            if (fifo_read_frame_offset_ * 2 >= fifo_.size()) {
                compact_fifo_locked();
            }
        } else {
            last_rate_ratio_ = nominal_rate_ratio_;
        }
    }

    OwnedSampleBlock block;
    block.channel_layout = kStereoInterleaved;
    block.frame_count = timeline_block_size_;
    block.samples.resize(output_storage.size());
    for (size_t i = 0; i < output_storage.size(); ++i) {
        block.samples[i] = output_storage[i];
    }
    return block;
}

size_t AudioInputSynchronizer::buffered_frames() const
{
    std::scoped_lock lock(mutex_);
    return fifo_.size() / channel_count(kStereoInterleaved) - fifo_read_frame_offset_;
}

double AudioInputSynchronizer::last_rate_ratio() const
{
    std::scoped_lock lock(mutex_);
    return last_rate_ratio_;
}

} // namespace iv
