#pragma once

#include "../basic_nodes/type_erased.h"
#include "port_data_node.h"
#include "runtime.h"
#include "wiring.h"

#include <algorithm>
#include <stdexcept>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace iv {
    struct GraphOutputTarget {
        std::string port_data_id;
        size_t input_port;
    };

    struct GraphNodeWrapper {
        TypeErasedNode _node;
        std::vector<InputConfig> _inputs;
        std::vector<OutputConfig> _outputs;
        std::vector<size_t> _input_sample_sizes;
        std::vector<size_t> _input_sample_offsets;
        std::string _node_id;
        std::vector<GraphOutputTarget> _output_targets;
        GraphPortDataNode _port_data_node;

        explicit GraphNodeWrapper(
            TypeErasedNode node,
            std::vector<size_t> input_sample_sizes,
            std::string node_id,
            std::vector<GraphOutputTarget> output_targets
        ) :
            _node(std::move(node)),
            _inputs(_node.inputs().begin(), _node.inputs().end()),
            _outputs(_node.outputs().begin(), _node.outputs().end()),
            _input_sample_sizes(std::move(input_sample_sizes)),
            _input_sample_offsets(make_input_sample_offsets(_input_sample_sizes)),
            _node_id(std::move(node_id)),
            _output_targets(std::move(output_targets)),
            _port_data_node(std::span<InputConfig const>(_inputs), std::vector<size_t>(_input_sample_sizes.begin(), _input_sample_sizes.end()), _node_id)
        {}

        struct State {
            std::span<InputPort> inputs;
            std::span<SharedPortData> port_data;
            std::span<OutputPort> outputs;
            std::span<std::byte*> nested_nodes;
        };

        auto inputs() const
        {
            return std::span<InputConfig const>(_inputs);
        }

        auto outputs() const
        {
            return std::span<OutputConfig const>(_outputs);
        }

        size_t internal_latency() const
        {
            return _node.internal_latency();
        }

        size_t max_block_size() const
        {
            return _node.max_block_size();
        }

        size_t output_history(size_t output_port) const
        {
            return _outputs[output_port].history;
        }

        void declare(DeclarationContext<GraphNodeWrapper> const& ctx) const
        {
            auto const& state = ctx.state();
            ctx.local_array(state.inputs, _inputs.size());
            ctx.import_array(port_data_export_id(_node_id), state.port_data);
            ctx.require_export_array<SharedPortData>(port_data_export_id(_node_id));
            for (auto const& target : _output_targets) {
                if (!target.port_data_id.empty()) {
                    ctx.require_export_array<SharedPortData>(target.port_data_id);
                }
            }
            ctx.local_array(state.outputs, _outputs.size());
            do_declare(_port_data_node, ctx);
            do_declare(_node, ctx);
            ctx.nested_node_states(state.nested_nodes);
        }

        void initialize(InitializationContext<GraphNodeWrapper> const& ctx) const
        {
            auto& state = ctx.state();
            for (size_t input_i = 0; input_i < _inputs.size(); ++input_i) {
                std::construct_at(&state.inputs[input_i], state.port_data[input_i], _inputs[input_i].history);
            }

            for (size_t output_i = 0; output_i < _outputs.size(); ++output_i) {
                auto const& target = _output_targets[output_i];
                auto target_port_data = ctx.template resolve_exported_array_storage<SharedPortData>(target.port_data_id);
                if (target.input_port >= target_port_data.size()) {
                    throw std::logic_error("graph output target wiring is unresolved");
                }
                std::construct_at(
                    &state.outputs[output_i],
                    const_cast<SharedPortData&>(target_port_data[target.input_port]),
                    _outputs[output_i].history
                );
            }
        }

        void tick_block(TickBlockContext<GraphNodeWrapper> const& ctx) const
        {
            auto& state = ctx.state();
            tick_nested_node(
                _node,
                state.nested_nodes[1],
                ctx,
                state.inputs,
                state.outputs
            );
        }
    };
}
