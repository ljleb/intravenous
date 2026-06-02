#pragma once

#include "audio_device.h"
#include "../compat.h"
#include "../wav.h"

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <utility>
#include <vector>

namespace iv {
    class WavFileLogicalDevice {
        std::filesystem::path _path;
        RenderConfig _config;
        bool _request_pending = false;
        bool _flushed = false;
        std::vector<Sample> _request_buffer;
        std::vector<std::vector<Sample>> _channel_data;

        size_t request_frames() const
        {
            return _config.preferred_block_size;
        }

        void append_request()
        {
            size_t const frames = request_frames();
            size_t const channels = _config.num_channels;

            if (_channel_data.size() < channels) {
                _channel_data.resize(channels);
            }

            for (size_t channel = 0; channel < channels; ++channel) {
                auto& output = _channel_data[channel];
                size_t const base = output.size();
                output.resize(base + frames, 0.0f);
                for (size_t frame = 0; frame < frames; ++frame) {
                    output[base + frame] = _request_buffer[frame * channels + channel];
                }
            }
        }

        void close()
        {
            if (_flushed) {
                return;
            }
            _flushed = true;

            if (_channel_data.empty()) {
                return;
            }

            if (_path.has_parent_path()) {
                std::filesystem::create_directories(_path.parent_path());
            }

            write_wav(
                _path.string(),
                std::span<std::vector<Sample> const>(_channel_data.data(), _channel_data.size()),
                static_cast<std::uint32_t>(_config.sample_rate)
            );
        }

    public:
        WavFileLogicalDevice(std::filesystem::path path, RenderConfig config = {})
        : _path(std::move(path))
        , _config(std::move(config))
        {
            validate_render_config(_config);
            _request_buffer.assign(request_frames() * _config.num_channels, 0.0f);
        }

        ~WavFileLogicalDevice()
        {
            try {
                close();
            } catch (std::exception const& e) {
                diagnostic_stream() << "WavFileLogicalDevice flush failed: " << e.what() << '\n';
            } catch (...) {
                diagnostic_stream() << "WavFileLogicalDevice flush failed with unknown exception\n";
            }
        }

        WavFileLogicalDevice(WavFileLogicalDevice&&) = delete;
        WavFileLogicalDevice& operator=(WavFileLogicalDevice&&) = delete;
        WavFileLogicalDevice(WavFileLogicalDevice const&) = delete;
        WavFileLogicalDevice& operator=(WavFileLogicalDevice const&) = delete;

        RenderConfig const& config() const
        {
            return _config;
        }

        std::span<Sample> wait_for_block_request()
        {
            _request_pending = true;
            std::fill(_request_buffer.begin(), _request_buffer.end(), 0.0f);
            return std::span<Sample>(_request_buffer.data(), _request_buffer.size());
        }

        void submit_response()
        {
            if (!_request_pending) {
                throw std::logic_error("WavFileLogicalDevice has no claimed request awaiting response");
            }

            append_request();
            _request_pending = false;
        }
    };

    inline LogicalAudioDevice make_wav_file_device(std::filesystem::path path, RenderConfig config = {})
    {
        return LogicalAudioDevice(std::in_place_type<WavFileLogicalDevice>, std::move(path), std::move(config));
    }
}
