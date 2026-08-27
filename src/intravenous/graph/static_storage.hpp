#pragma once

#include <intravenous/ports.h>

#include <meta>

#include <cstddef>
#include <limits>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

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

namespace details {
template<std::ranges::contiguous_range Range>
consteval auto define_static_span(Range const& range)
{
    using Value = std::remove_cv_t<std::ranges::range_value_t<Range>>;
    auto const storage = std::define_static_array(range);
    return StaticSpan<Value>{ storage.data(), storage.size() };
}

template<class String>
consteval StaticString define_static_string(String const& string)
{
    auto const view = std::string_view(string);
    return {
        .data = std::define_static_string(view),
        .size = view.size(),
    };
}

consteval StaticInputConfig define_static_config(InputConfig const& config)
{
    return {
        .name = define_static_string(config.name),
        .channel_layout = config.channel_layout,
        .history = config.history,
        .default_value = config.default_value,
        .min = config.min,
        .max = config.max,
    };
}

consteval StaticOutputConfig define_static_config(OutputConfig const& config)
{
    return {
        .name = define_static_string(config.name),
        .channel_layout = config.channel_layout,
        .latency = config.latency,
        .history = config.history,
    };
}

consteval StaticEventInputConfig define_static_config(
    EventInputConfig const& config)
{
    return {
        .name = define_static_string(config.name),
        .type = config.type,
    };
}

consteval StaticEventOutputConfig define_static_config(
    EventOutputConfig const& config)
{
    return {
        .name = define_static_string(config.name),
        .type = config.type,
    };
}

template<class StaticConfig, class ConfigRange>
consteval StaticSpan<StaticConfig> define_static_configs(
    ConfigRange const& configs)
{
    std::vector<StaticConfig> static_configs;
    static_configs.reserve(std::ranges::size(configs));
    for (auto const& config : configs) {
        static_configs.push_back(define_static_config(config));
    }
    return define_static_span(static_configs);
}
} // namespace details
} // namespace iv
