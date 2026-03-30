#pragma once

#include "ports.h"
#include "third_party/miniaudio/miniaudio.h"

#include <cmath>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace iv {
    struct RenderConfig {
        size_t sample_rate = 48000;
        size_t num_channels = 2;
        size_t max_block_frames = 4096;
        size_t preferred_block_size = 256;
    };

    class AudioDevicePlayback {
        std::shared_ptr<void> _state;
        size_t (*_preferred_block_size_fn)(void*) = nullptr;
        void (*_begin_block_fn)(void*, size_t) = nullptr;
        size_t (*_active_block_start_fn)(void const*) = nullptr;
        size_t (*_active_block_end_fn)(void const*) = nullptr;
        size_t (*_active_block_frames_fn)(void const*) = nullptr;
        void (*_mix_sink_block_fn)(void*, size_t, std::span<Sample const>, size_t, size_t) = nullptr;
        void (*_finish_tick_fn)(void*, size_t) = nullptr;
        std::span<Sample const> (*_output_block_fn)(void const*, size_t) = nullptr;
        bool (*_is_shutdown_requested_fn)(void const*) = nullptr;
        void (*_request_shutdown_fn)(void*) = nullptr;

    public:
        AudioDevicePlayback() = default;

        template<typename State>
        explicit AudioDevicePlayback(std::shared_ptr<State> state) :
            _state(std::move(state)),
            _preferred_block_size_fn([](void* state_ptr) {
                return static_cast<State*>(state_ptr)->preferred_block_size();
            }),
            _begin_block_fn([](void* state_ptr, size_t global_index) {
                static_cast<State*>(state_ptr)->begin_block(global_index);
            }),
            _active_block_start_fn([](void const* state_ptr) {
                return static_cast<State const*>(state_ptr)->active_block_start();
            }),
            _active_block_end_fn([](void const* state_ptr) {
                return static_cast<State const*>(state_ptr)->active_block_end();
            }),
            _active_block_frames_fn([](void const* state_ptr) {
                return static_cast<State const*>(state_ptr)->active_block_frames();
            }),
            _mix_sink_block_fn([](void* state_ptr, size_t channel, std::span<Sample const> sink_buffer, size_t block_start, size_t frames) {
                static_cast<State*>(state_ptr)->mix_sink_block(channel, sink_buffer, block_start, frames);
            }),
            _finish_tick_fn([](void* state_ptr, size_t global_index) {
                static_cast<State*>(state_ptr)->finish_tick(global_index);
            }),
            _output_block_fn([](void const* state_ptr, size_t channel) {
                return static_cast<State const*>(state_ptr)->output_block(channel);
            }),
            _is_shutdown_requested_fn([](void const* state_ptr) {
                return static_cast<State const*>(state_ptr)->is_shutdown_requested();
            }),
            _request_shutdown_fn([](void* state_ptr) {
                static_cast<State*>(state_ptr)->request_shutdown();
            })
        {}

        explicit operator bool() const
        {
            return static_cast<bool>(_state);
        }

        size_t preferred_block_size() const
        {
            return _preferred_block_size_fn(_state.get());
        }

        void begin_block(size_t global_index)
        {
            _begin_block_fn(_state.get(), global_index);
        }

        size_t active_block_start() const
        {
            return _active_block_start_fn(_state.get());
        }

        size_t active_block_end() const
        {
            return _active_block_end_fn(_state.get());
        }

        size_t active_block_frames() const
        {
            return _active_block_frames_fn(_state.get());
        }

        void mix_sink_block(size_t channel, std::span<Sample const> sink_buffer, size_t block_start, size_t frames)
        {
            _mix_sink_block_fn(_state.get(), channel, sink_buffer, block_start, frames);
        }

        void finish_tick(size_t global_index)
        {
            _finish_tick_fn(_state.get(), global_index);
        }

        std::span<Sample const> output_block(size_t channel) const
        {
            return _output_block_fn(_state.get(), channel);
        }

        bool is_shutdown_requested() const
        {
            return _is_shutdown_requested_fn(_state.get());
        }

        void request_shutdown()
        {
            _request_shutdown_fn(_state.get());
        }
    };

    namespace details {
        struct FakeAudioPlayback {
            RenderConfig config;
            std::vector<std::vector<Sample>> planar_storage;
            size_t active_block_start_index = 0;
            size_t active_block_end_index = 0;
            bool shutdown_requested = false;

            explicit FakeAudioPlayback(RenderConfig config_) :
                config(config_),
                planar_storage(config.num_channels, std::vector<Sample>(config.max_block_frames, 0.0f))
            {}

            size_t preferred_block_size() const
            {
                return prev_power_of_2(config.max_block_frames);
            }

            void begin_block(size_t global_index)
            {
                for (size_t channel = 0; channel < config.num_channels; ++channel) {
                    std::fill(planar_storage[channel].begin(), planar_storage[channel].end(), 0.0f);
                }
                active_block_start_index = global_index;
                active_block_end_index = global_index + config.max_block_frames;
            }

            size_t active_block_start() const
            {
                return active_block_start_index;
            }

            size_t active_block_end() const
            {
                return active_block_end_index;
            }

            size_t active_block_frames() const
            {
                return active_block_end_index - active_block_start_index;
            }

            void mix_sink_block(size_t channel, std::span<Sample const> sink_buffer, size_t block_start, size_t frames)
            {
                if (channel >= config.num_channels || sink_buffer.empty()) {
                    return;
                }

                for (size_t frame = 0; frame < frames; ++frame) {
                    size_t const global_index = block_start + frame;
                    planar_storage[channel][frame] += sink_buffer[global_index & (sink_buffer.size() - 1)];
                }
            }

            void finish_tick(size_t)
            {}

            std::span<Sample const> output_block(size_t channel) const
            {
                return planar_storage.at(channel);
            }

            bool is_shutdown_requested() const
            {
                return shutdown_requested;
            }

            void request_shutdown()
            {
                shutdown_requested = true;
            }
        };

        struct MiniaudioPlayback {
            ma_context context {};
            ma_device device {};

            bool context_initialized = false;
            bool device_initialized = false;
            bool device_started = false;
            bool shutdown_requested = false;
            bool render_requested = false;
            bool render_in_progress = false;
            bool render_finished = false;
            bool block_open = false;

            RenderConfig config;
            std::vector<std::vector<Sample>> planar_storage;
            std::vector<Sample*> channel_ptrs;

            size_t requested_frames = 0;
            size_t requested_channels = 0;
            size_t active_block_start_index = 0;
            size_t active_block_end_index = 0;
            std::mutex mutex;
            std::condition_variable cv;

            explicit MiniaudioPlayback(RenderConfig config_) :
                config(config_),
                planar_storage(config.num_channels, std::vector<Sample>(config.max_block_frames, 0.0f)),
                channel_ptrs(config.num_channels, nullptr)
            {
                for (size_t channel = 0; channel < config.num_channels; ++channel) {
                    channel_ptrs[channel] = planar_storage[channel].data();
                }
            }

            ~MiniaudioPlayback()
            {
                request_shutdown();
                shutdown_backend();
            }

            static void data_callback(ma_device* device, void* output, void const*, ma_uint32 frame_count)
            {
                auto* self = static_cast<MiniaudioPlayback*>(device->pUserData);
                auto* out = static_cast<float*>(output);
                if (!self) {
                    return;
                }

                std::unique_lock lock(self->mutex);

                if (
                    self->shutdown_requested ||
                    device->playback.channels > self->config.num_channels ||
                    frame_count > self->config.max_block_frames
                ) {
                    self->write_silence(out, frame_count, device->playback.channels);
                    return;
                }

                self->requested_frames = frame_count;
                self->requested_channels = device->playback.channels;
                self->render_requested = true;
                self->render_in_progress = false;
                self->render_finished = false;
                self->cv.notify_all();

                self->cv.wait(lock, [&] {
                    return self->render_finished || self->shutdown_requested;
                });

                if (self->shutdown_requested) {
                    self->write_silence(out, frame_count, device->playback.channels);
                    return;
                }

                self->interleave_to(out);
                self->render_finished = false;
                self->cv.notify_all();
            }

            void ensure_backend_initialized()
            {
                if (shutdown_requested || device_initialized) {
                    return;
                }

                ma_result result = ma_context_init(nullptr, 0, nullptr, &context);
                if (result != MA_SUCCESS) {
                    throw std::runtime_error("ma_context_init failed");
                }
                context_initialized = true;

                ma_device_config device_config = ma_device_config_init(ma_device_type_playback);
                device_config.playback.format = ma_format_f32;
                device_config.playback.channels = static_cast<ma_uint32>(config.num_channels);
                device_config.sampleRate = static_cast<ma_uint32>(config.sample_rate);
                device_config.dataCallback = data_callback;
                device_config.pUserData = this;

                result = ma_device_init(&context, &device_config, &device);
                if (result != MA_SUCCESS) {
                    shutdown_backend();
                    throw std::runtime_error("ma_device_init failed");
                }
                device_initialized = true;

                config.sample_rate = device.sampleRate;
                config.num_channels = device.playback.channels;
                if (device.playback.internalPeriodSizeInFrames != 0) {
                    config.max_block_frames = device.playback.internalPeriodSizeInFrames;
                }

                planar_storage.assign(config.num_channels, std::vector<Sample>(config.max_block_frames, 0.0f));
                channel_ptrs.assign(config.num_channels, nullptr);
                for (size_t channel = 0; channel < config.num_channels; ++channel) {
                    channel_ptrs[channel] = planar_storage[channel].data();
                }
            }

            void ensure_backend_started()
            {
                ensure_backend_initialized();
                if (shutdown_requested || device_started) {
                    return;
                }

                ma_result result = ma_device_start(&device);
                if (result != MA_SUCCESS) {
                    shutdown_backend();
                    throw std::runtime_error("ma_device_start failed");
                }
                device_started = true;
            }

            size_t preferred_block_size()
            {
                ensure_backend_initialized();
                return prev_power_of_2(config.max_block_frames);
            }

            void begin_block(size_t global_index)
            {
                if (block_open || shutdown_requested) {
                    return;
                }

                ensure_backend_started();

                std::unique_lock lock(mutex);
                cv.wait(lock, [&] {
                    return render_requested || shutdown_requested;
                });

                if (shutdown_requested) {
                    return;
                }

                render_requested = false;
                render_in_progress = true;
                clear_staging(requested_channels, requested_frames);
                block_open = true;
                active_block_start_index = global_index;
                active_block_end_index = global_index + requested_frames;
            }

            size_t active_block_start() const
            {
                return active_block_start_index;
            }

            size_t active_block_end() const
            {
                return active_block_end_index;
            }

            size_t active_block_frames() const
            {
                return active_block_end_index - active_block_start_index;
            }

            void mix_sink_block(size_t channel, std::span<Sample const> sink_buffer, size_t block_start, size_t frames)
            {
                if (channel >= requested_channels || sink_buffer.empty()) {
                    return;
                }

                for (size_t frame = 0; frame < frames; ++frame) {
                    size_t const global_index = block_start + frame;
                    size_t const local_frame = global_index - active_block_start_index;
                    planar_storage[channel][local_frame] += sink_buffer[global_index & (sink_buffer.size() - 1)];
                }
            }

            void finish_tick(size_t global_index)
            {
                if (!block_open || global_index + 1 < active_block_end_index) {
                    return;
                }
                block_open = false;

                {
                    std::lock_guard lock(mutex);
                    if (shutdown_requested || !render_in_progress) {
                        return;
                    }
                    render_in_progress = false;
                    render_finished = true;
                }
                cv.notify_all();
            }

            std::span<Sample const> output_block(size_t channel) const
            {
                return planar_storage.at(channel);
            }

            bool is_shutdown_requested() const
            {
                return shutdown_requested;
            }

            void request_shutdown()
            {
                {
                    std::lock_guard lock(mutex);
                    shutdown_requested = true;
                }
                cv.notify_all();
            }

        private:
            void clear_staging(size_t channels, size_t frames)
            {
                for (size_t channel = 0; channel < channels; ++channel) {
                    std::fill_n(planar_storage[channel].data(), frames, 0.0f);
                }
            }

            void interleave_to(float* output) const
            {
                for (size_t frame = 0; frame < requested_frames; ++frame) {
                    for (size_t channel = 0; channel < requested_channels; ++channel) {
                        output[frame * requested_channels + channel] = planar_storage[channel][frame];
                    }
                }
            }

            void write_silence(float* output, size_t frames, size_t channels) const
            {
                std::memset(output, 0, frames * channels * sizeof(float));
            }

            void shutdown_backend()
            {
                if (device_initialized) {
                    ma_device_uninit(&device);
                    device_initialized = false;
                    device_started = false;
                }

                if (context_initialized) {
                    ma_context_uninit(&context);
                    context_initialized = false;
                }
            }
        };
    }

    class AudioDevice {
        bool _open_backend = true;
        RenderConfig _config;
        Sample _sample_period;

    public:
        explicit AudioDevice(RenderConfig config = {}, bool open_backend = true) :
            _open_backend(open_backend),
            _config(config),
            _sample_period(static_cast<Sample>(1) / static_cast<Sample>(config.sample_rate))
        {
            if (_config.sample_rate == 0 || _config.num_channels == 0 || _config.max_block_frames == 0) {
                throw std::invalid_argument("render config values must be non-zero");
            }
            try {
                validate_block_size(_config.preferred_block_size, "preferred_block_size must be a power of 2");
            } catch (std::logic_error const& e) {
                throw std::invalid_argument(e.what());
            }
        }

        RenderConfig const& config() const
        {
            return _config;
        }

        bool open_backend() const
        {
            return _open_backend;
        }

        Sample& sample_period()
        {
            return _sample_period;
        }

        Sample sample_period() const
        {
            return _sample_period;
        }

        size_t processing_block_size() const
        {
            return _config.max_block_frames;
        }

        size_t scheduling_block_size() const
        {
            return prev_power_of_2(processing_block_size());
        }

        size_t sink_buffer_size() const
        {
            return size_t(1) << size_t(std::ceil(std::log2(_config.max_block_frames)));
        }

        AudioDevicePlayback make_playback() const
        {
            if (_open_backend) {
                return AudioDevicePlayback(std::make_shared<details::MiniaudioPlayback>(_config));
            }
            return AudioDevicePlayback(std::make_shared<details::FakeAudioPlayback>(_config));
        }
    };
}
