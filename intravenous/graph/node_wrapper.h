#pragma once

#include "../basic_nodes/type_erased.h"
#include "port_data_node.h"
#include "runtime.h"
#include "wiring.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <memory>
#include <sstream>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace iv {
    struct GraphOutputTarget {
        std::string port_data_id;
        size_t port_index = 0;
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
            std::span<std::span<std::byte>> nested_nodes;
            std::span<InputPort> inputs;
            std::span<OutputPort> outputs;
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
            ctx.nested_node_states(state.nested_nodes);
            do_declare(_port_data_node, ctx);
            ctx.local_array(state.inputs, _inputs.size());
            do_declare(_node, ctx);
            ctx.local_array(state.outputs, _outputs.size());

            if (!_inputs.empty()) {
                ctx.require_export_array<SharedPortData>(port_data_export_id(_node_id));
            }
            for (auto const& target : _output_targets) {
                if (!target.port_data_id.empty()) {
                    ctx.require_export_array<SharedPortData>(target.port_data_id);
                }
            }
        }

        void initialize(InitializationContext<GraphNodeWrapper> const& ctx) const
        {
            auto& state = ctx.state();
            auto input_port_data = _inputs.empty()
                ? std::span<SharedPortData const>()
                : ctx.template resolve_exported_array_storage<SharedPortData>(port_data_export_id(_node_id));
            for (size_t input_i = 0; input_i < _inputs.size(); ++input_i) {
                IV_ASSERT(input_i < input_port_data.size(), "graph node wrapper input wiring must resolve the requested SharedPortData entry");
                std::construct_at(&state.inputs[input_i], const_cast<SharedPortData&>(input_port_data[input_i]), _inputs[input_i].history);
            }

            for (size_t output_i = 0; output_i < _outputs.size(); ++output_i) {
                auto const& target = _output_targets[output_i];
                if (target.port_data_id.empty()) {
                    throw std::logic_error(
                        "graph node wrapper output wiring is missing for node '" + _node_id +
                        "' output " + std::to_string(output_i) + "'"
                    );
                }
                auto target_port_data = ctx.template resolve_exported_array_storage<SharedPortData>(target.port_data_id);
                if (target.port_index >= target_port_data.size()) {
                    throw std::logic_error(
                        "graph output target wiring is unresolved for node '" + _node_id +
                        "' output " + std::to_string(output_i) +
                        " -> '" + target.port_data_id +
                        "'[" + std::to_string(target.port_index) +
                        "], resolved size = " + std::to_string(target_port_data.size())
                    );
                }
                std::construct_at(
                    &state.outputs[output_i],
                    const_cast<SharedPortData&>(target_port_data[target.port_index]),
                    _outputs[output_i].history
                );
            }
        }

        void tick_block(TickBlockContext<GraphNodeWrapper> const& ctx) const
        {
            auto& state = ctx.state();
            // IV_ASSERT(state.nested_nodes.size() == 2, "graph node wrapper must own exactly port-data and nested-node state pointers");
            // IV_ASSERT(state.nested_nodes[0].data() != nullptr, "graph node wrapper port-data state pointer must not be null");
            // IV_ASSERT(state.nested_nodes[1].data() != nullptr, "graph node wrapper nested-node state pointer must not be null");
            // auto* const buffer_begin = ctx.buffer.data();
            // auto* const buffer_end = buffer_begin + ctx.buffer.size();
            // IV_ASSERT(
            //     state.nested_nodes[0].data() >= buffer_begin && state.nested_nodes[0].data() <= buffer_end,
            //     "graph node wrapper port-data state pointer must point inside the enclosing node buffer"
            // );
            // IV_ASSERT(
            //     state.nested_nodes[1].data() >= buffer_begin && state.nested_nodes[1].data() <= buffer_end,
            //     "graph node wrapper nested-node state pointer must point inside the enclosing node buffer"
            // );
            // auto const start_time = std::chrono::steady_clock::now();
            // try {
                // trace_block_inputs(ctx, state.inputs);
                _node.tick_block({
                    TickContext<TypeErasedNode> {
                        .inputs = state.inputs,
                        .outputs = state.outputs,
                        .buffer = state.nested_nodes[1],
                    },
                    ctx.index,
                    ctx.block_size
                });
                // trace_block_outputs(ctx, state.outputs);
            // } catch (std::exception const& e) {
            //     throw std::logic_error("graph node wrapper tick failed for node '" + _node_id + "': " + e.what());
            // }
            // maybe_log_timing(ctx, std::chrono::steady_clock::now() - start_time);
        }

    private:
        void trace_block_outputs(TickBlockContext<GraphNodeWrapper> const& ctx, std::span<OutputPort> outputs) const
        {
            if (!sample_trace_enabled()) {
                return;
            }
            constexpr std::string_view prefix = "trace.node.samples";
            if (!sample_trace_matches(prefix) && !sample_trace_matches(_node_id) && !sample_trace_matches(_node.type_name())) {
                return;
            }

            std::ostringstream oss;
            oss << prefix << ": id=" << _node_id
                << " type=" << _node.type_name()
                << " index=" << ctx.index
                << " size=" << ctx.block_size;

            for (size_t output_i = 0; output_i < outputs.size(); ++output_i) {
                auto block = outputs[output_i].get_block(ctx.block_size);
                Sample max_abs = 0.0f;
                for (Sample sample : block) {
                    max_abs = std::max(max_abs, Sample(std::abs(sample)));
                }

                oss << " out" << output_i << "=[";
                size_t emitted = 0;
                for (Sample sample : block) {
                    if (emitted == 4) {
                        break;
                    }
                    if (emitted != 0) {
                        oss << ", ";
                    }
                    oss << sample;
                    ++emitted;
                }
                if (block.size() > 4) {
                    oss << ", ...";
                }
                oss << "] max=" << max_abs;
            }

            debug_log(oss.str());
        }

        void trace_block_inputs(TickBlockContext<GraphNodeWrapper> const& ctx, std::span<InputPort> inputs) const
        {
            if (!sample_trace_enabled()) {
                return;
            }
            constexpr std::string_view prefix = "trace.node.inputs";
            if (!sample_trace_matches(prefix) && !sample_trace_matches(_node_id) && !sample_trace_matches(_node.type_name())) {
                return;
            }

            std::ostringstream oss;
            oss << prefix << ": id=" << _node_id
                << " type=" << _node.type_name()
                << " index=" << ctx.index
                << " size=" << ctx.block_size;

            for (size_t input_i = 0; input_i < inputs.size(); ++input_i) {
                auto block = inputs[input_i].get_block(ctx.block_size);
                Sample max_abs = 0.0f;
                for (Sample sample : block) {
                    max_abs = std::max(max_abs, Sample(std::abs(sample)));
                }

                oss << " in" << input_i << "=[";
                size_t emitted = 0;
                for (Sample sample : block) {
                    if (emitted == 4) {
                        break;
                    }
                    if (emitted != 0) {
                        oss << ", ";
                    }
                    oss << sample;
                    ++emitted;
                }
                if (block.size() > 4) {
                    oss << ", ...";
                }
                oss << "] max=" << max_abs;
            }

            debug_log(oss.str());
        }

        void maybe_log_timing(TickBlockContext<GraphNodeWrapper> const& ctx, std::chrono::steady_clock::duration duration) const
        {
            std::ostringstream oss;
            oss << "trace.node.timing: id=" << _node_id
                << " type=" << _node.type_name()
                << " index=" << ctx.index
                << " size=" << ctx.block_size;
            iv::maybe_log_node_timing(oss.str(), duration);
        }
    };
}
