#pragma once

#include "node_wrapper.h"

#include <algorithm>
#include <numeric>
#include <span>
#include <vector>

namespace iv {
    struct GraphSccWrapper {
        std::vector<GraphNodeWrapper> _nodes;
        size_t _block_size;
        size_t _internal_latency;

        GraphSccWrapper(
            std::vector<GraphNodeWrapper> nodes,
            size_t block_size,
            size_t internal_latency
        ) :
            _nodes(std::move(nodes)),
            _block_size(block_size),
            _internal_latency(internal_latency)
        {}

        struct State {
            std::span<std::span<std::byte>> node_states;
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
            for (auto const& node : _nodes) {
                do_declare(node, ctx);
            }
            ctx.nested_node_states(state.node_states);
        }

        void tick_block(TickBlockContext<GraphSccWrapper> const& ctx) const
        {
            auto& state = ctx.state();
            // validate_block_size(_block_size, "graph SCC block size must be a non-zero power of 2");
            // IV_ASSERT(state.node_states.size() == _nodes.size(), "SCC nested node-state count must match wrapper count");
            size_t const scc_block_size = std::min(ctx.block_size, _block_size);
            // IV_ASSERT(
            //     (ctx.block_size % scc_block_size) == 0,
            //     "graph planner must provide SCC-compatible block sizes"
            // );

            for (size_t offset = 0; offset < ctx.block_size; offset += scc_block_size) {
                for (size_t node_i = 0; node_i < _nodes.size(); ++node_i) {
                    // IV_ASSERT(state.node_states[node_i].data() != nullptr, "SCC nested node-state pointer must not be null");
                    // auto* const buffer_begin = ctx.buffer.data();
                    // auto* const buffer_end = buffer_begin + ctx.buffer.size();
                    // IV_ASSERT(
                    //     state.node_states[node_i].data() >= buffer_begin && state.node_states[node_i].data() <= buffer_end,
                    //     "SCC nested node-state pointer must point inside the enclosing SCC buffer"
                    // );
                    // try {
                        do_tick_block(_nodes[node_i], {
                            TickContext<GraphNodeWrapper> {
                                .inputs = {},
                                .outputs = {},
                                .buffer = state.node_states[node_i],
                            },
                            ctx.index + offset,
                            scc_block_size
                        });
                    // } catch (std::exception const& e) {
                    //     throw std::logic_error(
                    //         "graph SCC tick failed for node '" + _nodes[node_i].node_id() +
                    //         "' at local index " + std::to_string(node_i) + ": " + e.what()
                    //     );
                    // }
                }
            }
        }
    };
}
