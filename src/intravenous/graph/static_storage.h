#pragma once

#include <intravenous/ports.h>

#include <cstddef>
#include <limits>
#include <span>
#include <string>
#include <string_view>

namespace iv {
template<class T>
struct StaticSpan {
    T const* data = nullptr;
    size_t size = 0;

    constexpr T const* begin() const { return data; }
    constexpr T const* end() const { return data + size; }
    constexpr bool empty() const { return size == 0; }
    constexpr T const& front() const { return data[0]; }
    constexpr T const& back() const { return data[size - 1]; }
    constexpr T const& operator[](size_t index) const { return data[index]; }
    constexpr std::span<T const> span() const { return { data, size }; }
};

struct StaticString {
    char const* data = nullptr;
    size_t size = 0;

    constexpr bool empty() const { return size == 0; }
    constexpr std::string_view view() const { return { data, size }; }
    constexpr operator std::string_view() const { return view(); }
};

struct StaticInputConfig {
    StaticString name {};
    ChannelLayout channel_layout {
        .channel_type = ChannelTypeId::mono,
        .sample_layout = SampleStreamLayout::planar,
    };
    size_t history = 0;
    Sample default_value = 0.0;
    Sample min = -std::numeric_limits<Sample::storage>::infinity();
    Sample max = std::numeric_limits<Sample::storage>::infinity();

    constexpr InputConfig config() const
    {
        return {
            .name = std::string(name.view()),
            .channel_layout = channel_layout,
            .history = history,
            .default_value = default_value,
            .min = min,
            .max = max,
        };
    }
};

struct StaticOutputConfig {
    StaticString name {};
    ChannelLayout channel_layout {
        .channel_type = ChannelTypeId::mono,
        .sample_layout = SampleStreamLayout::planar,
    };
    size_t latency = 0;
    size_t history = 0;

    constexpr OutputConfig config() const
    {
        return {
            .name = std::string(name.view()),
            .channel_layout = channel_layout,
            .latency = latency,
            .history = history,
        };
    }
};

struct StaticEventInputConfig {
    StaticString name {};
    EventTypeId type {};

    constexpr EventInputConfig config() const
    {
        return {
            .name = std::string(name.view()),
            .type = type,
        };
    }
};

struct StaticEventOutputConfig {
    StaticString name {};
    EventTypeId type {};

    constexpr EventOutputConfig config() const
    {
        return {
            .name = std::string(name.view()),
            .type = type,
        };
    }
};
}
