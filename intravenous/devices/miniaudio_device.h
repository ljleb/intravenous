#pragma once

#include "audio_device.h"
#include "third_party/miniaudio/miniaudio.h"

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <vector>

namespace iv {
    struct MiniaudioDeviceState {
        RenderConfig config;
        mutable std::mutex mutex;
        std::condition_variable cv;
        std::optional<std::pair<size_t, size_t>> active_requested_block;
        std::vector<Sample> mixed_block;
        bool block_ready = false;
        bool shutdown_requested = false;
        size_t next_frame_index = 0;
        RequestNotification request_notification;
        ma_device device {};

        static void data_callback(ma_device* device, void* output, void const*, ma_uint32 frame_count)
        {
            auto& self = *static_cast<MiniaudioDeviceState*>(device->pUserData);
            self.render_callback(static_cast<float*>(output), static_cast<size_t>(frame_count));
        }

        void render_callback(float* output, size_t frame_count)
        {
            begin_requested_block(next_frame_index, frame_count);
            if (!wait_until_block_ready()) {
                std::fill_n(output, frame_count * config.num_channels, 0.0f);
                return;
            }

            auto mixed = current_mixed_block();
            if (mixed.size() != frame_count * config.num_channels) {
                throw std::logic_error("LogicalAudioDevice mixed block size mismatch");
            }

            std::memcpy(output, mixed.data(), mixed.size_bytes());
            finish_requested_block();
            next_frame_index += frame_count;
        }

        MiniaudioDeviceState(RenderConfig config_, RequestNotification request_notification_)
        : config(std::move(config_))
        , request_notification(std::move(request_notification_))
        {
            validate_render_config(config);

            ma_device_config device_config = ma_device_config_init(ma_device_type_playback);
            device_config.playback.format = ma_format_f32;
            device_config.playback.channels = static_cast<ma_uint32>(config.num_channels);
            device_config.sampleRate = static_cast<ma_uint32>(config.sample_rate);
            device_config.periodSizeInFrames = static_cast<ma_uint32>(config.preferred_block_size);
            device_config.dataCallback = &MiniaudioDeviceState::data_callback;
            device_config.pUserData = this;

            if (ma_device_init(nullptr, &device_config, &device) != MA_SUCCESS) {
                throw std::runtime_error("failed to initialize miniaudio playback device");
            }

            config.sample_rate = device.sampleRate;
            config.num_channels = device.playback.channels;
            if (device.playback.internalPeriodSizeInFrames > 0) {
                config.preferred_block_size = device.playback.internalPeriodSizeInFrames;
            }
            mixed_block.assign(config.max_block_frames * config.num_channels, 0.0f);

            if (ma_device_start(&device) != MA_SUCCESS) {
                ma_device_uninit(&device);
                throw std::runtime_error("failed to start miniaudio playback device");
            }
        }

        ~MiniaudioDeviceState()
        {
            request_shutdown();
            ma_device_uninit(&device);
        }

        bool is_shutdown_requested() const
        {
            std::scoped_lock lock(mutex);
            return shutdown_requested;
        }

        void request_shutdown()
        {
            {
                std::scoped_lock lock(mutex);
                shutdown_requested = true;
            }
            if (request_notification) {
                request_notification->notify_all();
            }
            cv.notify_all();
        }

        std::optional<std::pair<size_t, size_t>> requested_block() const
        {
            std::scoped_lock lock(mutex);
            return active_requested_block;
        }

        std::optional<std::pair<size_t, size_t>> wait_for_requested_block()
        {
            std::unique_lock lock(mutex);
            cv.wait(lock, [&] {
                return shutdown_requested || active_requested_block.has_value();
            });
            if (shutdown_requested) {
                return std::nullopt;
            }
            return active_requested_block;
        }

        void begin_requested_block(size_t frame_index, size_t frame_count)
        {
            std::scoped_lock lock(mutex);
            active_requested_block = std::pair{ frame_index, frame_count };
            block_ready = false;
            mixed_block.assign(frame_count * config.num_channels, 0.0f);
            if (request_notification) {
                request_notification->notify_all();
            }
            cv.notify_all();
        }

        void submit_mixed_block(
            size_t frame_index,
            std::span<Sample const> interleaved,
            size_t frames,
            size_t channels
        )
        {
            std::scoped_lock lock(mutex);
            if (!active_requested_block.has_value()) {
                throw std::logic_error("LogicalAudioDevice cannot accept a mixed block without an active request");
            }
            if (*active_requested_block != std::pair{ frame_index, frames }) {
                throw std::logic_error("LogicalAudioDevice mixed block does not match the active request");
            }
            if (channels != config.num_channels) {
                throw std::logic_error("LogicalAudioDevice mixed block channel count mismatch");
            }
            if (interleaved.size() != frames * channels) {
                throw std::logic_error("LogicalAudioDevice mixed block span size mismatch");
            }

            mixed_block.assign(interleaved.begin(), interleaved.end());
            block_ready = true;
            cv.notify_all();
        }

        bool wait_until_block_ready()
        {
            std::unique_lock lock(mutex);
            cv.wait(lock, [&] {
                return shutdown_requested || block_ready;
            });
            return !shutdown_requested && block_ready;
        }

        std::span<Sample const> current_mixed_block() const
        {
            std::scoped_lock lock(mutex);
            return { mixed_block.data(), mixed_block.size() };
        }

        void finish_requested_block()
        {
            std::scoped_lock lock(mutex);
            active_requested_block.reset();
            block_ready = false;
            cv.notify_all();
        }
    };

    inline LogicalAudioDevice make_miniaudio_device(RenderConfig config = {}, RequestNotification request_notification = {})
    {
        auto state = std::make_shared<MiniaudioDeviceState>(std::move(config), std::move(request_notification));
        auto const device_config = state->config;
        return LogicalAudioDevice::from_state(device_config, std::move(state));
    }
}
