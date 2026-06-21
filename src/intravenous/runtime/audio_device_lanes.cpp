#include <intravenous/runtime/audio_device_lanes.h>

#include <intravenous/runtime/audio_device_lane_nodes.h>
#include <intravenous/runtime/audio_device_lanes_events.h>
#include <intravenous/runtime/timeline_execution_events.h>

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace iv {
namespace {
constexpr ChannelLayout kStereoInterleaved {
    .channel_type = ChannelTypeId::stereo,
    .sample_layout = SampleStreamLayout::interleaved,
};
LaneIdAllocator g_audio_device_lane_ids;

LaneMetadata make_output_metadata()
{
    LaneMetadata metadata;
    metadata.set_unit("audio_device");
    metadata.set_unit("audio_output");
    metadata.set_unit("realtime");
    return metadata;
}

LaneMetadata make_input_metadata()
{
    LaneMetadata metadata;
    metadata.set_unit("audio_device");
    metadata.set_unit("audio_input");
    metadata.set_unit("realtime");
    return metadata;
}
} // namespace

AudioDeviceLanes::AudioDeviceLanes(
    size_t timeline_sample_rate,
    size_t timeline_block_size,
    std::optional<AudioOutputDevice> output_device,
    std::optional<AudioInputDevice> input_device)
    : timeline_sample_rate_(timeline_sample_rate),
      timeline_block_size_(timeline_block_size),
      output_device_(std::move(output_device)),
      input_device_(std::move(input_device))
{
    if (timeline_sample_rate_ == 0 || timeline_block_size_ == 0) {
        throw std::invalid_argument("audio device lanes require non-zero timeline sample rate and block size");
    }

    if (input_device_.has_value()) {
        auto const target_latency_frames = std::max(
            timeline_block_size_ * 2,
            input_device_->config().preferred_block_size * 2);
        input_synchronizer_.emplace(
            timeline_sample_rate_,
            timeline_block_size_,
            input_device_->config().sample_rate,
            target_latency_frames);
    }

    output_lane_id_ = g_audio_device_lane_ids.next();
    input_lane_id_ = g_audio_device_lane_ids.next();
    initialize_current_input_block();
    start_input_capture_thread();
}

AudioDeviceLanes::~AudioDeviceLanes()
{
    request_shutdown();
    if (input_capture_thread_.joinable()) {
        input_capture_thread_.join();
    }
}

void AudioDeviceLanes::request_shutdown()
{
    if (shutdown_requested_.exchange(true)) {
        return;
    }
    if (output_device_.has_value()) {
        output_device_->request_shutdown();
    }
    if (input_device_.has_value()) {
        input_device_->request_shutdown();
    }
}

void AudioDeviceLanes::bind()
{
    if (!output_device_.has_value()) {
        return;
    }
    initialize_timeline_lanes();
}

void AudioDeviceLanes::initialize_current_input_block()
{
    current_input_block_.channel_layout = kStereoInterleaved;
    current_input_block_.frame_count = timeline_block_size_;
    current_input_block_.samples.assign(
        sample_storage_size(kStereoInterleaved, timeline_block_size_),
        Sample {});
}

void AudioDeviceLanes::initialize_timeline_lanes()
{
    TimelineLaneBatchUpdate batch;
    batch.version_index = 1;
    batch.upserts.push_back(TimelineLaneUpsert{
        .lane = output_lane_id_,
        .make_node = [] {
            return TypeErasedLaneNode(AudioDeviceOutputLaneNode{});
        },
        .sample_channel_type = ChannelTypeId::stereo,
        .metadata = make_output_metadata(),
    });
    batch.upserts.push_back(TimelineLaneUpsert{
        .lane = input_lane_id_,
        .make_node = [lane = input_lane_id_] {
            return TypeErasedLaneNode(AudioDeviceInputLaneNode{.lane = lane});
        },
        .sample_channel_type = ChannelTypeId::stereo,
        .metadata = make_input_metadata(),
    });
    IV_INVOKE_LINKER_EVENT(
        iv_runtime_audio_device_lanes_timeline_batch_requested_event,
        batch);
}

void AudioDeviceLanes::start_input_capture_thread()
{
    if (!input_device_.has_value() || !input_synchronizer_.has_value()) {
        return;
    }
    input_capture_thread_ = std::thread([this] {
        input_capture_loop();
    });
}

void AudioDeviceLanes::input_capture_loop()
{
    while (!shutdown_requested_.load()) {
        try {
            auto block = input_device_->wait_for_captured_block();
            if (block.samples.empty()) {
                input_device_->release_captured_block();
                continue;
            }
            input_synchronizer_->push_captured_block(block);
            input_device_->release_captured_block();
        } catch (std::exception const &) {
            if (shutdown_requested_.load()) {
                return;
            }
        }
    }
}

void AudioDeviceLanes::append_output_block_locked(SampleBlockView<Sample const> view)
{
    if (view.channel_layout() == kStereoInterleaved) {
        auto const start = output_reservoir_.size();
        output_reservoir_.resize(start + view.samples().size());
        std::ranges::copy(view.samples(), output_reservoir_.begin() + static_cast<std::ptrdiff_t>(start));
        return;
    }

    std::vector<Sample> converted(sample_storage_size(kStereoInterleaved, view.frames()), Sample {});
    auto const plan = ChannelConversionRegistry::plan(view.channel_layout(), kStereoInterleaved);
    plan.convert(view.samples().data(), converted.data(), view.frames());
    auto const start = output_reservoir_.size();
    output_reservoir_.resize(start + converted.size());
    std::ranges::copy(converted, output_reservoir_.begin() + static_cast<std::ptrdiff_t>(start));
}

void AudioDeviceLanes::compact_output_reservoir_locked()
{
    if (output_reservoir_read_offset_ == 0) {
        return;
    }
    if (output_reservoir_read_offset_ >= output_reservoir_.size()) {
        output_reservoir_.clear();
        output_reservoir_read_offset_ = 0;
        return;
    }
    std::move(
        output_reservoir_.begin() + static_cast<std::ptrdiff_t>(output_reservoir_read_offset_),
        output_reservoir_.end(),
        output_reservoir_.begin());
    output_reservoir_.resize(output_reservoir_.size() - output_reservoir_read_offset_);
    output_reservoir_read_offset_ = 0;
}

bool AudioDeviceLanes::try_fill_pending_output_request_locked()
{
    if (!pending_output_request_.has_value()) {
        return false;
    }

    auto &request = *pending_output_request_;
    auto const available_samples = output_reservoir_.size() - output_reservoir_read_offset_;
    auto const remaining_samples = request.samples.size() - request.written_samples;
    auto const samples_to_copy = std::min(available_samples, remaining_samples);
    if (samples_to_copy > 0) {
        std::ranges::copy(
            std::span<Sample const>(
                output_reservoir_.data() + output_reservoir_read_offset_,
                samples_to_copy),
            request.samples.begin() + static_cast<std::ptrdiff_t>(request.written_samples));
        output_reservoir_read_offset_ += samples_to_copy;
        request.written_samples += samples_to_copy;
    }

    if (output_reservoir_read_offset_ * 2 >= output_reservoir_.size()) {
        compact_output_reservoir_locked();
    }

    return request.written_samples == request.samples.size();
}

void AudioDeviceLanes::prepare_next_input_block()
{
    OwnedSampleBlock next_block;
    if (input_synchronizer_.has_value()) {
        next_block = input_synchronizer_->render_timeline_block();
    } else {
        next_block.channel_layout = kStereoInterleaved;
        next_block.frame_count = timeline_block_size_;
        next_block.samples.assign(
            sample_storage_size(kStereoInterleaved, timeline_block_size_),
            Sample {});
    }

    std::scoped_lock lock(mutex_);
    current_input_block_ = std::move(next_block);
}

void AudioDeviceLanes::handle_task_runner_before_pass(TasksRunnerBeforePass const &)
{
    if (!output_device_.has_value() || shutdown_requested_.load()) {
        return;
    }

    while (!shutdown_requested_.load()) {
        bool should_submit_response = false;
        bool should_run_pass = false;
        std::span<Sample> new_request {};
        size_t next_start_index = 0;

        {
            std::scoped_lock lock(mutex_);
            if (!pending_output_request_.has_value()) {
                new_request = {};
            } else if (try_fill_pending_output_request_locked()) {
                pending_output_request_.reset();
                should_submit_response = true;
            } else {
                next_start_index = next_realtime_start_index_;
                should_run_pass = true;
            }
        }

        if (should_submit_response) {
            output_device_->submit_response();
            continue;
        }

        if (should_run_pass) {
            prepare_next_input_block();
            IV_INVOKE_LINKER_EVENT(
                iv_runtime_audio_device_lanes_set_realtime_start_index_event,
                next_start_index);
            return;
        }

        try {
            new_request = output_device_->wait_for_block_request();
        } catch (std::exception const &) {
            if (shutdown_requested_.load()) {
                return;
            }
            continue;
        }

        if (new_request.empty()) {
            if (shutdown_requested_.load()) {
                return;
            }
            continue;
        }

        {
            std::scoped_lock lock(mutex_);
            pending_output_request_ = PendingOutputRequest{
                .samples = new_request,
                .written_samples = 0,
            };
            if (try_fill_pending_output_request_locked()) {
                pending_output_request_.reset();
                should_submit_response = true;
            } else {
                next_start_index = next_realtime_start_index_;
                should_run_pass = true;
            }
        }

        if (should_submit_response) {
            output_device_->submit_response();
            continue;
        }

        if (should_run_pass) {
            prepare_next_input_block();
            IV_INVOKE_LINKER_EVENT(
                iv_runtime_audio_device_lanes_set_realtime_start_index_event,
                next_start_index);
            return;
        }
    }
}

void AudioDeviceLanes::handle_task_runner_after_pass(TasksRunnerAfterPass const &)
{
    if (!output_device_.has_value() || shutdown_requested_.load()) {
        return;
    }

    TimelineExecutionRealtimeSampleBlockBuilder builder;
    IV_INVOKE_SINGLETON_EVENT(
        iv_runtime_timeline_execution_realtime_sample_block_requested_event,
        output_lane_id_,
        builder);
    auto const rendered = builder.build();

    bool should_submit_response = false;
    {
        std::scoped_lock lock(mutex_);
        if (!rendered.empty()) {
            append_output_block_locked(rendered.view());
        } else {
            std::vector<Sample> silence(
                sample_storage_size(kStereoInterleaved, timeline_block_size_),
                Sample {});
            append_output_block_locked(SampleBlockView<Sample const>(
                silence,
                kStereoInterleaved,
                timeline_block_size_));
        }
        if (pending_output_request_.has_value() && try_fill_pending_output_request_locked()) {
            pending_output_request_.reset();
            should_submit_response = true;
        }
        next_realtime_start_index_ += timeline_block_size_;
    }

    if (should_submit_response) {
        output_device_->submit_response();
    }
}

BorrowedSampleBlock AudioDeviceLanes::handle_input_block_requested(LaneId lane) const
{
    if (lane != input_lane_id_) {
        return {};
    }

    std::scoped_lock lock(mutex_);
    return BorrowedSampleBlock{
        .samples = std::span<Sample const>(
            current_input_block_.samples.data(),
            current_input_block_.samples.size()),
        .channel_layout = current_input_block_.channel_layout,
        .frame_count = current_input_block_.frame_count,
    };
}

} // namespace iv
