#pragma once

#include "node_wrapper.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <numeric>
#include <span>
#include <stdexcept>
#include <vector>

namespace iv {
    struct GraphSccWrapper {
        std::vector<GraphNodeWrapper> _nodes;
        std::vector<size_t> _input_constant_offsets;
        size_t _block_size;
        size_t _internal_latency;
        size_t _scc_feedback_latency;

        GraphSccWrapper(
            std::vector<GraphNodeWrapper> nodes,
            size_t block_size,
            size_t internal_latency,
            size_t scc_feedback_latency
        ) :
            _nodes(std::move(nodes)),
            _input_constant_offsets(_nodes.size() + 1, 0),
            _block_size(block_size),
            _internal_latency(internal_latency),
            _scc_feedback_latency(scc_feedback_latency)
        {
            for (size_t node_i = 0; node_i < _nodes.size(); ++node_i) {
                _input_constant_offsets[node_i + 1] = _input_constant_offsets[node_i] + _nodes[node_i].inputs().size();
            }
        }

        struct DormancyState {
            std::span<std::uint8_t> dormant;
            std::span<std::uint8_t> unchanged_inputs;
            std::span<size_t> silent_samples_accumulated;
            std::span<size_t> effective_ttl_samples;
            std::span<Sample> remembered_constant_inputs;
            std::span<std::uint8_t> remembered_constant_valid;
        };

        struct State {
            std::span<std::span<std::byte>> nested_node_states;
            DormancyState dormancy;
        };

        size_t num_nodes() const
        {
            return _nodes.size();
        }

        size_t max_block_size() const
        {
            return _block_size;
        }

        size_t internal_latency() const
        {
            return _internal_latency;
        }

        void declare(DeclarationContext<GraphSccWrapper> const& ctx) const
        {
            auto const& state = ctx.state();
            ctx.nested_node_states(state.nested_node_states);
            ctx.local_array(state.dormancy.dormant, _nodes.size());
            ctx.local_array(state.dormancy.unchanged_inputs, _nodes.size());
            ctx.local_array(state.dormancy.silent_samples_accumulated, _nodes.size());
            ctx.local_array(state.dormancy.effective_ttl_samples, _nodes.size());
            ctx.local_array(state.dormancy.remembered_constant_inputs, _input_constant_offsets.back());
            ctx.local_array(state.dormancy.remembered_constant_valid, _input_constant_offsets.back());
            for (auto const& node : _nodes) {
                do_declare(node, ctx);
            }
        }

        void initialize(InitializationContext<GraphSccWrapper> const& ctx) const
        {
            auto& state = ctx.state();
            std::fill(state.dormancy.dormant.begin(), state.dormancy.dormant.end(), 0);
            std::fill(state.dormancy.unchanged_inputs.begin(), state.dormancy.unchanged_inputs.end(), 0);
            std::fill(state.dormancy.silent_samples_accumulated.begin(), state.dormancy.silent_samples_accumulated.end(), 0);
            for (size_t node_i = 0; node_i < _nodes.size(); ++node_i) {
                state.dormancy.effective_ttl_samples[node_i] =
                    _nodes[node_i].resolve_default_ttl_samples(ctx.default_silence_ttl_samples());
            }
            std::fill(state.dormancy.remembered_constant_inputs.begin(), state.dormancy.remembered_constant_inputs.end(), 0.0f);
            std::fill(state.dormancy.remembered_constant_valid.begin(), state.dormancy.remembered_constant_valid.end(), 0);
        }

        static GraphNodeWrapper::State& node_state(std::span<std::byte> buffer)
        {
            return TickContext<GraphNodeWrapper> {
                .buffer = buffer,
            }.state();
        }

        template<typename A>
        static bool block_is_constant(BlockView<A> block, Sample value)
        {
            if (block.empty()) {
                return true;
            }
            if (block[block.size() - 1] != value) {
                return false;
            }
            for (Sample sample : block) {
                if (sample != value) {
                    return false;
                }
            }
            return true;
        }

        template<typename A>
        static bool block_is_silent(BlockView<A> block)
        {
            return block_is_constant(block, 0.0f);
        }

        bool sample_inputs_unchanged(
            State& state,
            GraphNodeWrapper::State const& node_state,
            size_t node_i,
            size_t block_size,
            bool& inputs_constant
        ) const
        {
            size_t const begin = _input_constant_offsets[node_i];
            bool unchanged = true;
            inputs_constant = true;
            for (size_t input_i = 0; input_i < node_state.inputs.size(); ++input_i) {
                size_t const flat_i = begin + input_i;
                auto block = node_state.inputs[input_i].get_block(block_size);
                if (block.empty()) {
                    state.dormancy.remembered_constant_valid[flat_i] = 0;
                    unchanged = false;
                    inputs_constant = false;
                    continue;
                }

                Sample const first = block[0];
                bool const matches_previous =
                    state.dormancy.remembered_constant_valid[flat_i] != 0
                    && state.dormancy.remembered_constant_inputs[flat_i] == first;
                bool const constant = block_is_constant(block, first);

                state.dormancy.remembered_constant_valid[flat_i] = constant ? 1 : 0;
                if (constant) {
                    state.dormancy.remembered_constant_inputs[flat_i] = first;
                }

                unchanged = unchanged && matches_previous && constant;
                inputs_constant = inputs_constant && constant;
            }
            return unchanged;
        }

        static bool event_inputs_unchanged(GraphNodeWrapper::State const& node_state, EventStreamStorage* storage, size_t block_index, size_t block_size)
        {
            if (node_state.event_inputs.empty()) {
                return true;
            }
            if (!storage) {
                throw std::logic_error("event stream storage is unavailable");
            }
            for (auto const& input : node_state.event_inputs) {
                if (input.get_block(*storage, block_index, block_size).size() != 0) {
                    return false;
                }
            }
            return true;
        }

        static bool sample_outputs_silent(GraphNodeWrapper::State const& node_state, size_t block_size)
        {
            for (auto const& output : node_state.outputs) {
                if (!block_is_silent(output.current_block(block_size))) {
                    return false;
                }
            }
            return true;
        }

        static bool event_outputs_empty(GraphNodeWrapper::State const& node_state, EventStreamStorage* storage, size_t block_index, size_t block_size)
        {
            if (node_state.event_outputs.empty()) {
                return true;
            }
            if (!storage) {
                throw std::logic_error("event stream storage is unavailable");
            }
            for (auto const& output : node_state.event_outputs) {
                if (!output.empty_in_block(*storage, block_index, block_size)) {
                    return false;
                }
            }
            return true;
        }

        void tick_block(TickBlockContext<GraphSccWrapper> const& ctx) const
        {
            auto& state = ctx.state();
            size_t const scc_block_size = std::min(ctx.block_size, _block_size);
            EventStreamStorage* const event_storage = ctx.event_streams;

            for (size_t offset = 0; offset < ctx.block_size; offset += scc_block_size) {
                for (size_t node_i = 0; node_i < _nodes.size(); ++node_i) {
                    auto node_ctx = TickContext<GraphNodeWrapper> {
                        .inputs = {},
                        .outputs = {},
                        .event_inputs = {},
                        .event_outputs = {},
                        .event_streams = ctx.event_streams,
                        .scc_feedback_latency = _scc_feedback_latency,
                        .buffer = state.nested_node_states[node_i],
                    };
                    auto& runtime_state = node_state(state.nested_node_states[node_i]);
                    size_t const block_index = ctx.index + offset;
                    bool inputs_constant = false;
                    bool const unchanged =
                        sample_inputs_unchanged(state, runtime_state, node_i, scc_block_size, inputs_constant)
                        && event_inputs_unchanged(runtime_state, event_storage, block_index, scc_block_size);
                    state.dormancy.unchanged_inputs[node_i] = unchanged ? 1 : 0;

                    if (state.dormancy.dormant[node_i] != 0) {
                        if (unchanged) {
                            do_skip_block(_nodes[node_i], {
                                node_ctx,
                                block_index,
                                scc_block_size
                            });
                            continue;
                        }

                        state.dormancy.dormant[node_i] = 0;
                        state.dormancy.silent_samples_accumulated[node_i] = 0;
                    }

                    do_tick_block(_nodes[node_i], {
                        node_ctx,
                        block_index,
                        scc_block_size
                    });

                    size_t const ttl_samples = state.dormancy.effective_ttl_samples[node_i];
                    if (ttl_samples == std::numeric_limits<size_t>::max()) {
                        state.dormancy.silent_samples_accumulated[node_i] = 0;
                        continue;
                    }

                    bool const silent =
                        sample_outputs_silent(runtime_state, scc_block_size)
                        && event_outputs_empty(runtime_state, event_storage, block_index, scc_block_size);

                    if (inputs_constant && silent) {
                        size_t const accumulated = state.dormancy.silent_samples_accumulated[node_i] + scc_block_size;
                        state.dormancy.silent_samples_accumulated[node_i] = accumulated;
                        if (accumulated >= ttl_samples) {
                            state.dormancy.dormant[node_i] = 1;
                        }
                    } else {
                        state.dormancy.silent_samples_accumulated[node_i] = 0;
                    }
                }
            }
        }

    };
}
