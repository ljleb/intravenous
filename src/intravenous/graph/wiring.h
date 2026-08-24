#pragma once

#include <intravenous/ports.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace iv {
    namespace wiring_details {
        constexpr void append_decimal(std::string& string, size_t value)
        {
            char digits[std::numeric_limits<size_t>::digits10 + 2] {};
            size_t begin = sizeof(digits);
            do {
                digits[--begin] = static_cast<char>('0' + value % 10);
                value /= 10;
            } while (value != 0);
            string.append(digits + begin, digits + sizeof(digits));
        }
    }

    struct PortBufferPlan {
        size_t connection_max_block_size;
        size_t corrected_latency;
        size_t input_history;
        size_t output_history;
    };

    constexpr std::string port_data_export_id(std::string_view node_id)
    {
        std::string id = "port_data:";
        id += node_id;
        return id;
    }

    constexpr std::string port_data_export_id(std::string_view node_id, size_t port_index)
    {
        std::string id = port_data_export_id(node_id);
        id += ":";
        wiring_details::append_decimal(id, port_index);
        return id;
    }

    constexpr std::string graph_port_data_export_id(std::string_view graph_id)
    {
        std::string id = "graph_port_data:";
        id += graph_id;
        return id;
    }

    constexpr std::string graph_port_data_export_id(std::string_view graph_id, size_t port_index)
    {
        std::string id = graph_port_data_export_id(graph_id);
        id += ":";
        wiring_details::append_decimal(id, port_index);
        return id;
    }

    constexpr std::string event_port_data_export_id(std::string_view node_id)
    {
        std::string id = "event_port_data:";
        id += node_id;
        return id;
    }

    constexpr std::string event_port_data_export_id(std::string_view node_id, size_t port_index)
    {
        std::string id = event_port_data_export_id(node_id);
        id += ":";
        wiring_details::append_decimal(id, port_index);
        return id;
    }

    constexpr std::string graph_event_port_data_export_id(std::string_view graph_id)
    {
        std::string id = "graph_event_port_data:";
        id += graph_id;
        return id;
    }

    constexpr std::string graph_event_port_data_export_id(std::string_view graph_id, size_t port_index)
    {
        std::string id = graph_event_port_data_export_id(graph_id);
        id += ":";
        wiring_details::append_decimal(id, port_index);
        return id;
    }

    constexpr std::string graph_dormancy_node_skip_export_id(std::string_view graph_id)
    {
        std::string id = "graph_dormancy_node_skip:";
        id += graph_id;
        return id;
    }

    inline size_t calculate_port_buffer_size(
        size_t block_size,
        size_t latency,
        size_t input_history,
        size_t output_history
    )
    {
        size_t const min_size = block_size + latency + std::max(input_history, output_history);
        return next_power_of_2(min_size);
    }

    inline size_t calculate_port_buffer_size(size_t host_block_size, PortBufferPlan const& plan)
    {
        return calculate_port_buffer_size(
            std::min(host_block_size, plan.connection_max_block_size),
            plan.corrected_latency,
            plan.input_history,
            plan.output_history
        );
    }

    inline std::vector<size_t> resolve_port_buffer_sizes(
        size_t host_block_size,
        std::span<PortBufferPlan const> plans
    )
    {
        std::vector<size_t> sizes;
        sizes.reserve(plans.size());
        for (auto const& plan : plans) {
            sizes.push_back(calculate_port_buffer_size(host_block_size, plan));
        }
        return sizes;
    }

    inline std::vector<size_t> make_input_sample_offsets(std::span<size_t const> input_buffer_sizes)
    {
        std::vector<size_t> offsets;
        offsets.reserve(input_buffer_sizes.size() + 1);
        offsets.push_back(0);
        for (size_t buffer_size : input_buffer_sizes) {
            offsets.push_back(offsets.back() + buffer_size);
        }
        return offsets;
    }

    inline std::span<Sample> input_sample_buffer(
        std::span<Sample> samples,
        std::span<size_t const> input_offsets,
        size_t input_index
    )
    {
        return samples.subspan(
            input_offsets[input_index],
            input_offsets[input_index + 1] - input_offsets[input_index]
        );
    }

    inline void push_input_blocks_to_private_outputs(
        std::span<OutputPort> private_outputs,
        std::span<InputPort> public_inputs,
        size_t block_size
    )
    {
        for (size_t i = 0; i < public_inputs.size(); ++i) {
            auto const source_layout = public_inputs[i].channel_layout();
            IV_ASSERT(private_outputs[i].source_layout() == source_layout, "graph ingress output source layout does not match public input layout");
            Sample frame[2] {};
            for (size_t sample_i = 0; sample_i < block_size; ++sample_i) {
                for (size_t channel = 0; channel < channel_count(source_layout); ++channel) {
                    frame[channel] = public_inputs[i].get_frame(sample_i, channel);
                }
                private_outputs[i].push_frame(std::span<Sample const>(frame, channel_count(source_layout)));
            }
        }
    }

    inline void push_private_inputs_to_output_blocks(
        std::span<OutputPort> public_outputs,
        std::span<InputPort> private_inputs,
        size_t block_size
    )
    {
        for (size_t i = 0; i < public_outputs.size(); ++i) {
            auto const source_layout = private_inputs[i].channel_layout();
            IV_ASSERT(public_outputs[i].source_layout() == source_layout, "graph egress output source layout does not match private input layout");
            Sample frame[2] {};
            for (size_t sample_i = 0; sample_i < block_size; ++sample_i) {
                for (size_t channel = 0; channel < channel_count(source_layout); ++channel) {
                    frame[channel] = private_inputs[i].get_frame(sample_i, channel);
                }
                public_outputs[i].push_frame(std::span<Sample const>(frame, channel_count(source_layout)));
            }
            advance_input(private_inputs[i], block_size);
        }
    }

    inline void push_input_events_to_private_outputs(
        std::span<EventOutputPort> private_outputs,
        std::span<EventInputPort> public_inputs,
        size_t block_index,
        size_t block_size
    )
    {
        for (size_t i = 0; i < public_inputs.size(); ++i) {
            private_outputs[i].push_block(public_inputs[i].get_block(block_index, block_size));
        }
    }

    inline void push_private_input_events_to_output_events(
        std::span<EventOutputPort> public_outputs,
        std::span<EventInputPort> private_inputs,
        size_t block_index,
        size_t block_size
    )
    {
        for (size_t i = 0; i < public_outputs.size(); ++i) {
            public_outputs[i].push_block(private_inputs[i].get_block(block_index, block_size));
        }
    }
}
