#pragma once

#include "../basic_nodes/type_erased.h"
#include "event_port_data_node.h"
#include "port_data_node.h"
#include "runtime.h"
#include "types.h"
#include "wiring.h"

#include <algorithm>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace iv {
    struct GraphNodeWrapper {
        TypeErasedNode _node;
        std::optional<size_t> _ttl_samples;
        std::string _node_id;
        std::vector<std::string> _output_targets;
        std::vector<EventOutputBinding> _event_output_targets;
        std::vector<GraphPortDataNode> _input_port_data_nodes;
        std::vector<GraphEventPortDataNode> _input_event_port_data_nodes;

        explicit GraphNodeWrapper(
            TypeErasedNode node,
            std::optional<size_t> ttl_samples,
            std::vector<PortBufferPlan> input_buffer_plans,
            std::vector<EventInputConfig> input_event_configs,
            std::string node_id,
            std::vector<std::string> output_targets,
            std::vector<EventOutputBinding> event_output_targets
        )
        : _node(std::move(node))
        , _ttl_samples(ttl_samples)
        , _node_id(std::move(node_id))
        , _output_targets(std::move(output_targets))
        , _event_output_targets(std::move(event_output_targets))
        , _input_port_data_nodes(make_input_port_data_nodes(_node_id, get_inputs(_node), input_buffer_plans))
        , _input_event_port_data_nodes(make_input_event_port_data_nodes(_node_id, input_event_configs))
        {}

        explicit GraphNodeWrapper(
            TypeErasedNode node,
            std::optional<size_t> ttl_samples,
            std::vector<PortBufferPlan> input_buffer_plans,
            std::string node_id,
            std::vector<std::string> output_targets
        ) :
            GraphNodeWrapper(
                std::move(node),
                ttl_samples,
                std::move(input_buffer_plans),
                {},
                std::move(node_id),
                std::move(output_targets),
                {}
            )
        {}

        explicit GraphNodeWrapper(
            TypeErasedNode node,
            std::vector<PortBufferPlan> input_buffer_plans,
            std::string node_id,
            std::vector<std::string> output_targets
        ) :
            GraphNodeWrapper(
                std::move(node),
                std::nullopt,
                std::move(input_buffer_plans),
                std::move(node_id),
                std::move(output_targets)
            )
        {}

        static std::vector<GraphPortDataNode> make_input_port_data_nodes(
            std::string const& node_id,
            std::span<InputConfig const> inputs,
            std::span<PortBufferPlan const> input_buffer_plans
        )
        {
            IV_ASSERT(inputs.size() == input_buffer_plans.size(), "graph input port data must have one buffer plan per input");

            std::vector<GraphPortDataNode> port_data_nodes;
            port_data_nodes.reserve(inputs.size());
            for (size_t input_i = 0; input_i < inputs.size(); ++input_i) {
                port_data_nodes.emplace_back(
                    port_data_export_id(node_id, input_i),
                    inputs[input_i],
                    input_buffer_plans[input_i]
                );
            }
            return port_data_nodes;
        }

        static std::vector<GraphEventPortDataNode> make_input_event_port_data_nodes(
            std::string const& node_id,
            std::span<EventInputConfig const> inputs
        )
        {
            std::vector<GraphEventPortDataNode> port_data_nodes;
            port_data_nodes.reserve(inputs.size());
            for (size_t input_i = 0; input_i < inputs.size(); ++input_i) {
                port_data_nodes.emplace_back(
                    event_port_data_export_id(node_id, input_i),
                    inputs[input_i]
                );
            }
            return port_data_nodes;
        }

        struct State {
            std::span<std::span<std::byte>> nested_node_states;
            std::span<InputPort> inputs;
            std::span<OutputPort> outputs;
            std::span<EventInputPort> event_inputs;
            std::span<EventOutputPort> event_outputs;
        };

        auto inputs() const
        {
            return get_inputs(_node);
        }

        auto outputs() const
        {
            return get_outputs(_node);
        }

        auto event_inputs() const
        {
            return get_event_inputs(_node);
        }

        auto event_outputs() const
        {
            return get_event_outputs(_node);
        }

        size_t internal_latency() const
        {
            return get_internal_latency(_node);
        }

        size_t max_block_size() const
        {
            return get_max_block_size(_node);
        }

        void declare(DeclarationContext<GraphNodeWrapper> const& ctx) const
        {
            auto const& state = ctx.state();
            auto const num_inputs = get_num_inputs(_node);
            auto const num_outputs = get_num_outputs(_node);
            auto const num_event_inputs = get_num_event_inputs(_node);
            auto const num_event_outputs = get_num_event_outputs(_node);

            ctx.nested_node_states(state.nested_node_states);
            ctx.local_array(state.inputs, num_inputs);
            ctx.local_array(state.outputs, num_outputs);
            ctx.local_array(state.event_inputs, num_event_inputs);
            ctx.local_array(state.event_outputs, num_event_outputs);
            for (auto const& port_data_node : _input_port_data_nodes) {
                do_declare(port_data_node, ctx);
            }
            for (auto const& port_data_node : _input_event_port_data_nodes) {
                do_declare(port_data_node, ctx);
            }
            do_declare(_node, ctx);

            for (size_t input_i = 0; input_i < num_inputs; ++input_i) {
                ctx.require_export_array<SharedPortData>(port_data_export_id(_node_id, input_i));
            }
            for (size_t input_i = 0; input_i < num_event_inputs; ++input_i) {
                ctx.require_export_array<EventSharedPortData>(event_port_data_export_id(_node_id, input_i));
            }
            for (auto const& target : _output_targets) {
                if (!target.empty()) {
                    ctx.require_export_array<SharedPortData>(target);
                }
            }
            for (auto const& target : _event_output_targets) {
                if (!target.target.empty()) {
                    ctx.require_export_array<EventSharedPortData>(target.target);
                }
            }
        }

        void initialize(InitializationContext<GraphNodeWrapper> const& ctx) const
        {
            auto& state = ctx.state();
            std::vector<InputConfig> const inputs = get_inputs(_node);
            std::vector<OutputConfig> const outputs = get_outputs(_node);
            std::vector<EventInputConfig> const event_inputs = get_event_inputs(_node);
            std::vector<EventOutputConfig> const event_outputs = get_event_outputs(_node);

            for (size_t input_i = 0; input_i < inputs.size(); ++input_i) {
                auto input_port_data = ctx.template resolve_exported_array_storage<SharedPortData>(
                    port_data_export_id(_node_id, input_i)
                );
                IV_ASSERT(!input_port_data.empty(), "graph node wrapper input wiring must resolve the requested SharedPortData entry");
                std::construct_at(&state.inputs[input_i], const_cast<SharedPortData&>(input_port_data[0]), inputs[input_i].history);
            }
            for (size_t input_i = 0; input_i < event_inputs.size(); ++input_i) {
                auto input_port_data = ctx.template resolve_exported_array_storage<EventSharedPortData>(
                    event_port_data_export_id(_node_id, input_i)
                );
                IV_ASSERT(!input_port_data.empty(), "graph node wrapper event input wiring must resolve the requested EventSharedPortData entry");
                std::construct_at(&state.event_inputs[input_i], input_port_data[0]);
            }

            for (size_t output_i = 0; output_i < outputs.size(); ++output_i) {
                auto const& target = _output_targets[output_i];
                if (target.empty()) {
                    throw std::logic_error(
                        "graph node wrapper output wiring is missing for node '" + _node_id +
                        "' output " + std::to_string(output_i) + "'"
                    );
                }
                auto target_port_data = ctx.template resolve_exported_array_storage<SharedPortData>(target);
                if (target_port_data.empty()) {
                    throw std::logic_error(
                        "graph output target wiring is unresolved for node '" + _node_id +
                        "' output " + std::to_string(output_i) +
                        " -> '" + target +
                        "', resolved size = " + std::to_string(target_port_data.size())
                    );
                }
                std::construct_at(
                    &state.outputs[output_i],
                    const_cast<SharedPortData&>(target_port_data[0]),
                    outputs[output_i].history
                );
            }
            for (size_t output_i = 0; output_i < event_outputs.size(); ++output_i) {
                auto const& target = _event_output_targets[output_i];
                if (target.target.empty()) {
                    throw std::logic_error(
                        "graph node wrapper event output wiring is missing for node '" + _node_id +
                        "' output " + std::to_string(output_i) + "'"
                    );
                }
                auto target_port_data = ctx.template resolve_exported_array_storage<EventSharedPortData>(target.target);
                if (target_port_data.empty()) {
                    throw std::logic_error(
                        "graph event output target wiring is unresolved for node '" + _node_id +
                        "' output " + std::to_string(output_i) +
                        " -> '" + target.target +
                        "', resolved size = " + std::to_string(target_port_data.size())
                    );
                }
                std::construct_at(
                    &state.event_outputs[output_i],
                    target_port_data[0],
                    event_outputs[output_i].type,
                    target.conversion
                );
            }
        }

        void tick_block(TickBlockContext<GraphNodeWrapper> const& ctx) const
        {
            auto& state = ctx.state();
            _node.tick_block({
                    TickContext<TypeErasedNode> {
                        .inputs = state.inputs,
                        .outputs = state.outputs,
                        .event_inputs = state.event_inputs,
                        .event_outputs = state.event_outputs,
                        .event_streams = ctx.event_streams,
                        .scc_feedback_latency = ctx.scc_feedback_latency,
                        .buffer = state.nested_node_states[state.nested_node_states.size() - 1],
                    },
                ctx.index,
                ctx.block_size
            });
        }

        void skip_block(SkipBlockContext<GraphNodeWrapper> const& ctx) const
        {
            auto& state = ctx.state();
            _node.skip_block({
                TickContext<TypeErasedNode> {
                    .inputs = state.inputs,
                    .outputs = state.outputs,
                    .event_inputs = state.event_inputs,
                    .event_outputs = state.event_outputs,
                    .event_streams = ctx.event_streams,
                    .scc_feedback_latency = ctx.scc_feedback_latency,
                    .buffer = state.nested_node_states[state.nested_node_states.size() - 1],
                },
                ctx.index,
                ctx.block_size
            });
        }

        size_t resolve_default_ttl_samples(size_t default_ttl) const
        {
            return _ttl_samples.value_or(_node.ttl_samples().value_or(default_ttl));
        }
    };
}
