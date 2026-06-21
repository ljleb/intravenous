#pragma once

#include <intravenous/devices/audio_device.h>
#include <intravenous/third_party/miniaudio/miniaudio.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace iv {
    class MiniaudioAudioOutputDevice {
        RenderConfig _config;
        ma_device _device {};
        std::mutex _mutex;
        std::condition_variable _cv;
        bool _shutdown_requested = false;
        bool _request_pending = false;
        bool _response_ready = false;
        Sample* _pending_output = nullptr;
        size_t _pending_samples = 0;

        static void data_callback(ma_device* device, void* output, void const*, ma_uint32 frame_count)
        {
            auto& self = *static_cast<MiniaudioAudioOutputDevice*>(device->pUserData);
            self.render_callback(static_cast<Sample*>(output), static_cast<size_t>(frame_count));
        }

        void render_callback(Sample* output, size_t frame_count)
        {
            {
                std::unique_lock lock(_mutex);
                _pending_output = output;
                _pending_samples = frame_count * _config.num_channels;
                _request_pending = true;
                _response_ready = false;
                _cv.notify_all();

                _cv.wait(lock, [&] {
                    return _shutdown_requested || _response_ready;
                });

                _pending_output = nullptr;
                _pending_samples = 0;
                _request_pending = false;
                _response_ready = false;
            }
        }

        void begin_shutdown() noexcept
        {
            {
                std::scoped_lock lock(_mutex);
                _shutdown_requested = true;
            }
            _cv.notify_all();
        }

    public:
        explicit MiniaudioAudioOutputDevice(RenderConfig config = {})
        : _config(std::move(config))
        {
            validate_render_config(_config);

            ma_device_config device_config = ma_device_config_init(ma_device_type_playback);
            device_config.playback.format = ma_format_f32;
            device_config.playback.channels = static_cast<ma_uint32>(_config.num_channels);
            device_config.sampleRate = static_cast<ma_uint32>(_config.sample_rate);
            device_config.periodSizeInFrames = static_cast<ma_uint32>(_config.preferred_block_size);
            device_config.dataCallback = &MiniaudioAudioOutputDevice::data_callback;
            device_config.pUserData = this;
            device_config.noPreSilencedOutputBuffer = true;
            // device_config.noClip = true;

            if (ma_device_init(nullptr, &device_config, &_device) != MA_SUCCESS) {
                throw std::runtime_error("failed to initialize miniaudio playback device");
            }

            _config.sample_rate = _device.sampleRate;
            _config.num_channels = _device.playback.channels;
            if (_device.playback.internalPeriodSizeInFrames > 0) {
                _config.preferred_block_size = _device.playback.internalPeriodSizeInFrames;
            }

            if (ma_device_start(&_device) != MA_SUCCESS) {
                ma_device_uninit(&_device);
                throw std::runtime_error("failed to start miniaudio playback device");
            }
        }

        ~MiniaudioAudioOutputDevice()
        {
            begin_shutdown();
            ma_device_uninit(&_device);
        }

        MiniaudioAudioOutputDevice(MiniaudioAudioOutputDevice&&) = delete;
        MiniaudioAudioOutputDevice& operator=(MiniaudioAudioOutputDevice&&) = delete;
        MiniaudioAudioOutputDevice(MiniaudioAudioOutputDevice const&) = delete;
        MiniaudioAudioOutputDevice& operator=(MiniaudioAudioOutputDevice const&) = delete;

        RenderConfig const& config() const
        {
            return _config;
        }

        std::span<Sample> wait_for_block_request()
        {
            std::unique_lock lock(_mutex);
            _cv.wait(lock, [&] {
                return _shutdown_requested || (_request_pending && !_response_ready);
            });

            if (!_request_pending) {
                throw std::logic_error("MiniaudioLogicalDevice wait_for_block_request() interrupted without a pending request");
            }

            return std::span<Sample>(_pending_output, _pending_samples);
        }

        void submit_response()
        {
            {
                std::scoped_lock lock(_mutex);
                if (!_request_pending || _response_ready || _pending_output == nullptr) {
                    throw std::logic_error("MiniaudioLogicalDevice has no claimed request awaiting response");
                }
                _response_ready = true;
            }
            _cv.notify_all();
        }

        void request_shutdown()
        {
            begin_shutdown();
        }
    };

    inline AudioOutputDevice make_miniaudio_output_device(RenderConfig config = {})
    {
        return AudioOutputDevice(std::in_place_type<MiniaudioAudioOutputDevice>, std::move(config));
    }

    class MiniaudioAudioInputDevice {
        struct CapturedBlockSlot {
            std::vector<Sample> storage {};
            size_t sample_count = 0;
            double capture_timestamp_seconds = 0.0;
            bool discontinuous = false;
            bool ready = false;
            bool acquired = false;
        };

        static constexpr size_t queue_capacity = 4;

        RenderConfig _config;
        ma_device _device {};
        std::mutex _mutex;
        std::condition_variable _cv;
        bool _shutdown_requested = false;
        bool _overflowed_since_last_capture = false;
        size_t _read_index = 0;
        size_t _write_index = 0;
        std::optional<size_t> _acquired_index;
        std::array<CapturedBlockSlot, queue_capacity> _slots;

        static void data_callback(
            ma_device* device,
            void*,
            void const* input,
            ma_uint32 frame_count)
        {
            auto& self = *static_cast<MiniaudioAudioInputDevice*>(device->pUserData);
            self.capture_callback(static_cast<Sample const*>(input), static_cast<size_t>(frame_count));
        }

        void capture_callback(Sample const* input, size_t frame_count)
        {
            auto const sample_count = frame_count * _config.num_channels;
            auto const capture_timestamp_seconds =
                std::chrono::duration<double>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();

            std::scoped_lock lock(_mutex);
            auto& slot = _slots[_write_index];
            if (slot.ready || slot.acquired) {
                _overflowed_since_last_capture = true;
                return;
            }

            if (slot.storage.size() != _config.max_block_frames * _config.num_channels) {
                slot.storage.assign(
                    _config.max_block_frames * _config.num_channels,
                    Sample{});
            }

            if (sample_count > slot.storage.size()) {
                _overflowed_since_last_capture = true;
                return;
            }

            if (input != nullptr) {
                std::copy_n(input, sample_count, slot.storage.begin());
            } else {
                std::fill_n(slot.storage.begin(), sample_count, Sample{});
            }

            slot.sample_count = sample_count;
            slot.capture_timestamp_seconds = capture_timestamp_seconds;
            slot.discontinuous = _overflowed_since_last_capture;
            slot.ready = true;
            _overflowed_since_last_capture = false;
            _write_index = (_write_index + 1) % queue_capacity;
            _cv.notify_all();
        }

        void begin_shutdown() noexcept
        {
            {
                std::scoped_lock lock(_mutex);
                _shutdown_requested = true;
            }
            _cv.notify_all();
        }

    public:
        explicit MiniaudioAudioInputDevice(RenderConfig config = {})
        : _config(std::move(config))
        {
            validate_render_config(_config);

            ma_device_config device_config = ma_device_config_init(ma_device_type_capture);
            device_config.capture.format = ma_format_f32;
            device_config.capture.channels = static_cast<ma_uint32>(_config.num_channels);
            device_config.sampleRate = static_cast<ma_uint32>(_config.sample_rate);
            device_config.periodSizeInFrames =
                static_cast<ma_uint32>(_config.preferred_block_size);
            device_config.dataCallback = &MiniaudioAudioInputDevice::data_callback;
            device_config.pUserData = this;

            if (ma_device_init(nullptr, &device_config, &_device) != MA_SUCCESS) {
                throw std::runtime_error("failed to initialize miniaudio capture device");
            }

            _config.sample_rate = _device.sampleRate;
            _config.num_channels = _device.capture.channels;
            if (_device.capture.internalPeriodSizeInFrames > 0) {
                _config.preferred_block_size = _device.capture.internalPeriodSizeInFrames;
            }
            _config.max_block_frames =
                std::max(_config.max_block_frames, _config.preferred_block_size);

            for (auto& slot : _slots) {
                slot.storage.assign(
                    _config.max_block_frames * _config.num_channels,
                    Sample{});
            }

            if (ma_device_start(&_device) != MA_SUCCESS) {
                ma_device_uninit(&_device);
                throw std::runtime_error("failed to start miniaudio capture device");
            }
        }

        ~MiniaudioAudioInputDevice()
        {
            begin_shutdown();
            ma_device_uninit(&_device);
        }

        MiniaudioAudioInputDevice(MiniaudioAudioInputDevice&&) = delete;
        MiniaudioAudioInputDevice& operator=(MiniaudioAudioInputDevice&&) = delete;
        MiniaudioAudioInputDevice(MiniaudioAudioInputDevice const&) = delete;
        MiniaudioAudioInputDevice& operator=(MiniaudioAudioInputDevice const&) = delete;

        RenderConfig const& config() const
        {
            return _config;
        }

        AudioInputBlock wait_for_captured_block()
        {
            std::unique_lock lock(_mutex);
            _cv.wait(lock, [&] {
                return _shutdown_requested || _slots[_read_index].ready;
            });

            if (!_slots[_read_index].ready) {
                throw std::logic_error(
                    "MiniaudioAudioInputDevice wait_for_captured_block() interrupted without a pending capture");
            }

            auto& slot = _slots[_read_index];
            slot.ready = false;
            slot.acquired = true;
            _acquired_index = _read_index;
            _read_index = (_read_index + 1) % queue_capacity;
            return AudioInputBlock{
                .samples = std::span<Sample const>(slot.storage.data(), slot.sample_count),
                .capture_timestamp_seconds = slot.capture_timestamp_seconds,
                .discontinuous = slot.discontinuous,
            };
        }

        void release_captured_block()
        {
            std::scoped_lock lock(_mutex);
            if (!_acquired_index.has_value()) {
                throw std::logic_error(
                    "MiniaudioAudioInputDevice has no claimed capture awaiting release");
            }

            auto& slot = _slots[*_acquired_index];
            if (!slot.acquired) {
                throw std::logic_error(
                    "MiniaudioAudioInputDevice release_captured_block() called for a free slot");
            }

            slot.acquired = false;
            slot.sample_count = 0;
            slot.capture_timestamp_seconds = 0.0;
            slot.discontinuous = false;
            _acquired_index.reset();
        }

        void request_shutdown()
        {
            begin_shutdown();
        }
    };

    inline AudioInputDevice make_miniaudio_input_device(RenderConfig config = {})
    {
        return AudioInputDevice(
            std::in_place_type<MiniaudioAudioInputDevice>,
            std::move(config));
    }
}
