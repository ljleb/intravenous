#pragma once

#include <intravenous/graph/event_port_data_node.h>
#include <intravenous/graph/port_data_node.h>
#include <intravenous/graph/reflected_node.hpp>
#include <intravenous/graph/runtime.h>
#include <intravenous/graph/static_storage.hpp>
#include <intravenous/graph/types.h>
#include <intravenous/graph/wiring.h>

#include <algorithm>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace iv {
    struct StaticSampleOutputBinding {
        StaticString target {};
        ChannelConversionPlan conversion {};
    };

    struct StaticEventOutputBinding {
        StaticString target {};
        EventConversionPlan conversion {};
    };

    struct GraphNodeWrapper {
        ReflectedNodeRuntimeOperations _operations {};
        StaticSpan<StaticInputConfig> _inputs {};
        StaticSpan<StaticOutputConfig> _outputs {};
        StaticSpan<StaticEventInputConfig> _event_inputs {};
        StaticSpan<StaticEventOutputConfig> _event_outputs {};
        size_t _internal_latency = 0;
        size_t _max_block_size = MAX_BLOCK_SIZE;
        size_t _ttl_samples = 0;
        bool _has_ttl_samples = false;
        size_t _node_default_ttl_samples = 0;
        bool _has_node_default_ttl_samples = false;
        bool _can_skip_block = false;
        StaticString _node_id {};
        StaticSpan<StaticSampleOutputBinding> _output_targets {};
        StaticSpan<StaticEventOutputBinding> _event_output_targets {};
        StaticSpan<GraphPortDataNode> _input_port_data_nodes {};
        StaticSpan<GraphEventPortDataNode> _input_event_port_data_nodes {};

        consteval explicit GraphNodeWrapper(
            ReflectedNodeDescription node,
            std::optional<size_t> ttl_samples,
            std::vector<PortBufferPlan> input_buffer_plans,
            std::vector<EventInputConfig> input_event_configs,
            std::string node_id,
            std::vector<SampleOutputBinding> output_targets,
            std::vector<EventOutputBinding> event_output_targets
        )
        : _operations(node.operations.runtime)
        , _inputs(details::define_static_configs<StaticInputConfig>(node.inputs()))
        , _outputs(details::define_static_configs<StaticOutputConfig>(node.outputs()))
        , _event_inputs(details::define_static_configs<StaticEventInputConfig>(
              node.event_inputs()))
        , _event_outputs(details::define_static_configs<StaticEventOutputConfig>(
              node.event_outputs()))
        , _internal_latency(node.internal_latency())
        , _max_block_size(node.max_block_size())
        , _ttl_samples(ttl_samples.value_or(0))
        , _has_ttl_samples(ttl_samples.has_value())
        , _node_default_ttl_samples(node.ttl_samples().value_or(0))
        , _has_node_default_ttl_samples(node.ttl_samples().has_value())
        , _can_skip_block(node.can_skip_block())
        , _node_id(details::define_static_string(node_id))
        , _output_targets(freeze_output_targets(output_targets))
        , _event_output_targets(freeze_event_output_targets(
              event_output_targets))
        , _input_port_data_nodes(details::define_static_span(
              make_input_port_data_nodes(
                  node_id, node.inputs(), input_buffer_plans)))
        , _input_event_port_data_nodes(details::define_static_span(
              make_input_event_port_data_nodes(
                  node_id, input_event_configs)))
        {}

        consteval explicit GraphNodeWrapper(
            ReflectedNodeDescription node,
            std::optional<size_t> ttl_samples,
            std::vector<PortBufferPlan> input_buffer_plans,
            std::string node_id,
            std::vector<SampleOutputBinding> output_targets
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

        consteval explicit GraphNodeWrapper(
            ReflectedNodeDescription node,
            std::vector<PortBufferPlan> input_buffer_plans,
            std::string node_id,
            std::vector<SampleOutputBinding> output_targets
        ) :
            GraphNodeWrapper(
                std::move(node),
                std::nullopt,
                std::move(input_buffer_plans),
                std::move(node_id),
                std::move(output_targets)
            )
        {}

        static consteval StaticSpan<StaticSampleOutputBinding>
        freeze_output_targets(std::span<SampleOutputBinding const> targets)
        {
            std::vector<StaticSampleOutputBinding> result;
            result.reserve(targets.size());
            for (auto const& target : targets) {
                result.push_back({
                    .target = details::define_static_string(target.target),
                    .conversion = target.conversion,
                });
            }
            return details::define_static_span(result);
        }

        static consteval StaticSpan<StaticEventOutputBinding>
        freeze_event_output_targets(
            std::span<EventOutputBinding const> targets)
        {
            std::vector<StaticEventOutputBinding> result;
            result.reserve(targets.size());
            for (auto const& target : targets) {
                result.push_back({
                    .target = details::define_static_string(target.target),
                    .conversion = target.conversion,
                });
            }
            return details::define_static_span(result);
        }

        static consteval std::vector<GraphPortDataNode> make_input_port_data_nodes(
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

        static consteval std::vector<GraphEventPortDataNode> make_input_event_port_data_nodes(
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

        constexpr auto inputs() const
        {
            std::vector<InputConfig> result;
            result.reserve(_inputs.size);
            for (auto const& input : _inputs) result.push_back(input.config());
            return result;
        }

        constexpr auto outputs() const
        {
            std::vector<OutputConfig> result;
            result.reserve(_outputs.size);
            for (auto const& output : _outputs)
                result.push_back(output.config());
            return result;
        }

        constexpr auto event_inputs() const
        {
            std::vector<EventInputConfig> result;
            result.reserve(_event_inputs.size);
            for (auto const& input : _event_inputs)
                result.push_back(input.config());
            return result;
        }

        constexpr auto event_outputs() const
        {
            std::vector<EventOutputConfig> result;
            result.reserve(_event_outputs.size);
            for (auto const& output : _event_outputs)
                result.push_back(output.config());
            return result;
        }

        constexpr size_t internal_latency() const
        {
            return _internal_latency;
        }

        constexpr size_t max_block_size() const
        {
            return _max_block_size;
        }

        constexpr bool can_skip_block() const
        {
            return _can_skip_block;
        }

        void declare(DeclarationContext<GraphNodeWrapper> const& ctx) const
        {
            auto const& state = ctx.state();
            auto const num_inputs = _inputs.size;
            auto const num_outputs = _outputs.size;
            auto const num_event_inputs = _event_inputs.size;
            auto const num_event_outputs = _event_outputs.size;

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
            ctx.declare_reflected_child(_operations.declare_node);

            for (size_t input_i = 0; input_i < num_inputs; ++input_i) {
                ctx.require_export_array<SharedPortData>(
                    port_data_export_id(_node_id.view(), input_i));
            }
            for (size_t input_i = 0; input_i < num_event_inputs; ++input_i) {
                ctx.require_export_array<EventSharedPortData>(
                    event_port_data_export_id(_node_id.view(), input_i));
            }
            for (auto const& target : _output_targets) {
                if (!target.target.empty()) {
                    ctx.require_export_array<SharedPortData>(
                        std::string(target.target.view()));
                }
            }
            for (auto const& target : _event_output_targets) {
                if (!target.target.empty()) {
                    ctx.require_export_array<EventSharedPortData>(
                        std::string(target.target.view()));
                }
            }
        }

        void initialize(InitializationContext<GraphNodeWrapper> const& ctx) const
        {
            auto& state = ctx.state();
            auto const inputs = this->inputs();
            auto const outputs = this->outputs();
            auto const event_inputs = this->event_inputs();
            auto const event_outputs = this->event_outputs();
            auto const node_id = std::string(_node_id.view());

            for (size_t input_i = 0; input_i < inputs.size(); ++input_i) {
                auto input_port_data = ctx.template resolve_exported_array_storage<SharedPortData>(
                    port_data_export_id(_node_id.view(), input_i)
                );
                IV_ASSERT(!input_port_data.empty(), "graph node wrapper input wiring must resolve the requested SharedPortData entry");
                std::construct_at(&state.inputs[input_i], const_cast<SharedPortData&>(input_port_data[0]), inputs[input_i].history);
            }
            for (size_t input_i = 0; input_i < event_inputs.size(); ++input_i) {
                auto input_port_data = ctx.template resolve_exported_array_storage<EventSharedPortData>(
                    event_port_data_export_id(_node_id.view(), input_i)
                );
                IV_ASSERT(!input_port_data.empty(), "graph node wrapper event input wiring must resolve the requested EventSharedPortData entry");
                std::construct_at(&state.event_inputs[input_i], const_cast<EventSharedPortData&>(input_port_data[0]));
            }

            for (size_t output_i = 0; output_i < outputs.size(); ++output_i) {
                auto const& target = _output_targets[output_i];
                if (target.target.empty()) {
                    throw std::logic_error(
                        "graph node wrapper output wiring is missing for node '" + node_id +
                        "' output " + std::to_string(output_i) + "'"
                    );
                }
                auto const target_id = std::string(target.target.view());
                auto target_port_data = ctx.template resolve_exported_array_storage<SharedPortData>(target_id);
                if (target_port_data.empty()) {
                    throw std::logic_error(
                        "graph output target wiring is unresolved for node '" + node_id +
                        "' output " + std::to_string(output_i) +
                        " -> '" + target_id +
                        "', resolved size = " + std::to_string(target_port_data.size())
                    );
                }
                std::construct_at(
                    &state.outputs[output_i],
                    const_cast<SharedPortData&>(target_port_data[0]),
                    outputs[output_i].history,
                    effective_channel_layout(outputs[output_i]),
                    target.conversion
                );
            }
            for (size_t output_i = 0; output_i < event_outputs.size(); ++output_i) {
                auto const& target = _event_output_targets[output_i];
                if (target.target.empty()) {
                    std::construct_at(&state.event_outputs[output_i]);
                    continue;
                }
                auto const target_id = std::string(target.target.view());
                auto target_port_data = ctx.template resolve_exported_array_storage<EventSharedPortData>(target_id);
                if (target_port_data.empty()) {
                    throw std::logic_error(
                        "graph event output target wiring is unresolved for node '" + node_id +
                        "' output " + std::to_string(output_i) +
                        " -> '" + target_id +
                        "', resolved size = " + std::to_string(target_port_data.size())
                    );
                }
                std::construct_at(
                    &state.event_outputs[output_i],
                    const_cast<EventSharedPortData&>(target_port_data[0]),
                    event_outputs[output_i].type,
                    target.conversion
                );
            }
        }

        template<auto Node>
        static IV_FORCEINLINE void tick_static(
            TickBlockContext<GraphNodeWrapper> const& ctx)
        {
            auto& state = ctx.state();
            Node._operations.tick_block(
                ReflectedNodeTickContext {
                    .inputs = state.inputs,
                    .outputs = state.outputs,
                    .event_inputs = state.event_inputs,
                    .event_outputs = state.event_outputs,
                    .sample_rate = ctx.sample_rate,
                    .scc_feedback_latency = ctx.scc_feedback_latency,
                    .state = state.nested_node_states.back(),
                },
                ctx.index,
                ctx.block_size
            );
        }

        template<auto Node>
        static IV_FORCEINLINE void skip_static(
            SkipBlockContext<GraphNodeWrapper> const& ctx)
        {
            auto& state = ctx.state();
            Node._operations.skip_block(
                ReflectedNodeTickContext {
                    .inputs = state.inputs,
                    .outputs = state.outputs,
                    .event_inputs = state.event_inputs,
                    .event_outputs = state.event_outputs,
                    .sample_rate = ctx.sample_rate,
                    .scc_feedback_latency = ctx.scc_feedback_latency,
                    .state = state.nested_node_states.back(),
                },
                ctx.index,
                ctx.block_size
            );
        }

        void tick_block(TickBlockContext<GraphNodeWrapper> const& ctx) const
        {
            auto& state = ctx.state();
            _operations.tick_block(
                ReflectedNodeTickContext {
                    .inputs = state.inputs,
                    .outputs = state.outputs,
                    .event_inputs = state.event_inputs,
                    .event_outputs = state.event_outputs,
                    .sample_rate = ctx.sample_rate,
                    .scc_feedback_latency = ctx.scc_feedback_latency,
                    .state = state.nested_node_states.back(),
                },
                ctx.index,
                ctx.block_size
            );
        }

        void skip_block(SkipBlockContext<GraphNodeWrapper> const& ctx) const
        {
            auto& state = ctx.state();
            _operations.skip_block(
                ReflectedNodeTickContext {
                    .inputs = state.inputs,
                    .outputs = state.outputs,
                    .event_inputs = state.event_inputs,
                    .event_outputs = state.event_outputs,
                    .sample_rate = ctx.sample_rate,
                    .scc_feedback_latency = ctx.scc_feedback_latency,
                    .state = state.nested_node_states.back(),
                },
                ctx.index,
                ctx.block_size
            );
        }

        constexpr size_t resolve_default_ttl_samples(size_t default_ttl) const
        {
            if (_has_ttl_samples) return _ttl_samples;
            if (_has_node_default_ttl_samples) return _node_default_ttl_samples;
            return default_ttl;
        }
    };
}
