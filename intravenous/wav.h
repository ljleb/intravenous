#include "node.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>


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
        if (sample_rate == 0) {
            throw std::logic_error("write_wav: sample_rate must be > 0");
        }
        if (left.size() > (std::numeric_limits<std::uint32_t>::max() / 4u)) {
            throw std::logic_error("write_wav: file too large for simple WAV writer");
        }

        constexpr std::uint16_t num_channels = 2;
        constexpr std::uint16_t bits_per_sample = 16;
        constexpr std::uint16_t block_align = num_channels * (bits_per_sample / 8); // 4
        std::uint32_t const byte_rate = sample_rate * block_align;
        std::uint32_t const data_size = static_cast<std::uint32_t>(left.size() * block_align);
        std::uint32_t const riff_chunk_size = 36u + data_size;

        std::ofstream os(path, std::ios::binary);
        if (!os) {
            throw std::logic_error("write_wav: failed to open output file '" + path + "'");
        }

        // RIFF header
        os.write("RIFF", 4);
        details::write_u32_le(os, riff_chunk_size);
        os.write("WAVE", 4);

        // fmt chunk
        os.write("fmt ", 4);
        details::write_u32_le(os, 16);                 // PCM fmt chunk size
        details::write_u16_le(os, 1);                  // audio format = PCM
        details::write_u16_le(os, num_channels);       // channels
        details::write_u32_le(os, sample_rate);        // sample rate
        details::write_u32_le(os, byte_rate);          // byte rate
        details::write_u16_le(os, block_align);        // block align
        details::write_u16_le(os, bits_per_sample);    // bits per sample

        // data chunk
        os.write("data", 4);
        details::write_u32_le(os, data_size);

        // interleaved samples
        for (size_t i = 0; i < left.size(); ++i) {
            std::int16_t l = details::float_to_pcm16(left[i]);
            std::int16_t r = details::float_to_pcm16(right[i]);
            details::write_u16_le(os, static_cast<std::uint16_t>(l));
            details::write_u16_le(os, static_cast<std::uint16_t>(r));
        }

        if (!os) {
            throw std::logic_error("write_wav: failed while writing '" + path + "'");
        }
    }
}
