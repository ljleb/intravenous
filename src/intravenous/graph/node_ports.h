#pragma once

// Owning logical port descriptions belong to the retained graph. Port kind is
// encoded by InputConfig/OutputConfig; sample/event execution descriptors are
// derived only where kind-specific graph machinery needs them.

#include <intravenous/graph/port_ids.h>
#include <intravenous/ports.h>

#include <stdexcept>
#include <utility>
#include <vector>

namespace iv {

struct NodePorts {
    std::vector<InputConfig> input_configs {};
    std::vector<OutputConfig> output_configs {};

    constexpr std::vector<InputConfig> const& inputs() const
    {
        return input_configs;
    }

    constexpr std::vector<OutputConfig> const& outputs() const
    {
        return output_configs;
    }


    constexpr std::vector<SampleInputConfig> sample_inputs() const
    {
        std::vector<SampleInputConfig> result;
        result.reserve(sample_input_count());
        for (InputConfig const& config : input_configs) {
            if (is_sample(config)) result.push_back(materialize_sample_config(config));
        }
        return result;
    }

    constexpr std::vector<SampleOutputConfig> sample_outputs() const
    {
        std::vector<SampleOutputConfig> result;
        result.reserve(sample_output_count());
        for (OutputConfig const& config : output_configs) {
            if (is_sample(config)) result.push_back(materialize_sample_config(config));
        }
        return result;
    }

    constexpr std::vector<EventInputConfig> event_inputs() const
    {
        std::vector<EventInputConfig> result;
        result.reserve(event_input_count());
        for (InputConfig const& config : input_configs) {
            if (!is_sample(config)) result.push_back(materialize_event_config(config));
        }
        return result;
    }

    constexpr std::vector<EventOutputConfig> event_outputs() const
    {
        std::vector<EventOutputConfig> result;
        result.reserve(event_output_count());
        for (OutputConfig const& config : output_configs) {
            if (!is_sample(config)) result.push_back(materialize_event_config(config));
        }
        return result;
    }

    constexpr size_t sample_input_count() const
    {
        return count_sample_ports(input_configs);
    }

    constexpr size_t sample_output_count() const
    {
        return count_sample_ports(output_configs);
    }

    constexpr size_t event_input_count() const
    {
        return count_event_ports(input_configs);
    }

    constexpr size_t event_output_count() const
    {
        return count_event_ports(output_configs);
    }

    constexpr SampleInputConfig sample_input(size_t ordinal) const
    {
        for (InputConfig const& config : input_configs) {
            if (!is_sample(config)) continue;
            if (ordinal-- == 0) return materialize_sample_config(config);
        }
        throw std::out_of_range("sample input ordinal is out of bounds");
    }

    constexpr SampleOutputConfig sample_output(size_t ordinal) const
    {
        for (OutputConfig const& config : output_configs) {
            if (!is_sample(config)) continue;
            if (ordinal-- == 0) return materialize_sample_config(config);
        }
        throw std::out_of_range("sample output ordinal is out of bounds");
    }

    constexpr EventInputConfig event_input(size_t ordinal) const
    {
        for (InputConfig const& config : input_configs) {
            if (is_sample(config)) continue;
            if (ordinal-- == 0) return materialize_event_config(config);
        }
        throw std::out_of_range("event input ordinal is out of bounds");
    }

    constexpr EventOutputConfig event_output(size_t ordinal) const
    {
        for (OutputConfig const& config : output_configs) {
            if (is_sample(config)) continue;
            if (ordinal-- == 0) return materialize_event_config(config);
        }
        throw std::out_of_range("event output ordinal is out of bounds");
    }

    constexpr NodeBundlePortId input_port_at(
        NodeBundleHandle node_bundle_handle, size_t position) const
    {
        if (position >= input_configs.size()) {
            throw std::out_of_range("positional input is out of bounds");
        }
        auto const sample = is_sample(input_configs[position]);
        size_t ordinal = 0;
        for (size_t index = 0; index < position; ++index) {
            ordinal += is_sample(input_configs[index]) == sample;
        }
        return {
            node_bundle_handle,
            sample ? PortKind::sample : PortKind::event,
            ordinal,
        };
    }
};

} // namespace iv
