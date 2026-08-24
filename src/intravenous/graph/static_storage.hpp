#pragma once

#include <intravenous/graph/static_storage.h>

#include <meta>

#include <ranges>
#include <string_view>
#include <type_traits>
#include <vector>

namespace iv::details {
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
}
