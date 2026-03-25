#pragma once

#include "dsl.h"
#include "devices/channel_buffer_sink.h"
#include "third_party/miniaudio/miniaudio.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace iv {
    struct RenderConfig {
        size_t sample_rate = 48000;
        size_t num_channels = 2;
        size_t max_block_frames = 4096;
        size_t preferred_block_size = 256;
    };

    class AudioDevice {
        ma_context _context {};
        ma_device _device {};

        bool _open_backend = true;
        bool _context_initialized = false;
        bool _device_initialized = false;
        bool _device_started = false;
        bool _shutdown_requested = false;
        bool _render_requested = false;
        bool _render_in_progress = false;
        bool _render_finished = false;
        bool _block_open = false;

        RenderConfig _config;
        Sample _sample_period;
        std::vector<std::vector<Sample>> _planar_storage;
        std::vector<Sample*> _channel_ptrs;

        size_t _requested_frames = 0;
        size_t _requested_channels = 0;
        size_t _active_block_start = 0;
        size_t _active_block_end = 0;
        std::mutex _mutex;
        std::condition_variable _cv;

        void initialize_backend()
        {
            if (_shutdown_requested) {
                return;
            }
            if (_device_initialized) {
                return;
            }

            ma_result result = ma_context_init(nullptr, 0, nullptr, &_context);
            if (result != MA_SUCCESS) {
                throw std::runtime_error("ma_context_init failed");
            }
            _context_initialized = true;

            ma_device_config device_config = ma_device_config_init(ma_device_type_playback);
            device_config.playback.format = ma_format_f32;
            device_config.playback.channels = static_cast<ma_uint32>(_config.num_channels);
            device_config.sampleRate = static_cast<ma_uint32>(_config.sample_rate);
            device_config.dataCallback = data_callback;
            device_config.pUserData = this;

            result = ma_device_init(&_context, &device_config, &_device);
            if (result != MA_SUCCESS) {
                shutdown();
                throw std::runtime_error("ma_device_init failed");
            }
            _device_initialized = true;

            _config.sample_rate = _device.sampleRate;
            _config.num_channels = _device.playback.channels;
            if (_device.playback.internalPeriodSizeInFrames != 0) {
                _config.max_block_frames = _device.playback.internalPeriodSizeInFrames;
            }
            _sample_period = static_cast<Sample>(1) / static_cast<Sample>(_config.sample_rate);

            _planar_storage.assign(_config.num_channels, std::vector<Sample>(_config.max_block_frames, 0.0f));
            _channel_ptrs.assign(_config.num_channels, nullptr);
            for (size_t channel = 0; channel < _config.num_channels; ++channel) {
                _channel_ptrs[channel] = _planar_storage[channel].data();
            }
        }

        void start_backend()
        {
            if (_shutdown_requested) {
                return;
            }
            if (_device_started) {
                return;
            }

            ma_result result = ma_device_start(&_device);
            if (result != MA_SUCCESS) {
                shutdown();
                throw std::runtime_error("ma_device_start failed");
            }
            _device_started = true;
        }

        void clear_staging(size_t channels, size_t frames)
        {
            for (size_t channel = 0; channel < channels; ++channel) {
                std::fill_n(_planar_storage[channel].data(), frames, 0.0f);
            }
        }

        void interleave_to(float* output) const
        {
            for (size_t frame = 0; frame < _requested_frames; ++frame) {
                for (size_t channel = 0; channel < _requested_channels; ++channel) {
                    output[frame * _requested_channels + channel] = _planar_storage[channel][frame];
                }
            }
        }

        void write_silence(float* output, size_t frames, size_t channels) const
        {
            std::memset(output, 0, frames * channels * sizeof(float));
        }

        static void data_callback(ma_device* device, void* output, void const*, ma_uint32 frame_count)
        {
            auto* self = static_cast<AudioDevice*>(device->pUserData);
            auto* out = static_cast<float*>(output);
            if (!self) {
                return;
            }

            std::unique_lock lock(self->_mutex);

            if (
                self->_shutdown_requested ||
                device->playback.channels > self->_config.num_channels ||
                frame_count > self->_config.max_block_frames
            ) {
                self->write_silence(out, frame_count, device->playback.channels);
                return;
            }

            self->_requested_frames = frame_count;
            self->_requested_channels = device->playback.channels;
            self->_render_requested = true;
            self->_render_in_progress = false;
            self->_render_finished = false;
            self->_cv.notify_all();

            self->_cv.wait(lock, [&] {
                return self->_render_finished || self->_shutdown_requested;
            });

            if (self->_shutdown_requested) {
                self->write_silence(out, frame_count, device->playback.channels);
                return;
            }

            self->interleave_to(out);
            self->_render_finished = false;
            self->_cv.notify_all();
        }

    public:
        explicit AudioDevice(RenderConfig config = {}, bool open_backend = true) :
            _open_backend(open_backend),
            _config(config),
            _sample_period(static_cast<Sample>(1) / static_cast<Sample>(config.sample_rate)),
            _planar_storage(config.num_channels, std::vector<Sample>(config.max_block_frames, 0.0f)),
            _channel_ptrs(config.num_channels, nullptr)
        {
            if (_config.sample_rate == 0 || _config.num_channels == 0 || _config.max_block_frames == 0) {
                throw std::invalid_argument("render config values must be non-zero");
            }
            try {
                validate_block_size(_config.preferred_block_size, "preferred_block_size must be a power of 2");
            } catch (std::logic_error const& e) {
                throw std::invalid_argument(e.what());
            }

            for (size_t channel = 0; channel < _config.num_channels; ++channel) {
                _channel_ptrs[channel] = _planar_storage[channel].data();
            }

            if (_open_backend) {
                initialize_backend();
            }
        }

        ~AudioDevice()
        {
            shutdown();
        }

        AudioDevice(AudioDevice const&) = delete;
        AudioDevice& operator=(AudioDevice const&) = delete;

        AudioDevice(AudioDevice&& other) noexcept :
            _context(std::exchange(other._context, {})),
            _device(std::exchange(other._device, {})),
            _open_backend(std::exchange(other._open_backend, false)),
            _context_initialized(std::exchange(other._context_initialized, false)),
            _device_initialized(std::exchange(other._device_initialized, false)),
            _device_started(std::exchange(other._device_started, false)),
            _shutdown_requested(std::exchange(other._shutdown_requested, false)),
            _render_requested(std::exchange(other._render_requested, false)),
            _render_in_progress(std::exchange(other._render_in_progress, false)),
            _render_finished(std::exchange(other._render_finished, false)),
            _block_open(std::exchange(other._block_open, false)),
            _config(other._config),
            _sample_period(other._sample_period),
            _planar_storage(std::move(other._planar_storage)),
            _channel_ptrs(std::move(other._channel_ptrs)),
            _requested_frames(std::exchange(other._requested_frames, 0)),
            _requested_channels(std::exchange(other._requested_channels, 0)),
            _active_block_start(std::exchange(other._active_block_start, 0)),
            _active_block_end(std::exchange(other._active_block_end, 0))
        {}

        AudioDevice& operator=(AudioDevice&& other) noexcept
        {
            if (this == &other) {
                return *this;
            }

            shutdown();

            _context = std::exchange(other._context, {});
            _device = std::exchange(other._device, {});
            _open_backend = std::exchange(other._open_backend, false);
            _context_initialized = std::exchange(other._context_initialized, false);
            _device_initialized = std::exchange(other._device_initialized, false);
            _device_started = std::exchange(other._device_started, false);
            _shutdown_requested = std::exchange(other._shutdown_requested, false);
            _render_requested = std::exchange(other._render_requested, false);
            _render_in_progress = std::exchange(other._render_in_progress, false);
            _render_finished = std::exchange(other._render_finished, false);
            _block_open = std::exchange(other._block_open, false);
            _config = other._config;
            _sample_period = other._sample_period;
            _planar_storage = std::move(other._planar_storage);
            _channel_ptrs = std::move(other._channel_ptrs);
            _requested_frames = std::exchange(other._requested_frames, 0);
            _requested_channels = std::exchange(other._requested_channels, 0);
            _active_block_start = std::exchange(other._active_block_start, 0);
            _active_block_end = std::exchange(other._active_block_end, 0);
            return *this;
        }

        RenderConfig const& config() const
        {
            return _config;
        }

        void ensure_backend_initialized()
        {
            if (_shutdown_requested || !_open_backend || _device_initialized) {
                return;
            }
            initialize_backend();
        }

        Sample& sample_period()
        {
            return _sample_period;
        }

        bool is_shutdown_requested() const
        {
            return _shutdown_requested;
        }

        std::string sink_id(size_t channel, size_t device_id = 0) const
        {
            return "output-device:" + std::to_string(device_id) + ":channel:" + std::to_string(channel);
        }

        std::optional<size_t> sink_channel(std::string_view id) const
        {
            constexpr std::string_view prefix = "output-device:";
            constexpr std::string_view channel_marker = ":channel:";

            if (!id.starts_with(prefix)) {
                return std::nullopt;
            }

            size_t const marker = id.find(channel_marker, prefix.size());
            if (marker == std::string_view::npos) {
                return std::nullopt;
            }

            std::string_view channel_text = id.substr(marker + channel_marker.size());
            if (channel_text.empty()) {
                return std::nullopt;
            }

            size_t channel = 0;
            for (char c : channel_text) {
                if (c < '0' || c > '9') {
                    return std::nullopt;
                }
                channel = channel * 10 + size_t(c - '0');
            }
            return channel;
        }

        size_t sink_buffer_size() const
        {
            return size_t(1) << size_t(std::ceil(std::log2(_config.max_block_frames)));
        }

        size_t processing_block_size() const
        {
            return _config.max_block_frames;
        }

        size_t scheduling_block_size() const
        {
            return prev_power_of_2(processing_block_size());
        }

        std::span<Sample const> output_block(size_t channel) const
        {
            return _planar_storage.at(channel);
        }

        void prepare_tick(size_t global_index)
        {
            if (_block_open) {
                return;
            }
            if (_shutdown_requested) {
                return;
            }

            if (_open_backend && !_device_started) {
                if (!_device_initialized) {
                    initialize_backend();
                }
                start_backend();
            }

            if (!_open_backend) {
                clear_staging(_config.num_channels, _config.max_block_frames);
                _block_open = true;
                _active_block_start = global_index;
                _active_block_end = global_index + _config.max_block_frames;
                return;
            }

            std::unique_lock lock(_mutex);
            _cv.wait(lock, [&] {
                return _render_requested || _shutdown_requested;
            });

            if (_shutdown_requested) {
                return;
            }

            _render_requested = false;
            _render_in_progress = true;
            clear_staging(_requested_channels, _requested_frames);
            _block_open = true;
            _active_block_start = global_index;
            _active_block_end = global_index + _requested_frames;
        }

        size_t active_block_start() const
        {
            return _active_block_start;
        }

        size_t active_block_end() const
        {
            return _active_block_end;
        }

        size_t active_output_channels() const
        {
            return _requested_channels == 0 ? _config.num_channels : _requested_channels;
        }

        size_t active_block_frames() const
        {
            return _active_block_end - _active_block_start;
        }

        void mix_sink_block(size_t channel, std::span<Sample const> sink_buffer, size_t block_start, size_t frames)
        {
            if (channel >= active_output_channels() || sink_buffer.empty()) {
                return;
            }

            for (size_t frame = 0; frame < frames; ++frame) {
                size_t const global_index = block_start + frame;
                _planar_storage[channel][frame] += sink_buffer[global_index & (sink_buffer.size() - 1)];
            }
        }

        void finish_tick(size_t global_index)
        {
            if (!_block_open || global_index + 1 < _active_block_end) {
                return;
            }
            _block_open = false;

            if (!_open_backend) {
                return;
            }

            {
                std::lock_guard lock(_mutex);
                if (_shutdown_requested || !_render_in_progress) {
                    return;
                }
                _render_in_progress = false;
                _render_finished = true;
            }
            _cv.notify_all();
        }

        void request_shutdown()
        {
            shutdown();
        }

        void shutdown()
        {
            {
                std::lock_guard lock(_mutex);
                _shutdown_requested = true;
            }
            _cv.notify_all();

            if (_device_initialized) {
                ma_device_uninit(&_device);
                _device_initialized = false;
                _device_started = false;
            }

            if (_context_initialized) {
                ma_context_uninit(&_context);
                _context_initialized = false;
            }
        }
    };

    class AudioRenderSession {
        AudioDevice* _device = nullptr;

    public:
        AudioRenderSession() = default;

        explicit AudioRenderSession(AudioDevice& device) :
            _device(&device)
        {}

        RenderConfig const& config() const
        {
            return _device->config();
        }

        std::string sink_id(size_t channel, size_t device_id = 0) const
        {
            return _device->sink_id(channel, device_id);
        }

        std::optional<size_t> sink_channel(std::string_view id) const
        {
            return _device->sink_channel(id);
        }

        size_t sink_buffer_size() const
        {
            return _device->sink_buffer_size();
        }

        size_t scheduling_block_size() const
        {
            return _device->scheduling_block_size();
        }

        bool is_shutdown_requested() const
        {
            return _device->is_shutdown_requested();
        }

        void prepare_tick(size_t global_index) const
        {
            _device->prepare_tick(global_index);
        }

        size_t active_block_start() const
        {
            return _device->active_block_start();
        }

        size_t active_block_end() const
        {
            return _device->active_block_end();
        }

        size_t active_block_frames() const
        {
            return _device->active_block_frames();
        }

        void mix_sink_buffer(size_t channel, std::span<Sample const> sink_buffer) const
        {
            _device->mix_sink_block(
                channel,
                sink_buffer,
                active_block_start(),
                active_block_frames()
            );
        }

        void finish_tick(size_t global_index) const
        {
            _device->finish_tick(global_index);
        }
    };

}
