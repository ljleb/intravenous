#include <intravenous/runtime/audio_device_lanes.h>

#include <intravenous/runtime/audio_device_lane_nodes.h>
#include <intravenous/runtime/audio_device_lanes_events.h>
#include <intravenous/runtime/timeline_execution_events.h>
#include <intravenous/runtime/uuid.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <ranges>
#include <stdexcept>

namespace iv {
namespace {
constexpr ChannelLayout kStereoInterleaved {
    .channel_type = ChannelTypeId::stereo,
    .sample_layout = SampleStreamLayout::interleaved,
};
constexpr std::uint64_t stable_lane_id_for(std::string_view key)
{
    std::uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : key) {
        hash ^= static_cast<std::uint64_t>(c);
        hash *= 1099511628211ull;
    }
    hash &= 0x7fffffffffffffffull;
    return hash == 0 ? 1ull : hash;
}

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

std::optional<AudioDeviceDescriptor> find_device_descriptor(
    std::vector<AudioDeviceDescriptor> const &devices,
    std::string const &device_id)
{
    auto const it = std::ranges::find(
        devices,
        device_id,
        &AudioDeviceDescriptor::device_id);
    if (it == devices.end()) {
        return std::nullopt;
    }
    return *it;
}

} // namespace

AudioDeviceLanes::AudioDeviceLanes(
    size_t timeline_sample_rate,
    size_t timeline_block_size,
    AudioDeviceLanesBackend backend,
    std::optional<std::string> selected_output_device_id,
    std::optional<std::string> selected_input_device_id)
    : timeline_sample_rate_(timeline_sample_rate),
      timeline_block_size_(timeline_block_size),
      backend_(std::move(backend)),
      output_render_config_(RenderConfig{
          .sample_rate = timeline_sample_rate,
          .num_channels = 2,
          .max_block_frames = std::max<size_t>(timeline_block_size * 4, 4096),
          .preferred_block_size = timeline_block_size,
      }),
      input_render_config_(RenderConfig{
          .sample_rate = timeline_sample_rate,
          .num_channels = 2,
          .max_block_frames = std::max<size_t>(timeline_block_size * 4, 4096),
          .preferred_block_size = timeline_block_size,
      }),
      selected_output_device_id_(std::move(selected_output_device_id)),
      selected_input_device_id_(std::move(selected_input_device_id)),
      output_lane_external_id_(generate_uuid_v4()),
      input_lane_external_id_(generate_uuid_v4())
{
    if (timeline_sample_rate_ == 0 || timeline_block_size_ == 0) {
        throw std::invalid_argument(
            "audio device lanes require non-zero timeline sample rate and block size");
    }
    if (!backend_.list_output_devices || !backend_.list_input_devices ||
        !backend_.make_output_device || !backend_.make_input_device) {
        throw std::invalid_argument("audio device lanes backend is incomplete");
    }

    output_lane_id_ = LaneId{stable_lane_id_for("audio_device.output")};
    input_lane_id_ = LaneId{stable_lane_id_for("audio_device.input")};
    initialize_current_input_block();
    replace_output_device(create_output_device(selected_output_device_id_));
    replace_input_device(create_input_device(selected_input_device_id_));
}

AudioDeviceLanes::~AudioDeviceLanes()
{
    request_shutdown();
    if (input_capture_thread_.joinable()) {
        input_capture_thread_.join();
    }
}

std::vector<AudioDeviceDescriptor> AudioDeviceLanes::list_output_devices_unlocked() const
{
    return backend_.list_output_devices();
}

std::vector<AudioDeviceDescriptor> AudioDeviceLanes::list_input_devices_unlocked() const
{
    return backend_.list_input_devices();
}

std::shared_ptr<AudioDeviceLanes::ActiveOutputDevice> AudioDeviceLanes::create_output_device(
    std::optional<std::string> const &device_id) const
{
    if (!device_id.has_value()) {
        return nullptr;
    }

    auto const available_devices = list_output_devices_unlocked();
    auto const descriptor = find_device_descriptor(available_devices, *device_id);
    if (!descriptor.has_value()) {
        return nullptr;
    }

    try {
        return std::make_shared<ActiveOutputDevice>(ActiveOutputDevice{
            .descriptor = *descriptor,
            .device = backend_.make_output_device(*device_id, output_render_config_),
        });
    } catch (std::exception const &) {
        return nullptr;
    }
}

std::shared_ptr<AudioDeviceLanes::ActiveInputDevice> AudioDeviceLanes::create_input_device(
    std::optional<std::string> const &device_id) const
{
    if (!device_id.has_value()) {
        return nullptr;
    }

    auto const available_devices = list_input_devices_unlocked();
    auto const descriptor = find_device_descriptor(available_devices, *device_id);
    if (!descriptor.has_value()) {
        return nullptr;
    }

    try {
        auto device = backend_.make_input_device(*device_id, input_render_config_);
        auto const target_latency_frames = std::max(
            timeline_block_size_ * 2,
            device.config().preferred_block_size * 2);
        auto synchronizer = std::make_unique<AudioInputSynchronizer>(
            timeline_sample_rate_,
            timeline_block_size_,
            device.config().sample_rate,
            target_latency_frames);
        return std::make_shared<ActiveInputDevice>(
            *descriptor,
            std::move(device),
            std::move(synchronizer));
    } catch (std::exception const &) {
        return nullptr;
    }
}

void AudioDeviceLanes::start_input_capture_thread(
    std::shared_ptr<ActiveInputDevice> input_device)
{
    if (!input_device) {
        return;
    }
    input_capture_thread_ = std::thread([this, input_device = std::move(input_device)] {
        input_capture_loop(input_device);
    });
}

void AudioDeviceLanes::input_capture_loop(std::shared_ptr<ActiveInputDevice> input_device)
{
    while (!shutdown_requested_.load()) {
        try {
            auto block = input_device->device.wait_for_captured_block();
            if (block.samples.empty()) {
                input_device->device.release_captured_block();
                continue;
            }
            input_device->synchronizer->push_captured_block(block);
            input_device->device.release_captured_block();
        } catch (std::exception const &) {
            if (shutdown_requested_.load()) {
                return;
            }
            return;
        }
    }
}

void AudioDeviceLanes::request_shutdown()
{
    if (shutdown_requested_.exchange(true)) {
        return;
    }

    std::shared_ptr<ActiveOutputDevice> output_device;
    std::shared_ptr<ActiveInputDevice> input_device;
    {
        std::scoped_lock lock(mutex_);
        output_device = active_output_device_;
        input_device = active_input_device_;
        active_output_device_.reset();
        active_input_device_.reset();
    }
    if (output_device) {
        output_device->device.request_shutdown();
    }
    if (input_device) {
        input_device->device.request_shutdown();
    }
}

void AudioDeviceLanes::bind()
{
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
        .external_id = output_lane_external_id_,
        .make_node = [] {
            return TypeErasedLaneNode(AudioDeviceOutputLaneNode{});
        },
        .sample_channel_type = ChannelTypeId::stereo,
        .metadata = make_output_metadata(),
    });
    batch.upserts.push_back(TimelineLaneUpsert{
        .lane = input_lane_id_,
        .external_id = input_lane_external_id_,
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

void AudioDeviceLanes::set_lane_external_ids(
    InternedString output_lane_external_id,
    InternedString input_lane_external_id)
{
    {
        std::scoped_lock lock(mutex_);
        output_lane_external_id_ = std::move(output_lane_external_id);
        input_lane_external_id_ = std::move(input_lane_external_id);
    }

    TimelineLaneBatchUpdate batch;
    batch.version_index = 1;
    batch.upserts.push_back(TimelineLaneUpsert{
        .lane = output_lane_id_,
        .external_id = output_lane_external_id_,
        .make_node = [] {
            return TypeErasedLaneNode(AudioDeviceOutputLaneNode{});
        },
        .sample_channel_type = ChannelTypeId::stereo,
        .metadata = make_output_metadata(),
    });
    batch.upserts.push_back(TimelineLaneUpsert{
        .lane = input_lane_id_,
        .external_id = input_lane_external_id_,
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

InternedString AudioDeviceLanes::output_lane_external_id() const
{
    std::scoped_lock lock(mutex_);
    return output_lane_external_id_;
}

InternedString AudioDeviceLanes::input_lane_external_id() const
{
    std::scoped_lock lock(mutex_);
    return input_lane_external_id_;
}

void AudioDeviceLanes::append_output_block_locked(SampleBlockView<Sample const> view)
{
    if (view.channel_layout() == kStereoInterleaved) {
        auto const start = output_reservoir_.size();
        output_reservoir_.resize(start + view.samples().size());
        std::ranges::copy(
            view.samples(),
            output_reservoir_.begin() + static_cast<std::ptrdiff_t>(start));
        return;
    }

    std::vector<Sample> converted(
        sample_storage_size(kStereoInterleaved, view.frames()),
        Sample {});
    auto const plan =
        ChannelConversionRegistry::plan(view.channel_layout(), kStereoInterleaved);
    plan.convert(view.samples().data(), converted.data(), view.frames());
    auto const start = output_reservoir_.size();
    output_reservoir_.resize(start + converted.size());
    std::ranges::copy(
        converted,
        output_reservoir_.begin() + static_cast<std::ptrdiff_t>(start));
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
        output_reservoir_.begin()
            + static_cast<std::ptrdiff_t>(output_reservoir_read_offset_),
        output_reservoir_.end(),
        output_reservoir_.begin());
    output_reservoir_.resize(
        output_reservoir_.size() - output_reservoir_read_offset_);
    output_reservoir_read_offset_ = 0;
}

bool AudioDeviceLanes::try_fill_pending_output_request_locked()
{
    if (!pending_output_request_.has_value()) {
        return false;
    }

    auto &request = *pending_output_request_;
    auto const available_samples =
        output_reservoir_.size() - output_reservoir_read_offset_;
    auto const remaining_samples =
        request.samples.size() - request.written_samples;
    auto const samples_to_copy =
        std::min(available_samples, remaining_samples);
    if (samples_to_copy > 0) {
        std::ranges::copy(
            std::span<Sample const>(
                output_reservoir_.data() + output_reservoir_read_offset_,
                samples_to_copy),
            request.samples.begin()
                + static_cast<std::ptrdiff_t>(request.written_samples));
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
    std::shared_ptr<ActiveInputDevice> input_device;
    {
        std::scoped_lock lock(mutex_);
        input_device = active_input_device_;
    }

    if (input_device && input_device->synchronizer) {
        next_block = input_device->synchronizer->render_timeline_block();
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

AudioDeviceSelectionState AudioDeviceLanes::selection_state_for(
    std::optional<std::string> const &selected_id,
    std::shared_ptr<ActiveOutputDevice> const &active_device,
    std::vector<AudioDeviceDescriptor> const &available_devices) const
{
    AudioDeviceSelectionState selection;
    selection.device_id = selected_id;
    selection.available = static_cast<bool>(active_device);
    if (active_device) {
        selection.name = active_device->descriptor.name;
        return selection;
    }
    if (selected_id.has_value()) {
        if (auto descriptor = find_device_descriptor(available_devices, *selected_id);
            descriptor.has_value()) {
            selection.name = descriptor->name;
        }
    }
    return selection;
}

AudioDeviceSelectionState AudioDeviceLanes::selection_state_for(
    std::optional<std::string> const &selected_id,
    std::shared_ptr<ActiveInputDevice> const &active_device,
    std::vector<AudioDeviceDescriptor> const &available_devices) const
{
    AudioDeviceSelectionState selection;
    selection.device_id = selected_id;
    selection.available = static_cast<bool>(active_device);
    if (active_device) {
        selection.name = active_device->descriptor.name;
        return selection;
    }
    if (selected_id.has_value()) {
        if (auto descriptor = find_device_descriptor(available_devices, *selected_id);
            descriptor.has_value()) {
            selection.name = descriptor->name;
        }
    }
    return selection;
}

AudioDevicesSnapshot AudioDeviceLanes::audio_devices_snapshot() const
{
    AudioDevicesSnapshot snapshot;
    std::shared_ptr<ActiveOutputDevice> active_output;
    std::shared_ptr<ActiveInputDevice> active_input;
    std::optional<std::string> selected_output;
    std::optional<std::string> selected_input;
    {
        std::scoped_lock lock(mutex_);
        active_output = active_output_device_;
        active_input = active_input_device_;
        selected_output = selected_output_device_id_;
        selected_input = selected_input_device_id_;
    }

    snapshot.output_devices = list_output_devices_unlocked();
    snapshot.input_devices = list_input_devices_unlocked();
    snapshot.selected_output =
        selection_state_for(selected_output, active_output, snapshot.output_devices);
    snapshot.selected_input =
        selection_state_for(selected_input, active_input, snapshot.input_devices);
    return snapshot;
}

void AudioDeviceLanes::replace_output_device(
    std::shared_ptr<ActiveOutputDevice> output_device)
{
    std::shared_ptr<ActiveOutputDevice> old_output;
    std::optional<std::string> selected_output_device_id;
    {
        std::scoped_lock lock(mutex_);
        old_output = std::move(active_output_device_);
        active_output_device_ = std::move(output_device);
        selected_output_device_id = selected_output_device_id_;
        output_reservoir_.clear();
        output_reservoir_read_offset_ = 0;
        pending_output_request_.reset();
        logged_missing_output_device_ = false;
        logged_first_output_request_ = false;
        logged_first_non_silent_output_ = false;
        silent_output_pass_count_ = 0;
        if (pass_output_device_ == old_output) {
            pass_output_device_.reset();
        }
    }
    if (old_output) {
        old_output->device.request_shutdown();
    }
    (void)selected_output_device_id;
}

void AudioDeviceLanes::replace_input_device(
    std::shared_ptr<ActiveInputDevice> input_device)
{
    std::shared_ptr<ActiveInputDevice> old_input;
    std::shared_ptr<ActiveInputDevice> new_input;
    std::thread old_thread;
    std::optional<std::string> selected_input_device_id;
    {
        std::scoped_lock lock(mutex_);
        old_input = std::move(active_input_device_);
        active_input_device_ = input_device;
        new_input = active_input_device_;
        selected_input_device_id = selected_input_device_id_;
        if (input_capture_thread_.joinable()) {
            old_thread = std::move(input_capture_thread_);
        }
        initialize_current_input_block();
    }

    if (old_input) {
        old_input->device.request_shutdown();
    }
    if (old_thread.joinable()) {
        old_thread.join();
    }
    start_input_capture_thread(std::move(input_device));
    (void)new_input;
    (void)selected_input_device_id;
}

AudioDevicesSnapshot AudioDeviceLanes::set_selected_devices(
    std::optional<std::string> output_device_id,
    std::optional<std::string> input_device_id)
{
    {
        std::scoped_lock lock(mutex_);
        selected_output_device_id_ = std::move(output_device_id);
        selected_input_device_id_ = std::move(input_device_id);
    }

    replace_output_device(create_output_device(selected_output_device_id_));
    replace_input_device(create_input_device(selected_input_device_id_));
    return audio_devices_snapshot();
}

void AudioDeviceLanes::handle_task_runner_before_pass(TasksRunnerBeforePass const &)
{
    if (shutdown_requested_.load()) {
        return;
    }

    for (;;) {
        std::shared_ptr<ActiveOutputDevice> output_device;
        {
            std::scoped_lock lock(mutex_);
            output_device = active_output_device_;
            if (!output_device && !logged_missing_output_device_) {
                logged_missing_output_device_ = true;
            }
        }
        if (!output_device) {
            return;
        }

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
                pass_output_device_ = output_device;
                next_start_index = next_realtime_start_index_;
                should_run_pass = true;
            }
        }

        if (should_submit_response) {
            output_device->device.submit_response();
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
            new_request = output_device->device.wait_for_block_request();
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
            if (active_output_device_ != output_device) {
                continue;
            }
            pending_output_request_ = PendingOutputRequest{
                .samples = new_request,
                .written_samples = 0,
            };
            if (try_fill_pending_output_request_locked()) {
                pending_output_request_.reset();
                should_submit_response = true;
            } else {
                pass_output_device_ = output_device;
                next_start_index = next_realtime_start_index_;
                should_run_pass = true;
            }
        }

        if (!logged_first_output_request_) {
            logged_first_output_request_ = true;
        }

        if (should_submit_response) {
            output_device->device.submit_response();
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
    if (shutdown_requested_.load()) {
        return;
    }

    std::shared_ptr<ActiveOutputDevice> pass_output_device;
    std::shared_ptr<ActiveOutputDevice> current_output_device;
    {
        std::scoped_lock lock(mutex_);
        pass_output_device = pass_output_device_;
        current_output_device = active_output_device_;
    }
    if (!pass_output_device || pass_output_device != current_output_device) {
        std::scoped_lock lock(mutex_);
        pass_output_device_.reset();
        return;
    }

    TimelineExecutionRealtimeSampleBlockBuilder builder;
    IV_INVOKE_SINGLETON_EVENT(
        iv_runtime_timeline_execution_realtime_sample_block_requested_event,
        output_lane_id_,
        builder);
    auto const rendered = builder.build();
    Sample::storage peak = 0.0f;
    for (auto const &sample : rendered.samples) {
        peak = std::max<Sample::storage>(peak, std::abs(sample.value));
    }

    bool should_submit_response = false;
    {
        std::scoped_lock lock(mutex_);
        if (pass_output_device_ != pass_output_device ||
            active_output_device_ != pass_output_device) {
            return;
        }

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
        if (peak > 0.0f) {
            if (!logged_first_non_silent_output_) {
                logged_first_non_silent_output_ = true;
            }
            silent_output_pass_count_ = 0;
        } else {
            ++silent_output_pass_count_;
        }
        if (pending_output_request_.has_value()
            && try_fill_pending_output_request_locked()) {
            pending_output_request_.reset();
            should_submit_response = true;
        }
        next_realtime_start_index_ += timeline_block_size_;
        pass_output_device_.reset();
    }

    if (should_submit_response) {
        try {
            pass_output_device->device.submit_response();
        } catch (std::exception const &) {
        }
    }
}

OwnedSampleBlock AudioDeviceLanes::handle_input_block_requested(LaneId lane) const
{
    if (lane != input_lane_id_) {
        return {};
    }

    std::scoped_lock lock(mutex_);
    return current_input_block_;
}

} // namespace iv
