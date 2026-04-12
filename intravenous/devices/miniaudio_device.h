#pragma once

#include "audio_device.h"
#include "third_party/miniaudio/miniaudio.h"

#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace iv {
    class MiniaudioLogicalDevice {
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
            auto& self = *static_cast<MiniaudioLogicalDevice*>(device->pUserData);
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
        explicit MiniaudioLogicalDevice(RenderConfig config = {})
        : _config(std::move(config))
        {
            validate_render_config(_config);

            ma_device_config device_config = ma_device_config_init(ma_device_type_playback);
            device_config.playback.format = ma_format_f32;
            device_config.playback.channels = static_cast<ma_uint32>(_config.num_channels);
            device_config.sampleRate = static_cast<ma_uint32>(_config.sample_rate);
            device_config.periodSizeInFrames = static_cast<ma_uint32>(_config.preferred_block_size);
            device_config.dataCallback = &MiniaudioLogicalDevice::data_callback;
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

        ~MiniaudioLogicalDevice()
        {
            begin_shutdown();
            ma_device_uninit(&_device);
        }

        MiniaudioLogicalDevice(MiniaudioLogicalDevice&&) = delete;
        MiniaudioLogicalDevice& operator=(MiniaudioLogicalDevice&&) = delete;
        MiniaudioLogicalDevice(MiniaudioLogicalDevice const&) = delete;
        MiniaudioLogicalDevice& operator=(MiniaudioLogicalDevice const&) = delete;

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
    };

    inline LogicalAudioDevice make_miniaudio_device(RenderConfig config = {})
    {
        return LogicalAudioDevice(std::in_place_type<MiniaudioLogicalDevice>, std::move(config));
    }
}
