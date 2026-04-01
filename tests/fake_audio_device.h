#pragma once

#include "devices/audio_device.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace iv::test {
    struct FakeAudioDeviceState {
        iv::RenderConfig config;
        iv::Sample sample_period = 0.0f;
        mutable std::mutex mutex;
        std::condition_variable cv;
        std::optional<std::pair<size_t, size_t>> active_requested_block;
        std::vector<iv::Sample> mixed_block;
        std::vector<iv::Sample> output_storage;
        bool block_ready = false;
        bool shutdown_requested = false;
        iv::RequestNotification request_notification;

        FakeAudioDeviceState(iv::RenderConfig config_, iv::RequestNotification request_notification_)
        : config(std::move(config_))
        , request_notification(std::move(request_notification_))
        , output_storage(config.num_channels * config.max_block_frames, 0.0f)
        {
            iv::validate_render_config(config);
            sample_period = iv::sample_period(config);
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
            std::span<iv::Sample const> interleaved,
            size_t frames,
            size_t channels
        )
        {
            std::scoped_lock lock(mutex);
            if (channels != config.num_channels) {
                throw std::logic_error("FakeAudioDevice mixed block channel count mismatch");
            }
            if (interleaved.size() != frames * channels) {
                throw std::logic_error("FakeAudioDevice mixed block span size mismatch");
            }
            if (active_requested_block.has_value() && *active_requested_block != std::pair{ frame_index, frames }) {
                throw std::logic_error("FakeAudioDevice mixed block does not match the active request");
            }

            mixed_block.assign(interleaved.begin(), interleaved.end());
            std::fill(output_storage.begin(), output_storage.end(), 0.0f);
            for (size_t frame = 0; frame < frames && frame < config.max_block_frames; ++frame) {
                for (size_t channel = 0; channel < channels; ++channel) {
                    output_storage[channel * config.max_block_frames + frame] =
                        interleaved[frame * channels + channel];
                }
            }
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

        template<typename Rep, typename Period>
        bool wait_until_block_ready_for(std::chrono::duration<Rep, Period> timeout)
        {
            std::unique_lock lock(mutex);
            bool const ready = cv.wait_for(lock, timeout, [&] {
                return shutdown_requested || block_ready;
            });
            return ready && !shutdown_requested && block_ready;
        }

        bool is_block_ready() const
        {
            std::scoped_lock lock(mutex);
            return block_ready;
        }

        std::span<iv::Sample const> current_mixed_block() const
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

    class FakeAudioDevice {
        std::shared_ptr<FakeAudioDeviceState> _state;
        iv::LogicalAudioDevice _device;

    public:
        explicit FakeAudioDevice(iv::RenderConfig config = {}, iv::RequestNotification request_notification = {})
        : _state(std::make_shared<FakeAudioDeviceState>(std::move(config), std::move(request_notification)))
        , _device([&] {
            auto const device_config = _state->config;
            return iv::LogicalAudioDevice::from_state(device_config, _state);
        }())
        {}

        iv::LogicalAudioDevice& device()
        {
            return _device;
        }

        iv::LogicalAudioDevice const& device() const
        {
            return _device;
        }

        iv::RenderConfig const& config() const
        {
            return _device.config();
        }

        bool is_block_ready() const
        {
            return _state->is_block_ready();
        }

        template<typename Rep, typename Period>
        bool wait_until_block_ready_for(std::chrono::duration<Rep, Period> timeout) const
        {
            return _state->wait_until_block_ready_for(timeout);
        }

        std::span<iv::Sample const> output_block(size_t channel = 0) const
        {
            std::scoped_lock lock(_state->mutex);
            return {
                _state->output_storage.data() + channel * _state->config.max_block_frames,
                _state->config.max_block_frames
            };
        }

        iv::Sample& sample_period()
        {
            return _state->sample_period;
        }

        iv::Sample sample_period() const
        {
            return _state->sample_period;
        }
    };
}
