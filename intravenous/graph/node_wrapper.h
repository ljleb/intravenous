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
    };

    struct GraphNodeWrapper {
        TypeErasedNode _node;
        std::vector<InputConfig> _inputs;
        std::vector<OutputConfig> _outputs;
        std::vector<PortBufferPlan> _input_buffer_plans;
        std::string _node_id;
        std::vector<GraphOutputTarget> _output_targets;
        GraphPortDataNode _port_data_node;

        explicit GraphNodeWrapper(
            TypeErasedNode node,
            std::vector<PortBufferPlan> input_buffer_plans,
            std::string node_id,
            std::vector<GraphOutputTarget> output_targets
        ) :
            _node(std::move(node)),
            _inputs(_node.inputs().begin(), _node.inputs().end()),
            _outputs(_node.outputs().begin(), _node.outputs().end()),
            _input_buffer_plans(std::move(input_buffer_plans)),
            _node_id(std::move(node_id)),
            _output_targets(std::move(output_targets)),
            _port_data_node(std::span<InputConfig const>(_inputs), std::vector<PortBufferPlan>(_input_buffer_plans.begin(), _input_buffer_plans.end()), _node_id)
        {}

        struct State {
            std::span<InputPort> inputs;
            std::span<SharedPortData> disconnected_output_port_data;
            std::span<Sample> disconnected_output_samples;
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

        std::string const& node_id() const
        {
            return _node_id;
        }

        void declare(DeclarationContext<GraphNodeWrapper> const& ctx) const
        {
            auto const& state = ctx.state();
            ctx.local_array(state.inputs, _inputs.size());
            for (size_t input_i = 0; input_i < _inputs.size(); ++input_i) {
                ctx.require_export_array<SharedPortData>(port_data_export_id(_node_id, input_i));
            }
            for (auto const& target : _output_targets) {
                if (!target.port_data_id.empty()) {
                    ctx.require_export_array<SharedPortData>(target.port_data_id);
                }
            }
            ctx.local_array(state.disconnected_output_port_data, _outputs.size());
            ctx.local_array(state.disconnected_output_samples, _outputs.size());
            ctx.local_array(state.outputs, _outputs.size());
            do_declare(_port_data_node, ctx);
            do_declare(_node, ctx);
            ctx.nested_node_states(state.nested_nodes);
        }

        void initialize(InitializationContext<GraphNodeWrapper> const& ctx) const
        {
            auto& state = ctx.state();
            for (size_t input_i = 0; input_i < _inputs.size(); ++input_i) {
                auto port_data = ctx.template resolve_exported_array_storage<SharedPortData>(
                    port_data_export_id(_node_id, input_i)
                );
                IV_ASSERT(port_data.size() == 1, "graph node wrapper input wiring must resolve exactly one SharedPortData entry");
                std::construct_at(&state.inputs[input_i], const_cast<SharedPortData&>(port_data[0]), _inputs[input_i].history);
            }

            for (size_t output_i = 0; output_i < _outputs.size(); ++output_i) {
                auto const& target = _output_targets[output_i];
                if (target.port_data_id.empty()) {
                    state.disconnected_output_samples[output_i] = Sample(0);
                    std::construct_at(
                        &state.disconnected_output_port_data[output_i],
                        std::span<Sample>(&state.disconnected_output_samples[output_i], 1),
                        0
                    );
                    std::construct_at(
                        &state.outputs[output_i],
                        state.disconnected_output_port_data[output_i],
                        _outputs[output_i].history
                    );
                    continue;
                }
                auto target_port_data = ctx.template resolve_exported_array_storage<SharedPortData>(target.port_data_id);
                if (target_port_data.size() != 1) {
                    throw std::logic_error(
                        "graph output target wiring is unresolved for node '" + _node_id +
                        "' output " + std::to_string(output_i) +
                        " -> '" + target.port_data_id +
                        "', resolved size = " + std::to_string(target_port_data.size())
                    );
                }
                std::construct_at(
                    &state.outputs[output_i],
                    const_cast<SharedPortData&>(target_port_data[0]),
                    _outputs[output_i].history
                );
            }
        }

        void tick_block(TickBlockContext<GraphNodeWrapper> const& ctx) const
        {
            auto& state = ctx.state();
            IV_ASSERT(state.nested_nodes.size() == 2, "graph node wrapper must own exactly port-data and nested-node state pointers");
            IV_ASSERT(state.nested_nodes[0] != nullptr, "graph node wrapper port-data state pointer must not be null");
            IV_ASSERT(state.nested_nodes[1] != nullptr, "graph node wrapper nested-node state pointer must not be null");
            auto* const buffer_begin = ctx.buffer.data();
            auto* const buffer_end = buffer_begin + ctx.buffer.size();
            IV_ASSERT(
                state.nested_nodes[0] >= buffer_begin && state.nested_nodes[0] <= buffer_end,
                "graph node wrapper port-data state pointer must point inside the enclosing node buffer"
            );
            IV_ASSERT(
                state.nested_nodes[1] >= buffer_begin && state.nested_nodes[1] <= buffer_end,
                "graph node wrapper nested-node state pointer must point inside the enclosing node buffer"
            );
            try {
                tick_nested_node(
                    _node,
                    state.nested_nodes[1],
                    ctx,
                    state.inputs,
                    state.outputs
                );
            } catch (std::exception const& e) {
                throw std::logic_error("graph node wrapper tick failed for node '" + _node_id + "': " + e.what());
            }
        }
    };
}
