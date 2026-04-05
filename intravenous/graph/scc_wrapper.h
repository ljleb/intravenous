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
            std::span<std::span<std::byte>> nested_node_states;
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
            for (auto const& node : _nodes) {
                do_declare(node, ctx);
            }
        }

        void tick_block(TickBlockContext<GraphSccWrapper> const& ctx) const
        {
            auto& state = ctx.state();
            size_t const scc_block_size = std::min(ctx.block_size, _block_size);

            for (size_t offset = 0; offset < ctx.block_size; offset += scc_block_size) {
                for (size_t node_i = 0; node_i < _nodes.size(); ++node_i) {
                    do_tick_block(_nodes[node_i], {
                        TickContext<GraphNodeWrapper> {
                            .inputs = {},
                            .outputs = {},
                            .event_inputs = {},
                            .event_outputs = {},
                            .buffer = state.nested_node_states[node_i],
                        },
                        ctx.index + offset,
                        scc_block_size
                    });
                }
            }
        }
    };
}
