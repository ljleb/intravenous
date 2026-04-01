#pragma once

#include "compat.h"
#include "devices/audio_device.h"
#include "ports.h"
#include "wav.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <memory>
#include <optional>
#include <condition_variable>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace iv {
    class ExecutionTargetRegistry;
    class AudioDeviceExecutionTarget;
    class WavFileExecutionTarget;

    class ExecutionTargetRegistrar {
        ExecutionTargetRegistry* _registry;
        size_t _executor_id;

    public:
        ExecutionTargetRegistrar(ExecutionTargetRegistry* registry, size_t executor_id)
        : _registry(registry)
        , _executor_id(executor_id)
        {}

        explicit operator bool() const
        {
            return _registry != nullptr;
        }

        AudioDeviceExecutionTarget& audio_device(size_t device_id, size_t channel) const;
        WavFileExecutionTarget& file(std::filesystem::path const& path, size_t channel) const;
    };

    class AudioDeviceExecutionTarget {
    private:
        struct SinkEntry {
            size_t channel = 0;
            std::span<Sample> buffer;
        };

        size_t _device_id;
        RenderConfig _config;
        std::vector<SinkEntry> _sinks;
        std::vector<size_t> _channel_sink_counts;
        std::vector<Sample> _interleaved_block;

    public:
        AudioDeviceExecutionTarget(
            size_t device_id,
            RenderConfig config
        )
        : _device_id(std::move(device_id))
        , _config(std::move(config))
        {
            validate_render_config(_config);
            _interleaved_block.assign(_config.num_channels * _config.max_block_frames, 0.0f);
        }

        size_t device_id() const
        {
            return _device_id;
        }

        size_t preferred_block_size() const
        {
            return _config.preferred_block_size;
        }

        void register_sink(size_t channel, std::span<Sample> buffer)
        {
            validate_channel(channel);
            ensure_channel(channel);
            auto [begin, end] = channel_range(channel);
            for (size_t i = begin; i < end; ++i) {
                if (same_span(_sinks[i].buffer, buffer)) {
                    return;
                }
            }
            _sinks.insert(_sinks.begin() + static_cast<std::ptrdiff_t>(end), SinkEntry{ channel, buffer });
            ++_channel_sink_counts[channel];
        }

        void update_sink(size_t channel, std::span<Sample> previous_buffer, std::span<Sample> buffer)
        {
            validate_channel(channel);
            ensure_channel(channel);
            auto [begin, end] = channel_range(channel);
            for (size_t i = begin; i < end; ++i) {
                if (same_span(_sinks[i].buffer, previous_buffer)) {
                    _sinks[i].buffer = buffer;
                    return;
                }
            }

            register_sink(channel, buffer);
        }

        void unregister_sink(size_t channel, std::span<Sample> buffer)
        {
            if (channel >= _channel_sink_counts.size()) {
                return;
            }
            auto [begin, end] = channel_range(channel);
            for (size_t i = begin; i < end; ++i) {
                if (same_span(_sinks[i].buffer, buffer)) {
                    _sinks.erase(_sinks.begin() + static_cast<std::ptrdiff_t>(i));
                    --_channel_sink_counts[channel];
                    return;
                }
            }
        }

        template<typename A>
        static bool same_span(std::span<A> const& a, std::span<A> const& b)
        {
            return a.data() == b.data() && a.size() == b.size();
        }

        void begin_block(size_t block_index, size_t block_size)
        {
            clear_sinks(block_index, block_size);
        }

        void end_block(size_t block_index, size_t block_size)
        {
            mix_chunk(block_index, block_size);
        }

        std::span<Sample const> mixed_block(size_t frames) const
        {
            return { _interleaved_block.data(), frames * _config.num_channels };
        }

    private:
        void validate_channel(size_t channel) const
        {
            if (channel >= _config.num_channels) {
                throw std::logic_error("AudioDeviceExecutionTarget channel exceeds device channel count");
            }
        }

        void ensure_channel(size_t channel)
        {
            if (_channel_sink_counts.size() <= channel) {
                _channel_sink_counts.resize(channel + 1, 0);
            }
        }

        std::pair<size_t, size_t> channel_range(size_t channel) const
        {
            size_t begin = 0;
            for (size_t i = 0; i < channel; ++i) {
                begin += _channel_sink_counts[i];
            }
            return { begin, begin + _channel_sink_counts[channel] };
        }

        void clear_sinks(size_t block_start, size_t frames)
        {
            for (auto const& sink_entry : _sinks) {
                auto const& sink = sink_entry.buffer;
                if (sink.empty()) {
                    continue;
                }
                for (size_t frame = 0; frame < frames; ++frame) {
                    sink[(block_start + frame) & (sink.size() - 1)] = 0.0f;
                }
            }
        }

        void mix_chunk(size_t block_start, size_t frames)
        {
            size_t const channel_count = _config.num_channels;
            if (_interleaved_block.size() < frames * channel_count) {
                _interleaved_block.resize(frames * channel_count, 0.0f);
            }

            std::fill_n(_interleaved_block.data(), frames * channel_count, 0.0f);

            for (size_t channel = 0; channel < std::min(_channel_sink_counts.size(), channel_count); ++channel) {
                auto [begin, end] = channel_range(channel);
                for (size_t i = begin; i < end; ++i) {
                    auto const& sink = _sinks[i].buffer;
                    if (sink.empty()) {
                        continue;
                    }
                    for (size_t frame = 0; frame < frames; ++frame) {
                        size_t const global_index = block_start + frame;
                        _interleaved_block[frame * channel_count + channel] += sink[global_index & (sink.size() - 1)];
                    }
                }
            }

        }
    };

    class WavFileExecutionTarget {
        struct SinkEntry {
            size_t channel = 0;
            std::span<Sample> buffer;
        };

        std::filesystem::path _path;
        size_t _sample_rate;
        std::vector<SinkEntry> _sinks;
        std::vector<size_t> _channel_sink_counts;
        std::vector<std::vector<Sample>> _channel_data;

    public:
        WavFileExecutionTarget(std::filesystem::path path, size_t sample_rate)
        : _path(std::move(path))
        , _sample_rate(sample_rate)
        {}

        ~WavFileExecutionTarget() noexcept
        {
            try {
                flush_to_disk();
            } catch (std::exception const& e) {
                diagnostic_stream() << "WavFileExecutionTarget flush failed: " << e.what() << '\n';
            } catch (...) {
                diagnostic_stream() << "WavFileExecutionTarget flush failed with unknown exception\n";
            }
        }

        size_t preferred_block_size() const
        {
            return MAX_BLOCK_SIZE;
        }

        std::filesystem::path const& path() const
        {
            return _path;
        }

        void begin_block(size_t block_index, size_t block_size)
        {
            for (auto const& sink_entry : _sinks) {
                auto const& sink = sink_entry.buffer;
                if (sink.empty()) {
                    continue;
                }
                for (size_t frame = 0; frame < block_size; ++frame) {
                    sink[(block_index + frame) & (sink.size() - 1)] = 0.0f;
                }
            }
        }

        void end_block(size_t block_index, size_t block_size)
        {
            size_t channel_count = _channel_sink_counts.size();
            if (channel_count == 0) {
                return;
            }

            size_t const block_end = block_index + block_size;
            for (size_t channel = 0; channel < channel_count; ++channel) {
                auto& output = ensure_channel_data(channel);
                if (output.size() < block_end) {
                    output.resize(block_end, 0.0f);
                }

                auto [begin, end] = channel_range(channel);
                for (size_t i = begin; i < end; ++i) {
                    auto const& sink = _sinks[i].buffer;
                    if (sink.empty()) {
                        continue;
                    }
                    for (size_t frame = 0; frame < block_size; ++frame) {
                        size_t const global_index = block_index + frame;
                        output[global_index] += sink[global_index & (sink.size() - 1)];
                    }
                }
            }
        }

        void register_sink(size_t channel, std::span<Sample> buffer)
        {
            ensure_channel(channel);
            auto [begin, end] = channel_range(channel);
            for (size_t i = begin; i < end; ++i) {
                if (AudioDeviceExecutionTarget::same_span(_sinks[i].buffer, buffer)) {
                    return;
                }
            }
            _sinks.insert(_sinks.begin() + static_cast<std::ptrdiff_t>(end), SinkEntry{ channel, buffer });
            ++_channel_sink_counts[channel];
        }

        void update_sink(size_t channel, std::span<Sample> previous_buffer, std::span<Sample> buffer)
        {
            ensure_channel(channel);
            auto [begin, end] = channel_range(channel);
            for (size_t i = begin; i < end; ++i) {
                if (AudioDeviceExecutionTarget::same_span(_sinks[i].buffer, previous_buffer)) {
                    _sinks[i].buffer = buffer;
                    return;
                }
            }
            _sinks.insert(_sinks.begin() + static_cast<std::ptrdiff_t>(end), SinkEntry{ channel, buffer });
            ++_channel_sink_counts[channel];
        }

        void unregister_sink(size_t channel, std::span<Sample> buffer)
        {
            if (channel >= _channel_sink_counts.size()) {
                return;
            }
            auto [begin, end] = channel_range(channel);
            for (size_t i = begin; i < end; ++i) {
                if (AudioDeviceExecutionTarget::same_span(_sinks[i].buffer, buffer)) {
                    _sinks.erase(_sinks.begin() + static_cast<std::ptrdiff_t>(i));
                    --_channel_sink_counts[channel];
                    return;
                }
            }
        }

    private:
        void ensure_channel(size_t channel)
        {
            if (_channel_sink_counts.size() <= channel) {
                _channel_sink_counts.resize(channel + 1, 0);
            }
            if (_channel_data.size() <= channel) {
                _channel_data.resize(channel + 1);
            }
        }

        std::pair<size_t, size_t> channel_range(size_t channel) const
        {
            size_t begin = 0;
            for (size_t i = 0; i < channel; ++i) {
                begin += _channel_sink_counts[i];
            }
            return { begin, begin + _channel_sink_counts[channel] };
        }

        std::vector<Sample>& ensure_channel_data(size_t channel)
        {
            ensure_channel(channel);
            return _channel_data[channel];
        }

        void flush_to_disk() const
        {
            size_t channel_count = _channel_data.size();
            if (channel_count == 0) {
                return;
            }

            size_t frame_count = 0;
            for (auto const& channel : _channel_data) {
                frame_count = std::max(frame_count, channel.size());
            }
            if (frame_count == 0) {
                return;
            }

            if (_path.has_parent_path()) {
                std::filesystem::create_directories(_path.parent_path());
            }
            if (channel_count > 2) {
                throw std::logic_error("WavFileExecutionTarget currently supports up to 2 channels");
            }

            std::vector<Sample> left(frame_count, 0.0f);
            std::vector<Sample> right(frame_count, 0.0f);

            if (!_channel_data.empty()) {
                auto const& source = _channel_data[0];
                std::copy(source.begin(), source.end(), left.begin());
            }
            if (_channel_data.size() >= 2) {
                auto const& source = _channel_data[1];
                std::copy(source.begin(), source.end(), right.begin());
            }

            write_wav(_path.string(), left, right, static_cast<std::uint32_t>(_sample_rate));
        }
    };

    class BufferedAudioDeviceTarget {
        struct PendingBlock {
            bool occupied = false;
            size_t block_index = 0;
            size_t block_size = 0;
            size_t expected_contributors = 0;
            size_t submitted_count = 0;
            uint64_t submission_epoch = 1;
        };

        LogicalAudioDevice* _device = nullptr;
        std::vector<Sample> _buffered_audio;
        std::vector<Sample> _delivery_block;
        size_t _buffer_capacity_frames = 0;
        size_t _buffer_start_frame = 0;
        size_t _buffered_frames = 0;
        std::optional<std::pair<size_t, size_t>> _submitted_request;
        size_t _pending_capacity_frames = 0;
        std::optional<size_t> _next_append_block_index;
        std::vector<PendingBlock> _pending_blocks;
        std::vector<uint64_t> _target_submission_epochs;
        uint64_t _next_submission_epoch = 1;

    public:
        explicit BufferedAudioDeviceTarget(LogicalAudioDevice& device)
        : _device(&device)
        {
            auto const& config = _device->config();
            _delivery_block.assign(config.max_block_frames * config.num_channels, 0.0f);
            _buffer_capacity_frames = std::bit_ceil(std::max(config.max_block_frames * 8, config.preferred_block_size * 8));
            _buffered_audio.assign(_buffer_capacity_frames * config.num_channels, 0.0f);
            _pending_capacity_frames = _buffer_capacity_frames;
            _pending_blocks.resize(_pending_capacity_frames);
        }

        void clear()
        {
            _buffer_start_frame = 0;
            _buffered_frames = 0;
            _submitted_request.reset();
            _next_append_block_index.reset();
            std::fill(_buffered_audio.begin(), _buffered_audio.end(), 0.0f);
            std::fill(_delivery_block.begin(), _delivery_block.end(), 0.0f);
            for (auto& pending : _pending_blocks) {
                pending.occupied = false;
                pending.block_index = 0;
                pending.block_size = 0;
                pending.expected_contributors = 0;
                pending.submitted_count = 0;
                pending.submission_epoch = 1;
            }
            std::fill(_target_submission_epochs.begin(), _target_submission_epochs.end(), 0);
            _next_submission_epoch = 1;
        }

        LogicalAudioDevice& device()
        {
            return *_device;
        }

        void accept_contribution(
            size_t target_index,
            size_t block_index,
            size_t block_size,
            size_t expected_contributors,
            std::span<Sample const> contribution
        )
        {
            auto const& config = _device->config();
            size_t const mixed_samples = block_size * config.num_channels;

            if (block_size > config.max_block_frames) {
                throw std::logic_error("BufferedAudioDeviceTarget block size exceeds device max block size");
            }
            if (contribution.size() != mixed_samples) {
                throw std::logic_error("BufferedAudioDeviceTarget mixed block size mismatch");
            }

            auto& pending = pending_slot(block_index, block_size);
            if (!pending.occupied) {
                if (block_index + block_size > _buffer_start_frame + _buffer_capacity_frames) {
                    throw std::logic_error("BufferedAudioDeviceTarget pending block exceeds device buffer capacity");
                }
                pending.occupied = true;
                pending.block_index = block_index;
                pending.block_size = block_size;
                pending.expected_contributors = expected_contributors;
                pending.submitted_count = 0;
                pending.submission_epoch = next_submission_epoch();
                clear_block_region(block_index, block_size);
            } else if (pending.block_size != block_size || pending.block_index != block_index) {
                throw std::logic_error("BufferedAudioDeviceTarget pending block slot collision");
            }

            if (_target_submission_epochs.size() <= target_index) {
                _target_submission_epochs.resize(target_index + 1, 0);
            }
            if (_target_submission_epochs[target_index] == pending.submission_epoch) {
                return;
            }

            _target_submission_epochs[target_index] = pending.submission_epoch;
            ++pending.submitted_count;

            accumulate_block_region(block_index, block_size, contribution);

            if (pending.submitted_count == pending.expected_contributors) {
                append_ready_blocks();
                try_submit_requested_block();
            }
        }

        bool needs_graph_frames(size_t graph_block_size) const
        {
            auto requested = _device->requested_block();
            if (!requested.has_value()) {
                return false;
            }

            if (_submitted_request.has_value() && *_submitted_request == *requested) {
                return _buffered_frames < graph_block_size;
            }

            if (requested->first < _buffer_start_frame) {
                return false;
            }

            size_t const request_offset = requested->first - _buffer_start_frame;
            size_t const available_frames = request_offset < _buffered_frames ? (_buffered_frames - request_offset) : 0;
            size_t const low_water_frames = requested->second + graph_block_size;
            return available_frames < low_water_frames;
        }

        void try_submit_requested_block()
        {
            auto requested = _device->requested_block();
            if (!requested.has_value()) {
                _submitted_request.reset();
                return;
            }

            if (_submitted_request.has_value() && *_submitted_request == *requested) {
                return;
            }

            auto const [frame_index, frame_count] = *requested;
            auto const& config = _device->config();
            if (frame_count > config.max_block_frames) {
                throw std::logic_error("BufferedAudioDeviceTarget requested block exceeds device max block size");
            }
            if (frame_index < _buffer_start_frame) {
                return;
            }

            size_t const request_offset = frame_index - _buffer_start_frame;
            if (request_offset + frame_count > _buffered_frames) {
                return;
            }

            size_t const channels = config.num_channels;
            size_t const samples = frame_count * channels;
            size_t const source_frame = (_buffer_start_frame + request_offset) & (_buffer_capacity_frames - 1);
            auto const source = make_block_view(
                std::span<Sample>(_buffered_audio),
                source_frame * channels,
                samples
            );

            std::span<Sample const> submission;
            if (source.second.empty()) {
                submission = source.first;
            } else {
                auto destination = make_block_view(
                    std::span<Sample>(_delivery_block).first(samples),
                    0,
                    samples
                );
                source.copy_to(destination);
                submission = std::span<Sample const>(_delivery_block.data(), samples);
            }

            discard_frames(request_offset + frame_count);
            _device->submit_mixed_block(
                frame_index,
                submission,
                frame_count,
                channels
            );
            _submitted_request = requested;
        }

    private:
        void append_ready_blocks()
        {
            if (!_next_append_block_index.has_value()) {
                _next_append_block_index = earliest_pending_block_index();
            }

            while (_next_append_block_index.has_value()) {
                auto& pending = pending_slot(*_next_append_block_index, 1);
                if (!pending.occupied || pending.block_index != *_next_append_block_index) {
                    return;
                }
                if (pending.submitted_count != pending.expected_contributors) {
                    return;
                }

                size_t const buffered_end = _buffer_start_frame + _buffered_frames;
                if (_buffered_frames == 0) {
                    _buffer_start_frame = pending.block_index;
                } else if (pending.block_index != buffered_end) {
                    return;
                }

                if (_buffered_frames + pending.block_size > _buffer_capacity_frames) {
                    throw std::logic_error("BufferedAudioDeviceTarget overflowed its device buffer");
                }

                _buffered_frames += pending.block_size;
                pending.occupied = false;
                pending.submitted_count = 0;
                pending.expected_contributors = 0;
                *_next_append_block_index += pending.block_size;
                if (!has_pending_block(*_next_append_block_index)) {
                    _next_append_block_index = earliest_pending_block_index();
                }
            }
        }

        void discard_frames(size_t frame_count)
        {
            if (frame_count > _buffered_frames) {
                throw std::logic_error("BufferedAudioDeviceTarget cannot discard more frames than it has buffered");
            }
            _buffer_start_frame += frame_count;
            _buffered_frames -= frame_count;
        }

        void clear_block_region(size_t block_index, size_t block_size)
        {
            size_t const channels = _device->config().num_channels;
            for (size_t frame = 0; frame < block_size; ++frame) {
                size_t const destination_frame = block_index + frame;
                size_t const destination_offset = (destination_frame & (_buffer_capacity_frames - 1)) * channels;
                std::fill_n(_buffered_audio.data() + destination_offset, channels, 0.0f);
            }
        }

        void accumulate_block_region(size_t block_index, size_t block_size, std::span<Sample const> contribution)
        {
            size_t const channels = _device->config().num_channels;
            for (size_t frame = 0; frame < block_size; ++frame) {
                size_t const destination_frame = block_index + frame;
                size_t const destination_offset = (destination_frame & (_buffer_capacity_frames - 1)) * channels;
                size_t const source_offset = frame * channels;
                for (size_t channel = 0; channel < channels; ++channel) {
                    _buffered_audio[destination_offset + channel] += contribution[source_offset + channel];
                }
            }
        }

        PendingBlock& pending_slot(size_t block_index, size_t block_size)
        {
            if (block_size == 0) {
                throw std::logic_error("BufferedAudioDeviceTarget block size must be non-zero");
            }

            size_t const slot_index = (block_index & (_pending_capacity_frames - 1));
            auto& pending = _pending_blocks[slot_index];
            if (pending.occupied && pending.block_index != block_index) {
                throw std::logic_error("BufferedAudioDeviceTarget pending block ring overflow");
            }
            return pending;
        }

        bool has_pending_block(size_t block_index) const
        {
            size_t const slot_index = (block_index & (_pending_capacity_frames - 1));
            auto const& pending = _pending_blocks[slot_index];
            return pending.occupied && pending.block_index == block_index;
        }

        std::optional<size_t> earliest_pending_block_index() const
        {
            std::optional<size_t> earliest;
            for (auto const& pending : _pending_blocks) {
                if (!pending.occupied) {
                    continue;
                }
                if (!earliest.has_value() || pending.block_index < *earliest) {
                    earliest = pending.block_index;
                }
            }
            return earliest;
        }

        uint64_t next_submission_epoch()
        {
            ++_next_submission_epoch;
            if (_next_submission_epoch == 0) {
                _next_submission_epoch = 1;
                std::fill(_target_submission_epochs.begin(), _target_submission_epochs.end(), 0);
            }
            return _next_submission_epoch;
        }
    };

    class ExecutionTargetRegistry {
    public:
        struct AudioDeviceProvider {
            void* owner;
            LogicalAudioDevice* (*device_fn)(void* owner, size_t device_id);
            size_t (*preferred_block_size_fn)(void* owner) = nullptr;
        };

    private:
        AudioDeviceProvider _audio_device_provider;
        RequestNotification _request_notification;
        size_t _sample_rate = 48000;
        mutable std::mutex _mutex;
        std::unordered_set<size_t> _registered_executors;
        std::unordered_map<size_t, size_t> _executor_next_block_indices;
        std::optional<size_t> _shared_executor_block_size;
        std::vector<std::shared_ptr<AudioDeviceExecutionTarget>> _audio_device_targets;
        std::vector<bool> _audio_device_target_active;
        std::unordered_map<std::uint64_t, size_t> _audio_device_target_indices;
        std::unordered_map<size_t, std::vector<size_t>> _device_audio_target_indices;
        std::unordered_map<size_t, std::vector<size_t>> _executor_audio_target_indices;
        std::unordered_map<size_t, std::vector<size_t>> _executor_audio_device_ids;
        std::unordered_map<size_t, size_t> _device_active_audio_target_counts;
        std::unordered_map<size_t, LogicalAudioDevice*> _devices;
        std::unordered_map<size_t, BufferedAudioDeviceTarget> _buffered_audio_device_targets;
        std::vector<std::shared_ptr<WavFileExecutionTarget>> _file_targets;
        std::vector<bool> _file_target_active;
        std::unordered_map<size_t, std::vector<size_t>> _executor_file_target_indices;

    public:
        explicit ExecutionTargetRegistry(AudioDeviceProvider audio_device_provider, size_t sample_rate = 48000, RequestNotification request_notification = std::make_shared<std::condition_variable>())
        {
            _audio_device_provider = audio_device_provider;
            _request_notification = std::move(request_notification);
            _sample_rate = sample_rate;
            if (!_audio_device_provider.device_fn) {
                throw std::logic_error("ExecutionTargetRegistry requires an audio device provider");
            }
            if (!_request_notification) {
                _request_notification = std::make_shared<std::condition_variable>();
            }
        }

        void register_executor(size_t executor_id)
        {
            std::scoped_lock lock(_mutex);
            if (!_registered_executors.insert(executor_id).second) {
                throw std::logic_error("ExecutionTargetRegistry executor id is already registered");
            }
            _executor_next_block_indices[executor_id] = 0;
        }

        void validate_executor_block_size(size_t executor_id, size_t block_size)
        {
            std::scoped_lock lock(_mutex);
            require_registered_executor_locked(executor_id);
            if (_shared_executor_block_size.has_value() && *_shared_executor_block_size != block_size) {
                throw std::logic_error("ExecutionTargetRegistry requires all executors to use the same block size");
            }
            _shared_executor_block_size = block_size;
        }

        size_t preferred_block_size_hint() const
        {
            if (_audio_device_provider.preferred_block_size_fn) {
                return _audio_device_provider.preferred_block_size_fn(_audio_device_provider.owner);
            }
            return MAX_BLOCK_SIZE;
        }

        AudioDeviceExecutionTarget& resolve_audio_device_target(size_t executor_id, size_t device_id, size_t channel)
        {
            (void)channel;
            std::scoped_lock lock(_mutex);
            require_registered_executor_locked(executor_id);

            auto key = audio_target_key(executor_id, device_id);
            if (auto it = _audio_device_target_indices.find(key); it != _audio_device_target_indices.end()) {
                return *_audio_device_targets[it->second];
            }

            auto* device = get_audio_device_locked(device_id);
            if (!device) {
                throw std::logic_error("ExecutionTargetRegistry audio device provider returned null");
            }

            auto target = std::make_shared<AudioDeviceExecutionTarget>(device_id, device->config());
            size_t const target_index = _audio_device_targets.size();
            _audio_device_targets.push_back(target);
            _audio_device_target_active.push_back(true);
            _audio_device_target_indices.emplace(key, target_index);
            _device_audio_target_indices[device_id].push_back(target_index);
            _executor_audio_target_indices[executor_id].push_back(target_index);
            auto& executor_device_ids = _executor_audio_device_ids[executor_id];
            if (std::find(executor_device_ids.begin(), executor_device_ids.end(), device_id) == executor_device_ids.end()) {
                executor_device_ids.push_back(device_id);
            }
            ++_device_active_audio_target_counts[device_id];
            if (!_buffered_audio_device_targets.contains(device_id)) {
                _buffered_audio_device_targets.emplace(device_id, BufferedAudioDeviceTarget(*device));
            }
            return *target;
        }

        void unregister_executor(size_t executor_id)
        {
            std::scoped_lock lock(_mutex);
            if (!_registered_executors.contains(executor_id)) {
                return;
            }

            if (auto it = _executor_audio_target_indices.find(executor_id); it != _executor_audio_target_indices.end()) {
                for (size_t target_index : it->second) {
                    if (target_index >= _audio_device_targets.size()) {
                        continue;
                    }
                    _audio_device_target_active[target_index] = false;
                    auto const& target = _audio_device_targets[target_index];
                    if (!target) {
                        continue;
                    }
                    auto active_count_it = _device_active_audio_target_counts.find(target->device_id());
                    if (active_count_it != _device_active_audio_target_counts.end() && active_count_it->second > 0) {
                        --active_count_it->second;
                    }
                    _audio_device_target_indices.erase(audio_target_key(executor_id, target->device_id()));
                }
                _executor_audio_target_indices.erase(it);
            }
            _executor_audio_device_ids.erase(executor_id);

            if (auto it = _executor_file_target_indices.find(executor_id); it != _executor_file_target_indices.end()) {
                for (size_t target_index : it->second) {
                    if (target_index < _file_target_active.size()) {
                        _file_target_active[target_index] = false;
                    }
                }
                _executor_file_target_indices.erase(it);
            }

            _registered_executors.erase(executor_id);
            _executor_next_block_indices.erase(executor_id);
            if (_registered_executors.empty()) {
                _shared_executor_block_size.reset();
            }
            _request_notification->notify_all();
        }

        void clear_audio_state_for_executor(size_t executor_id)
        {
            std::scoped_lock lock(_mutex);
            auto local_audio_it = _executor_audio_target_indices.find(executor_id);
            if (local_audio_it == _executor_audio_target_indices.end()) {
                return;
            }

            std::unordered_set<size_t> device_ids;
            for (size_t target_index : local_audio_it->second) {
                if (target_index >= _audio_device_targets.size() || !_audio_device_target_active[target_index]) {
                    continue;
                }
                auto const& target = _audio_device_targets[target_index];
                if (target) {
                    device_ids.insert(target->device_id());
                }
            }

            for (size_t device_id : device_ids) {
                if (auto it = _buffered_audio_device_targets.find(device_id); it != _buffered_audio_device_targets.end()) {
                    it->second.clear();
                }
            }

            _request_notification->notify_all();
        }

        void begin_block(size_t executor_id, size_t block_index, size_t block_size)
        {
            auto audio_indices = local_audio_target_indices(executor_id);
            for (size_t target_index : audio_indices) {
                auto const& target = _audio_device_targets[target_index];
                if (target && _audio_device_target_active[target_index]) {
                    target->begin_block(block_index, block_size);
                }
            }

            auto file_indices = local_file_target_indices(executor_id);
            for (size_t target_index : file_indices) {
                auto const& target = _file_targets[target_index];
                if (target && _file_target_active[target_index]) {
                    target->begin_block(block_index, block_size);
                }
            }
        }

        WavFileExecutionTarget& resolve_file_target(size_t executor_id, std::filesystem::path const& path, size_t channel)
        {
            (void)channel;
            std::filesystem::path const normalized = std::filesystem::absolute(path).lexically_normal();
            std::scoped_lock lock(_mutex);
            require_registered_executor_locked(executor_id);

            if (auto it = _executor_file_target_indices.find(executor_id); it != _executor_file_target_indices.end()) {
                for (size_t target_index : it->second) {
                    if (target_index >= _file_targets.size() || !_file_target_active[target_index]) {
                        continue;
                    }
                    auto const& target = _file_targets[target_index];
                    if (target && target->path() == normalized) {
                        return *target;
                    }
                }
            }

            auto target = std::make_shared<WavFileExecutionTarget>(normalized, _sample_rate);
            size_t const target_index = _file_targets.size();
            _file_targets.push_back(target);
            _file_target_active.push_back(true);
            _executor_file_target_indices[executor_id].push_back(target_index);
            return *target;
        }

        void end_block(size_t executor_id, size_t block_index, size_t block_size)
        {
            auto file_indices = local_file_target_indices(executor_id);
            for (size_t target_index : file_indices) {
                auto const& target = _file_targets[target_index];
                if (target && _file_target_active[target_index]) {
                    target->end_block(block_index, block_size);
                }
            }

            std::scoped_lock lock(_mutex);

            auto local_audio_it = _executor_audio_target_indices.find(executor_id);
            if (local_audio_it == _executor_audio_target_indices.end()) {
                return;
            }

            for (size_t target_index : local_audio_it->second) {
                if (target_index >= _audio_device_targets.size()) {
                    continue;
                }
                if (!_audio_device_target_active[target_index]) {
                    continue;
                }

                auto const& target = _audio_device_targets[target_index];
                if (!target) {
                    continue;
                }

                target->end_block(block_index, block_size);

                size_t const device_id = target->device_id();
                auto* device = get_audio_device_locked(device_id);
                if (!device) {
                    continue;
                }

                size_t const expected_contributors = _device_active_audio_target_counts[device_id];

                auto buffered_device_it = _buffered_audio_device_targets.find(device_id);
                if (buffered_device_it == _buffered_audio_device_targets.end()) {
                    throw std::logic_error("ExecutionTargetRegistry missing buffered audio device target");
                }

                buffered_device_it->second.accept_contribution(
                    target_index,
                    block_index,
                    block_size,
                    expected_contributors,
                    target->mixed_block(block_size)
                );
            }

            _request_notification->notify_all();
        }

        std::optional<std::pair<size_t, size_t>> wait_for_work(size_t executor_id, size_t block_size)
        {
            std::unique_lock lock(_mutex);
            auto local_device_it = _executor_audio_device_ids.find(executor_id);
            if (local_device_it == _executor_audio_device_ids.end() || local_device_it->second.empty()) {
                return std::nullopt;
            }

            while (!_is_shutdown_requested_locked(executor_id)) {
                bool any_hungry_device = false;

                for (size_t device_id : local_device_it->second) {
                    auto active_count_it = _device_active_audio_target_counts.find(device_id);
                    if (active_count_it == _device_active_audio_target_counts.end() || active_count_it->second == 0) {
                        continue;
                    }

                    auto buffered_device_it = _buffered_audio_device_targets.find(device_id);
                    if (buffered_device_it == _buffered_audio_device_targets.end()) {
                        continue;
                    }

                    buffered_device_it->second.try_submit_requested_block();
                    if (buffered_device_it->second.needs_graph_frames(block_size)) {
                        any_hungry_device = true;
                    }
                }

                if (any_hungry_device) {
                    size_t const next_block_index = _executor_next_block_indices[executor_id];
                    _executor_next_block_indices[executor_id] = next_block_index + block_size;
                    return std::pair{ next_block_index, block_size };
                }

                _request_notification->wait(lock, [&] {
                    if (_is_shutdown_requested_locked(executor_id)) {
                        return true;
                    }

                    auto it = _executor_audio_device_ids.find(executor_id);
                    if (it == _executor_audio_device_ids.end() || it->second.empty()) {
                        return true;
                    }

                    for (size_t device_id : it->second) {
                        auto active_count_it = _device_active_audio_target_counts.find(device_id);
                        if (active_count_it == _device_active_audio_target_counts.end() || active_count_it->second == 0) {
                            continue;
                        }
                        auto buffered_device_it = _buffered_audio_device_targets.find(device_id);
                        if (buffered_device_it != _buffered_audio_device_targets.end() &&
                            buffered_device_it->second.needs_graph_frames(block_size)) {
                            return true;
                        }
                    }

                    return false;
                });
            }

            return std::nullopt;
        }

        template<typename F>
        void for_each_target(size_t executor_id, F&& f) const
        {
            for (size_t target_index : local_audio_target_indices(executor_id)) {
                auto const& target = _audio_device_targets[target_index];
                if (target && _audio_device_target_active[target_index]) {
                    f(*target);
                }
            }
            for (size_t target_index : local_file_target_indices(executor_id)) {
                auto const& target = _file_targets[target_index];
                if (target && _file_target_active[target_index]) {
                    f(*target);
                }
            }
        }

        template<typename F>
        void for_each_target_reverse(size_t executor_id, F&& f) const
        {
            auto file_indices = local_file_target_indices(executor_id);
            for (auto it = file_indices.rbegin(); it != file_indices.rend(); ++it) {
                auto const& target = _file_targets[*it];
                if (target && _file_target_active[*it]) {
                    f(*target);
                }
            }
            auto audio_indices = local_audio_target_indices(executor_id);
            for (auto it = audio_indices.rbegin(); it != audio_indices.rend(); ++it) {
                auto const& target = _audio_device_targets[*it];
                if (target && _audio_device_target_active[*it]) {
                    f(*target);
                }
            }
        }

        bool is_shutdown_requested(size_t executor_id) const
        {
            std::scoped_lock lock(_mutex);
            return _is_shutdown_requested_locked(executor_id);
        }

    private:
        bool _is_shutdown_requested_locked(size_t executor_id) const
        {
            auto local_audio_it = _executor_audio_target_indices.find(executor_id);
            if (local_audio_it == _executor_audio_target_indices.end()) {
                return false;
            }

            for (size_t target_index : local_audio_it->second) {
                if (target_index >= _audio_device_targets.size() || !_audio_device_target_active[target_index]) {
                    continue;
                }
                auto const& target = _audio_device_targets[target_index];
                if (!target) {
                    continue;
                }
                auto* device = const_cast<ExecutionTargetRegistry*>(this)->get_audio_device_locked(target->device_id());
                if (device && device->is_shutdown_requested()) {
                    return true;
                }
            }
            return false;
        }

    public:
        void request_shutdown(size_t executor_id)
        {
            std::scoped_lock lock(_mutex);
            auto local_audio_it = _executor_audio_target_indices.find(executor_id);
            if (local_audio_it == _executor_audio_target_indices.end()) {
                return;
            }

            for (size_t target_index : local_audio_it->second) {
                if (target_index >= _audio_device_targets.size() || !_audio_device_target_active[target_index]) {
                    continue;
                }
                auto const& target = _audio_device_targets[target_index];
                if (!target) {
                    continue;
                }
                auto* device = get_audio_device_locked(target->device_id());
                if (device) {
                    device->request_shutdown();
                }
            }
            _request_notification->notify_all();
        }

    private:
        static std::uint64_t audio_target_key(size_t executor_id, size_t device_id)
        {
            return (static_cast<std::uint64_t>(executor_id) << 32) | static_cast<std::uint64_t>(device_id);
        }

        LogicalAudioDevice* get_audio_device_locked(size_t device_id)
        {
            if (auto it = _devices.find(device_id); it != _devices.end()) {
                return it->second;
            }

            auto* device = _audio_device_provider.device_fn(_audio_device_provider.owner, device_id);
            if (device) {
                _devices.emplace(device_id, device);
            }
            return device;
        }

        void require_registered_executor_locked(size_t executor_id) const
        {
            if (!_registered_executors.contains(executor_id)) {
                throw std::logic_error("ExecutionTargetRegistry executor id is not registered");
            }
        }

        std::vector<size_t> const& local_audio_target_indices(size_t executor_id) const
        {
            static std::vector<size_t> const empty;
            if (auto it = _executor_audio_target_indices.find(executor_id); it != _executor_audio_target_indices.end()) {
                return it->second;
            }
            return empty;
        }

        std::vector<size_t> const& local_file_target_indices(size_t executor_id) const
        {
            static std::vector<size_t> const empty;
            if (auto it = _executor_file_target_indices.find(executor_id); it != _executor_file_target_indices.end()) {
                return it->second;
            }
            return empty;
        }
    };

    inline AudioDeviceExecutionTarget& ExecutionTargetRegistrar::audio_device(size_t device_id, size_t channel) const
    {
        if (!_registry) {
            throw std::logic_error("ExecutionTargetRegistrar has no registry");
        }
        return _registry->resolve_audio_device_target(_executor_id, device_id, channel);
    }

    inline WavFileExecutionTarget& ExecutionTargetRegistrar::file(std::filesystem::path const& path, size_t channel) const
    {
        if (!_registry) {
            throw std::logic_error("ExecutionTargetRegistrar has no registry");
        }
        return _registry->resolve_file_target(_executor_id, path, channel);
    }
}
