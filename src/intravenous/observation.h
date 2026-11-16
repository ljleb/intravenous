#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <type_traits>

namespace iv {

// The finite wire primitives used by registered observation result schemas.
// Operation IDs and their result shapes remain type-owned.
enum class ObservationNumberKind : std::uint8_t { float32, float64, unsigned64 };

struct ObservationResultField {
    std::string_view name;
    ObservationNumberKind kind{};
    std::size_t byte_offset = 0;
};

struct ObservationResultData {
    std::array<std::byte, 32> bytes{};
};

// Bounded inline capture state. The registered operation owns its layout;
// the executor and delivery service only inspect the captured flag.
struct ObservationCaptureState {
    std::array<std::byte, 32> bytes{};
    bool captured = false;

    template<class T> T load() const noexcept
    {
        static_assert(std::is_trivially_copyable_v<T> && sizeof(T) <= 32);
        T result;
        std::memcpy(&result, bytes.data(), sizeof(T));
        return result;
    }
    template<class T> void store(T const& value) noexcept
    {
        static_assert(std::is_trivially_copyable_v<T> && sizeof(T) <= 32);
        std::memcpy(bytes.data(), &value, sizeof(T));
        captured = true;
    }
};

// Borrowed only during capture. Physical Sequential indices address each
// power-of-two ring; global indices belong to the result's separate provenance.
struct ObservationChannelView {
    std::byte const* storage = nullptr;
    std::size_t value_capacity = 0;
    std::size_t value_stride_bytes = 0;
    std::size_t value_delay = 0;
};

struct ObservationBlockView {
    std::span<ObservationChannelView const> channels{};
    std::uint64_t sequential_begin_index = 0;
    std::size_t value_count = 0;
};

struct ObservationOperation {
    std::string_view id;
    std::span<ObservationResultField const> result_fields{};
    void (*capture_block)(ObservationCaptureState&, ObservationBlockView const&) noexcept = nullptr;
    void (*capture_state)(ObservationCaptureState&, void const*) noexcept = nullptr;
    ObservationResultData (*project)(ObservationCaptureState const&) noexcept = nullptr;
};

namespace observation_details {
template<class T>
inline void store(ObservationResultData& result, std::size_t offset, T value) noexcept
{
    std::memcpy(result.bytes.data() + offset, &value, sizeof(value));
}

inline float sample_at(ObservationChannelView const& channel, std::uint64_t index) noexcept
{
    float value;
    auto const position = (index - channel.value_delay) & (channel.value_capacity - 1);
    std::memcpy(&value, channel.storage + position * channel.value_stride_bytes, sizeof(value));
    return value;
}

inline constexpr std::array sample_level_fields{
    ObservationResultField{"peak", ObservationNumberKind::float64, 0},
    ObservationResultField{"rms", ObservationNumberKind::float64, 8},
    ObservationResultField{"samples", ObservationNumberKind::unsigned64, 16},
    ObservationResultField{"nonfinite", ObservationNumberKind::unsigned64, 24},
};

struct SampleLevelState {
    double peak = 0;
    double sum_squares = 0;
    std::uint64_t samples = 0;
    std::uint64_t nonfinite = 0;
};
inline void capture_sample_level(ObservationCaptureState& state, ObservationBlockView const& block) noexcept
{
    auto result = state.load<SampleLevelState>();
    for (auto const& channel : block.channels) {
        for (std::size_t offset = 0; offset < block.value_count; ++offset) {
            auto const value = static_cast<double>(sample_at(channel, block.sequential_begin_index + offset));
            if (!std::isfinite(value)) { ++result.nonfinite; continue; }
            result.peak = std::fmax(result.peak, std::abs(value));
            result.sum_squares += value * value;
            ++result.samples;
        }
    }
    if (block.value_count && !block.channels.empty()) state.store(result);
}

inline ObservationResultData project_sample_level(ObservationCaptureState const& state) noexcept
{
    auto const value = state.load<SampleLevelState>();
    ObservationResultData result;
    store(result, 0, value.peak);
    store(result, 8, value.samples ? std::sqrt(value.sum_squares / static_cast<double>(value.samples)) : 0.0);
    store(result, 16, value.samples);
    store(result, 24, value.nonfinite);
    return result;
}

inline constexpr std::array sample_operations{
    ObservationOperation{"level", sample_level_fields, &capture_sample_level, nullptr, &project_sample_level},
};

inline constexpr std::array float_fields{
    ObservationResultField{"value", ObservationNumberKind::float32, 0},
};
inline constexpr std::array double_fields{
    ObservationResultField{"value", ObservationNumberKind::float64, 0},
};

template<class T>
inline void capture_scalar(ObservationCaptureState& result, void const* source) noexcept
{
    T scalar;
    std::memcpy(&scalar, source, sizeof(T));
    result.store(scalar);
}
inline ObservationResultData project_scalar(ObservationCaptureState const& value) noexcept
{
    ObservationResultData result;
    result.bytes = value.bytes;
    return result;
}
inline constexpr ObservationOperation float_latest{
    "latest", float_fields, nullptr, &capture_scalar<float>, &project_scalar};
inline constexpr ObservationOperation double_latest{
    "latest", double_fields, nullptr, &capture_scalar<double>, &project_scalar};
} // namespace observation_details

struct StateObservationCodec {
    std::string_view field_type_id;
    std::size_t size_bits = 0;
    ObservationOperation const* operation = nullptr;
};

// NodeStateFieldStructure::type_name stores the compiler type identity (USR),
// not its display spelling. Field reflection alone is not a capture permission.
// Only these exact scalar types opt in initially; pointers, containers, bitfields and Tock State do not.
inline constexpr std::array state_observation_codecs{
    StateObservationCodec{"c:f", sizeof(float) * 8, &observation_details::float_latest},
    StateObservationCodec{"c:d", sizeof(double) * 8, &observation_details::double_latest},
};

} // namespace iv
