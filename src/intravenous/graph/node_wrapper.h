#pragma once

#include <intravenous/graph/event_port_data_node.h>
#include <intravenous/graph/port_data_node.h>
#include <intravenous/graph/reflected_node.hpp>
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
    enum class GraphNodeWrapperBuildMode {
        minimal,
        full,
    };

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
        StaticSpan<size_t> _output_fanout_offsets {};
        StaticSpan<StaticSampleOutputBinding> _fanout_targets {};
        StaticSpan<StaticEventOutputBinding> _event_output_targets {};
        StaticSpan<GraphPortDataNode> _input_port_data_nodes {};
        StaticSpan<GraphPortDataNode> _output_port_data_nodes {};
        StaticSpan<GraphEventPortDataNode> _input_event_port_data_nodes {};

        consteval explicit GraphNodeWrapper(
            ReflectedNodeDescription node,
            std::optional<size_t> ttl_samples,
            std::vector<InputPortPlan> input_plans,
            std::vector<SampleInputBinding> input_bindings,
            std::vector<EventInputConfig> input_event_configs,
            std::string node_id,
            std::vector<std::vector<SampleOutputBinding>> output_targets,
            std::vector<SampleBufferStorage> output_fanout_storage,
            std::vector<EventOutputBinding> event_output_targets,
            // The minimal mode is used only by artifact profiling to isolate
            // static port/binding promotion from wrapper scaffolding.
            GraphNodeWrapperBuildMode build_mode =
                GraphNodeWrapperBuildMode::full
        )
        : _operations(node.operations.runtime)
        , _inputs(build_mode == GraphNodeWrapperBuildMode::full
              ? details::define_static_configs<StaticInputConfig>(node.inputs())
              : StaticSpan<StaticInputConfig>{})
        , _outputs(build_mode == GraphNodeWrapperBuildMode::full
              ? details::define_static_configs<StaticOutputConfig>(
                    node.outputs())
              : StaticSpan<StaticOutputConfig>{})
        , _event_outputs(build_mode == GraphNodeWrapperBuildMode::full
              ? details::define_static_configs<StaticEventOutputConfig>(
                    node.event_outputs())
              : StaticSpan<StaticEventOutputConfig>{})
        , _internal_latency(node.internal_latency())
        , _max_block_size(node.max_block_size())
        , _ttl_samples(ttl_samples.value_or(0))
        , _has_ttl_samples(ttl_samples.has_value())
        , _node_default_ttl_samples(node.ttl_samples().value_or(0))
        , _has_node_default_ttl_samples(node.ttl_samples().has_value())
        , _can_skip_block(node.can_skip_block())
        , _node_id(build_mode == GraphNodeWrapperBuildMode::full
              ? details::define_static_string(node_id)
              : StaticString{})
        , _output_targets(build_mode == GraphNodeWrapperBuildMode::full
              ? freeze_primary_output_targets(output_targets)
              : StaticSpan<StaticSampleOutputBinding>{})
        , _output_fanout_offsets(build_mode == GraphNodeWrapperBuildMode::full
              ? freeze_output_fanout_offsets(output_targets)
              : StaticSpan<size_t>{})
        , _fanout_targets(build_mode == GraphNodeWrapperBuildMode::full
              ? freeze_fanout_targets(output_targets)
              : StaticSpan<StaticSampleOutputBinding>{})
        , _event_output_targets(build_mode == GraphNodeWrapperBuildMode::full
              ? freeze_event_output_targets(event_output_targets)
              : StaticSpan<StaticEventOutputBinding>{})
        , _input_port_data_nodes(build_mode == GraphNodeWrapperBuildMode::full
              ? details::define_static_span(make_input_port_data_nodes(
                    node_id, node.inputs(), input_plans, input_bindings))
              : StaticSpan<GraphPortDataNode>{})
        , _output_port_data_nodes(build_mode == GraphNodeWrapperBuildMode::full
              ? details::define_static_span(make_output_port_data_nodes(
                    output_fanout_storage))
              : StaticSpan<GraphPortDataNode>{})
        , _input_event_port_data_nodes(
              build_mode == GraphNodeWrapperBuildMode::full
                  ? details::define_static_span(make_input_event_port_data_nodes(
                        node_id, input_event_configs))
                  : StaticSpan<GraphEventPortDataNode>{})
        {}

        // Kept for the small standalone layout test. Production compilation
        // supplies the complete source-to-many target representation above.
        consteval explicit GraphNodeWrapper(
            ReflectedNodeDescription node,
            std::vector<InputPortPlan> input_plans,
            std::string node_id,
            std::vector<SampleOutputBinding> output_targets
        ) :
            GraphNodeWrapper(
                node,
                std::nullopt,
                std::move(input_plans),
                std::vector<SampleInputBinding>(node.inputs().size()),
                {},
                std::move(node_id),
                wrap_primary_output_targets(output_targets),
                {},
                {})
        {}

        static consteval std::vector<std::vector<SampleOutputBinding>>
        wrap_primary_output_targets(
            std::span<SampleOutputBinding const> targets)
        {
            std::vector<std::vector<SampleOutputBinding>> result;
            result.reserve(targets.size());
            for (auto const& target : targets) {
                result.push_back({target});
            }
            return result;
        }

        static consteval StaticSpan<StaticSampleOutputBinding>
        freeze_primary_output_targets(
            std::span<std::vector<SampleOutputBinding> const> targets)
        {
            std::vector<StaticSampleOutputBinding> result;
            result.reserve(targets.size());
            for (auto const& target : targets) {
                auto const& primary = target.empty()
                    ? SampleOutputBinding{}
                    : target.front();
                result.push_back({
                    .target = details::define_static_string(primary.target),
                    .conversion = primary.conversion,
                });
            }
            return details::define_static_span(result);
        }

        static consteval StaticSpan<size_t> freeze_output_fanout_offsets(
            std::span<std::vector<SampleOutputBinding> const> targets)
        {
            std::vector<size_t> offsets{0};
            offsets.reserve(targets.size() + 1);
            for (auto const& target : targets) {
                offsets.push_back(offsets.back() +
                    (target.empty() ? 0 : target.size() - 1));
            }
            return details::define_static_span(offsets);
        }

        static consteval StaticSpan<StaticSampleOutputBinding>
        freeze_fanout_targets(
            std::span<std::vector<SampleOutputBinding> const> targets)
        {
            std::vector<StaticSampleOutputBinding> result;
            for (auto const& output_targets : targets) {
                for (size_t i = 1; i < output_targets.size(); ++i) {
                    result.push_back({
                        .target = details::define_static_string(
                            output_targets[i].target),
                        .conversion = output_targets[i].conversion,
                    });
                }
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
            std::span<InputPortPlan const> input_plans,
            std::span<SampleInputBinding const> input_bindings
        )
        {
            IV_ASSERT(inputs.size() == input_plans.size(), "graph input port data must have one plan per input");
            IV_ASSERT(inputs.size() == input_bindings.size(), "graph input bindings must align with input configs");

            std::vector<GraphPortDataNode> port_data_nodes;
            port_data_nodes.reserve(inputs.size());
            for (size_t input_i = 0; input_i < inputs.size(); ++input_i) {
                port_data_nodes.emplace_back(
                    port_data_export_id(node_id, input_i),
                    inputs[input_i],
                    input_plans[input_i],
                    input_bindings[input_i].aliases,
                    input_bindings[input_i].owns_storage
                );
            }
            return port_data_nodes;
        }

        static consteval std::vector<GraphPortDataNode> make_output_port_data_nodes(
            std::span<SampleBufferStorage const> storage)
        {
            std::vector<GraphPortDataNode> result;
            result.reserve(storage.size());
            for (auto const& entry : storage) {
                result.emplace_back(
                    entry.id, entry.config,
                    InputPortPlan{.storage = entry.plan});
            }
            return result;
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
            std::span<OutputPort> fanout_outputs;
            std::span<EventInputPort> event_inputs;
            std::span<EventOutputPort> event_outputs;
        };

        constexpr auto inputs() const
        {
            std::vector<InputConfig> result;
            result.reserve(_inputs.size);
            for (auto const& input : _inputs) {
                result.push_back(input.config());
            }
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
            result.reserve(_input_event_port_data_nodes.size);
            for (auto const& port_data : _input_event_port_data_nodes) {
                result.push_back(port_data._input.config());
            }
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
            auto const num_inputs = _input_port_data_nodes.size;
            auto const num_outputs = _outputs.size;
            auto const num_event_inputs = _input_event_port_data_nodes.size;
            auto const num_event_outputs = _event_outputs.size;

            ctx.nested_node_states(state.nested_node_states);
            ctx.local_array(state.inputs, num_inputs);
            ctx.local_array(state.outputs, num_outputs);
            ctx.local_array(state.fanout_outputs, _fanout_targets.size);
            ctx.local_array(state.event_inputs, num_event_inputs);
            ctx.local_array(state.event_outputs, num_event_outputs);
            for (auto const& port_data_node : _input_port_data_nodes) {
                do_declare(port_data_node, ctx);
            }
            for (auto const& port_data_node : _output_port_data_nodes) {
                do_declare(port_data_node, ctx);
            }
            for (auto const& port_data_node : _input_event_port_data_nodes) {
                do_declare(port_data_node, ctx);
            }
            ctx.declare_reflected_child(
                _operations.node_data,
                _operations.declare_node);

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
            for (auto const& target : _fanout_targets) {
                ctx.require_export_array<SharedPortData>(
                    std::string(target.target.view()));
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
                std::construct_at(
                    &state.inputs[input_i],
                    const_cast<SharedPortData&>(input_port_data[0]),
                    inputs[input_i].history,
                    _input_port_data_nodes[input_i]._input_plan.read_latency);
            }
            for (size_t input_i = 0; input_i < event_inputs.size(); ++input_i) {
                auto input_port_data = ctx.template resolve_exported_array_storage<EventSharedPortData>(
                    event_port_data_export_id(_node_id.view(), input_i)
                );
                IV_ASSERT(!input_port_data.empty(), "graph node wrapper event input wiring must resolve the requested EventSharedPortData entry");
                std::construct_at(&state.event_inputs[input_i], const_cast<EventSharedPortData&>(input_port_data[0]));
            }

            for (size_t output_i = 0; output_i < outputs.size(); ++output_i) {
                for (size_t fanout_i = _output_fanout_offsets[output_i];
                     fanout_i < _output_fanout_offsets[output_i + 1];
                     ++fanout_i) {
                    auto const& target = _fanout_targets[fanout_i];
                    auto target_port_data = ctx.template resolve_exported_array_storage<SharedPortData>(
                        std::string(target.target.view()));
                    IV_ASSERT(!target_port_data.empty(), "graph fanout target wiring must resolve");
                    std::construct_at(
                        &state.fanout_outputs[fanout_i],
                        const_cast<SharedPortData&>(target_port_data[0]),
                        outputs[output_i].history,
                        effective_channel_layout(outputs[output_i]),
                        target.conversion);
                }
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

        void propagate_sample_fanout(
            State& state, size_t block_size) const
        {
            for (size_t output_i = 0; output_i < state.outputs.size(); ++output_i) {
                for (size_t fanout_i = _output_fanout_offsets[output_i];
                     fanout_i < _output_fanout_offsets[output_i + 1];
                     ++fanout_i) {
                    state.outputs[output_i].copy_completed_block_to(
                        state.fanout_outputs[fanout_i], block_size);
                }
            }
        }

        void tick(TickBlockContext<GraphNodeWrapper> const& ctx) const
        {
            auto& state = ctx.state();
            _operations.tick_block(
                _operations.node_data,
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
            propagate_sample_fanout(state, ctx.block_size);
        }

        void skip(SkipBlockContext<GraphNodeWrapper> const& ctx) const
        {
            auto& state = ctx.state();
            _operations.skip_block(
                _operations.node_data,
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
            propagate_sample_fanout(state, ctx.block_size);
        }

        template<auto Node>
        static IV_FORCEINLINE void propagate_sample_fanout_static(
            State& state, size_t block_size)
        {
            for (size_t output_i = 0; output_i < state.outputs.size(); ++output_i) {
                for (size_t fanout_i = Node._output_fanout_offsets[output_i];
                     fanout_i < Node._output_fanout_offsets[output_i + 1];
                     ++fanout_i) {
                    state.outputs[output_i].copy_completed_block_to(
                        state.fanout_outputs[fanout_i], block_size);
                }
            }
        }

        template<auto Node>
        static IV_FORCEINLINE void tick_static(
            TickBlockContext<GraphNodeWrapper> const& ctx)
        {
            auto& state = ctx.state();
            Node._operations.tick_block(
                Node._operations.node_data,
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
            propagate_sample_fanout_static<Node>(state, ctx.block_size);
        }

        template<auto Node>
        static IV_FORCEINLINE void skip_static(
            SkipBlockContext<GraphNodeWrapper> const& ctx)
        {
            auto& state = ctx.state();
            Node._operations.skip_block(
                Node._operations.node_data,
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
            propagate_sample_fanout_static<Node>(state, ctx.block_size);
        }

        constexpr size_t resolve_default_ttl_samples(size_t default_ttl) const
        {
            if (_has_ttl_samples) return _ttl_samples;
            if (_has_node_default_ttl_samples) return _node_default_ttl_samples;
            return default_ttl;
        }
    };
}
