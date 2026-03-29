#pragma once

#include "../ports.h"

#include <algorithm>
#include <cmath>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace iv {
    inline std::string port_data_export_id(std::string_view node_id)
    {
        std::string id = "port_data:";
        id += node_id;
        return id;
    }

    inline std::string graph_port_data_export_id()
    {
        return port_data_export_id("graph");
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
            private_outputs[i].push_block(public_inputs[i].get_block(block_size));
        }
    }

    inline void push_private_inputs_to_output_blocks(
        std::span<OutputPort> public_outputs,
        std::span<InputPort> private_inputs,
        size_t block_size
    )
    {
        for (size_t i = 0; i < public_outputs.size(); ++i) {
            public_outputs[i].push_block(private_inputs[i].get_block(block_size));
            advance_input(private_inputs[i], block_size);
        }
    }
}
