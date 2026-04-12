#pragma once

#include "devices/audio_device.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace iv::test {
    class FakeAudioDevice {
        iv::RenderConfig _config;
        iv::Sample _sample_period = 0.0f;
        std::mutex _mutex;
        std::condition_variable _cv;
        bool _shutdown_requested = false;
        bool _request_pending = false;
        bool _response_ready = false;
        bool _block_ready = false;
        size_t _requested_frame_index = 0;
        size_t _requested_frames = 0;
        std::vector<iv::Sample> _interleaved_output;
        std::vector<iv::Sample> _planar_output;

        void materialize_planar_output_locked()
        {
            size_t const channels = _config.num_channels;
            std::fill(_planar_output.begin(), _planar_output.end(), 0.0f);
            for (size_t frame = 0; frame < _requested_frames && frame < _config.max_block_frames; ++frame) {
                for (size_t channel = 0; channel < channels; ++channel) {
                    _planar_output[channel * _config.max_block_frames + frame] =
                        _interleaved_output[frame * channels + channel];
                }
            }
        }

    public:
        explicit FakeAudioDevice(
            iv::RenderConfig config = {},
            [[maybe_unused]] std::shared_ptr<std::condition_variable> request_notification = {}
        )
        : _config(std::move(config))
        {
            iv::validate_render_config(_config);
            _sample_period = iv::sample_period(_config);
            _interleaved_output.assign(_config.max_block_frames * _config.num_channels, 0.0f);
            _planar_output.assign(_config.max_block_frames * _config.num_channels, 0.0f);
        }

        iv::RenderConfig const& config() const
        {
            return _config;
        }

        std::span<iv::Sample> wait_for_block_request()
        {
            std::unique_lock lock(_mutex);
            _cv.wait(lock, [&] {
                return _shutdown_requested || (_request_pending && !_response_ready);
            });

            if (!_request_pending) {
                throw std::logic_error("FakeAudioDevice wait_for_block_request() interrupted without a pending request");
            }

            return std::span<iv::Sample>(
                _interleaved_output.data(),
                _requested_frames * _config.num_channels
            );
        }

        void submit_response()
        {
            {
                std::scoped_lock lock(_mutex);
                if (!_request_pending || _response_ready) {
                    throw std::logic_error("FakeAudioDevice has no pending request awaiting response");
                }

                materialize_planar_output_locked();
                _response_ready = true;
                _block_ready = true;
            }
            _cv.notify_all();
        }

        void begin_requested_block(size_t frame_index, size_t frame_count)
        {
            {
                std::scoped_lock lock(_mutex);
                if (frame_count > _config.max_block_frames) {
                    throw std::logic_error("FakeAudioDevice request exceeds max block size");
                }

                bool const preserve_previous_output = _response_ready;
                _requested_frame_index = frame_index;
                _requested_frames = frame_count;
                _request_pending = true;
                _response_ready = false;
                _block_ready = false;
                if (!preserve_previous_output) {
                    std::fill(_interleaved_output.begin(), _interleaved_output.end(), 0.0f);
                    std::fill(_planar_output.begin(), _planar_output.end(), 0.0f);
                }
            }
            _cv.notify_all();
        }

        bool wait_until_block_ready()
        {
            std::unique_lock lock(_mutex);
            _cv.wait(lock, [&] {
                return _shutdown_requested || _block_ready;
            });
            return !_shutdown_requested && _block_ready;
        }

        template<typename Rep, typename Period>
        bool wait_until_block_ready_for(std::chrono::duration<Rep, Period> timeout)
        {
            std::unique_lock lock(_mutex);
            bool const ready = _cv.wait_for(lock, timeout, [&] {
                return _shutdown_requested || _block_ready;
            });
            return ready && !_shutdown_requested && _block_ready;
        }

        void finish_requested_block()
        {
            {
                std::scoped_lock lock(_mutex);
                _request_pending = false;
                _response_ready = false;
                _block_ready = false;
                _requested_frames = 0;
            }
            _cv.notify_all();
        }

        bool is_block_ready()
        {
            std::scoped_lock lock(_mutex);
            return _block_ready;
        }

        std::span<iv::Sample const> output_block(size_t channel = 0)
        {
            std::scoped_lock lock(_mutex);
            return {
                _planar_output.data() + channel * _config.max_block_frames,
                _config.max_block_frames
            };
        }

        std::span<iv::Sample const> output_block(size_t channel = 0) const
        {
            return const_cast<FakeAudioDevice*>(this)->output_block(channel);
        }

        iv::Sample& sample_period()
        {
            return _sample_period;
        }

        iv::Sample sample_period() const
        {
            return _sample_period;
        }

        size_t requested_frame_index()
        {
            std::scoped_lock lock(_mutex);
            return _requested_frame_index;
        }

        void request_shutdown()
        {
            {
                std::scoped_lock lock(_mutex);
                _shutdown_requested = true;
            }
            _cv.notify_all();
        }
    };
}
