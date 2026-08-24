#pragma once

#include <intravenous/graph/connection_node.h>
#include <intravenous/graph/static_storage.hpp>

#include <algorithm>
#include <cstdint>
#include <ranges>
#include <stdexcept>
#include <vector>

namespace iv::details {
consteval ConnectionNode freeze_connection_node(ConnectionNodeSpec const& spec)
{
    auto const output_channel_count =
        channel_count(spec.output_config.channel_layout);
    std::vector<std::vector<bool>> gathered_channels;
    gathered_channels.reserve(spec.ephemeral_port_configs.size());
    for (auto const& ephemeral : spec.ephemeral_port_configs) {
        if (!ephemeral.conversion
            || ephemeral.conversion.source != ephemeral.channel_layout) {
            throw std::logic_error(
                "ConnectionNode ephemeral conversion source layout mismatch");
        }
        gathered_channels.emplace_back(
            channel_count(ephemeral.channel_layout), false);
        auto const converted_channels =
            channel_count(ephemeral.conversion.target);
        for (auto const copy : ephemeral.output_channel_copies) {
            if (copy.converted_channel >= converted_channels
                || copy.output_channel >= output_channel_count) {
                throw std::logic_error(
                    "ConnectionNode output channel copy is out of bounds");
            }
        }
    }

    for (auto const& input : spec.input_configs) {
        auto const input_channels = channel_count(input.input.channel_layout);
        for (auto const copy : input.channel_copies) {
            if (copy.input_channel >= input_channels
                || copy.ephemeral_port >= spec.ephemeral_port_configs.size()
                || copy.ephemeral_channel
                    >= gathered_channels[copy.ephemeral_port].size()) {
                throw std::logic_error(
                    "ConnectionNode input channel copy is out of bounds");
            }
            if (gathered_channels[copy.ephemeral_port]
                                   [copy.ephemeral_channel]) {
                throw std::logic_error(
                    "ConnectionNode ephemeral channel has multiple gathers");
            }
            gathered_channels[copy.ephemeral_port]
                               [copy.ephemeral_channel] = true;
        }
    }
    for (auto const& channels : gathered_channels) {
        if (!std::ranges::all_of(channels, [](bool value) { return value; })) {
            throw std::logic_error(
                "ConnectionNode ephemeral channel has no gathered source");
        }
    }

    std::vector<StaticConnectionNodeInputConfig> input_configs;
    input_configs.reserve(spec.input_configs.size());
    for (auto const& input : spec.input_configs) {
        input_configs.push_back({
            .name = define_static_string(input.input.name),
            .channel_layout = input.input.channel_layout,
            .history = input.input.history,
            .default_value = input.input.default_value,
            .min = input.input.min,
            .max = input.input.max,
            .channel_copies = define_static_span(input.channel_copies),
        });
    }

    std::vector<StaticConnectionNodeEphemeralPortConfig> ephemeral_configs;
    ephemeral_configs.reserve(spec.ephemeral_port_configs.size());
    std::vector<size_t> ephemeral_channel_offsets;
    ephemeral_channel_offsets.reserve(spec.ephemeral_port_configs.size());
    std::vector<std::uint8_t> contributed_output_channels(
        output_channel_count, 0);
    size_t gathered_channel_count = 0;
    size_t converted_channel_count = 0;
    for (auto const& ephemeral : spec.ephemeral_port_configs) {
        ephemeral_configs.push_back({
            .channel_layout = ephemeral.channel_layout,
            .conversion = ephemeral.conversion,
            .output_channel_copies =
                define_static_span(ephemeral.output_channel_copies),
        });
        ephemeral_channel_offsets.push_back(gathered_channel_count);
        gathered_channel_count += channel_count(ephemeral.channel_layout);
        converted_channel_count = std::max(
            converted_channel_count,
            channel_count(ephemeral.conversion.target));
        for (auto const copy : ephemeral.output_channel_copies)
            contributed_output_channels[copy.output_channel] = 1;
    }

    return {
        .input_configs = define_static_span(input_configs),
        .ephemeral_port_configs = define_static_span(ephemeral_configs),
        .output_config = {
            .name = define_static_string(spec.output_config.name),
            .channel_layout = spec.output_config.channel_layout,
            .latency = spec.output_config.latency,
            .history = spec.output_config.history,
        },
        .default_value = spec.default_value,
        .runtime_binding_id = define_static_string(spec.runtime_binding_id),
        .runtime_source_channel_offset = spec.runtime_source_channel_offset,
        .ephemeral_channel_offsets =
            define_static_span(ephemeral_channel_offsets),
        .contributed_output_channels =
            define_static_span(contributed_output_channels),
        .output_channel_count = output_channel_count,
        .gathered_channel_count = gathered_channel_count,
        .converted_channel_count = converted_channel_count,
    };
}

consteval ConnectionNode freeze_generated_node(ConnectionNodeSpec const& spec)
{
    return freeze_connection_node(spec);
}
}
