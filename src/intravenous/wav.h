#pragma once

#include "sample.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>


namespace iv {
    namespace details {
        inline void write_u16_le(std::ostream& os, std::uint16_t v)
        {
            char b[2]{
                static_cast<char>(v & 0xFF),
                static_cast<char>((v >> 8) & 0xFF)
            };
            os.write(b, 2);
        }

        inline void write_u32_le(std::ostream& os, std::uint32_t v)
        {
            char b[4]{
                static_cast<char>(v & 0xFF),
                static_cast<char>((v >> 8) & 0xFF),
                static_cast<char>((v >> 16) & 0xFF),
                static_cast<char>((v >> 24) & 0xFF)
            };
            os.write(b, 4);
        }

        inline std::int16_t float_to_pcm16(Sample x)
        {
            x = std::clamp(x, Sample(-1), Sample(1));
            if (x >= Sample(1)) {
                return 32767;
            }
            if (x <= Sample(-1)) {
                return -32768;
            }
            return static_cast<std::int16_t>(x * 32767.0f);
        }

        template<typename SampleAt>
        inline void write_pcm16_wav(
            std::string const& path,
            size_t channel_count,
            size_t frame_count,
            std::uint32_t sample_rate,
            SampleAt&& sample_at)
        {
            if (channel_count == 0) {
                throw std::logic_error("write_wav: channel count must be > 0");
            }
            if (channel_count > std::numeric_limits<std::uint16_t>::max()) {
                throw std::logic_error("write_wav: channel count exceeds WAV header capacity");
            }
            if (sample_rate == 0) {
                throw std::logic_error("write_wav: sample_rate must be > 0");
            }

            constexpr std::uint16_t bits_per_sample = 16;
            constexpr size_t bytes_per_sample = bits_per_sample / 8;
            if (channel_count > (std::numeric_limits<std::uint16_t>::max() / bytes_per_sample)) {
                throw std::logic_error("write_wav: block align exceeds WAV header capacity");
            }
            std::uint16_t const num_channels = static_cast<std::uint16_t>(channel_count);
            std::uint16_t const block_align = static_cast<std::uint16_t>(channel_count * bytes_per_sample);
            if (sample_rate > (std::numeric_limits<std::uint32_t>::max() / block_align)) {
                throw std::logic_error("write_wav: byte rate exceeds WAV header capacity");
            }
            if (frame_count > (std::numeric_limits<std::uint32_t>::max() / block_align)) {
                throw std::logic_error("write_wav: file too large for simple WAV writer");
            }

            std::uint32_t const byte_rate = sample_rate * block_align;
            std::uint32_t const data_size = static_cast<std::uint32_t>(frame_count * block_align);
            std::uint32_t const riff_chunk_size = 36u + data_size;

            std::ofstream os(path, std::ios::binary);
            if (!os) {
                throw std::logic_error("write_wav: failed to open output file '" + path + "'");
            }

            os.write("RIFF", 4);
            write_u32_le(os, riff_chunk_size);
            os.write("WAVE", 4);

            os.write("fmt ", 4);
            write_u32_le(os, 16);
            write_u16_le(os, 1);
            write_u16_le(os, num_channels);
            write_u32_le(os, sample_rate);
            write_u32_le(os, byte_rate);
            write_u16_le(os, block_align);
            write_u16_le(os, bits_per_sample);

            os.write("data", 4);
            write_u32_le(os, data_size);

            for (size_t frame = 0; frame < frame_count; ++frame) {
                for (size_t channel = 0; channel < channel_count; ++channel) {
                    std::int16_t const sample = float_to_pcm16(sample_at(channel, frame));
                    write_u16_le(os, static_cast<std::uint16_t>(sample));
                }
            }

            if (!os) {
                throw std::logic_error("write_wav: failed while writing '" + path + "'");
            }
        }
    }

    inline void write_wav(
        std::string const& path,
        std::span<std::vector<Sample> const> channels,
        std::uint32_t sample_rate)
    {
        size_t frame_count = 0;
        for (auto const& channel : channels) {
            frame_count = std::max(frame_count, channel.size());
        }

        details::write_pcm16_wav(
            path,
            channels.size(),
            frame_count,
            sample_rate,
            [&](size_t channel, size_t frame) {
                auto const& samples = channels[channel];
                return frame < samples.size() ? samples[frame] : Sample{0.0f};
            }
        );
    }

    inline void write_wav(
        std::string const& path,
        std::span<Sample const> left,
        std::span<Sample const> right,
        std::uint32_t sample_rate)
    {
        if (left.size() != right.size()) {
            throw std::logic_error("write_wav: left/right channel sizes differ");
        }
        details::write_pcm16_wav(
            path,
            2,
            left.size(),
            sample_rate,
            [&](size_t channel, size_t frame) -> Sample {
                return channel == 0 ? left[frame] : right[frame];
            }
        );
    }
}
