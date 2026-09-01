#pragma once

#include <intravenous/graph/event_port_data_node.h>
#include <intravenous/graph/port_data_node.h>
#include <intravenous/graph/reflected_node.hpp>
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

    // A lowered wrapper needs only the output attributes consumed while it
    // constructs runtime OutputPorts.  Name and latency were needed by graph
    // compilation, but are not needed by execution.
    struct GraphOutputPortConfig {
        ChannelLayout channel_layout {
            .channel_type = ChannelTypeId::mono,
            .sample_layout = SampleStreamLayout::planar,
        };
        size_t history = 0;
    };

    struct GraphNodeWrapper {
        ReflectedNodeRuntimeOperations _operations {};
        std::vector<GraphOutputPortConfig> _outputs {};
        std::vector<EventTypeId> _event_output_types {};
        size_t _internal_latency = 0;
        size_t _max_block_size = MAX_BLOCK_SIZE;
        size_t _ttl_samples = 0;
        bool _has_ttl_samples = false;
        size_t _node_default_ttl_samples = 0;
        bool _has_node_default_ttl_samples = false;
        bool _can_skip_block = false;
        std::string _node_id {};
        std::vector<SampleOutputBinding> _output_targets {};
        std::vector<size_t> _output_fanout_offsets {};
        std::vector<SampleOutputBinding> _fanout_targets {};
        std::vector<EventOutputBinding> _event_output_targets {};
        std::vector<GraphPortDataNode> _input_port_data_nodes {};
        std::vector<GraphPortDataNode> _output_port_data_nodes {};
        std::vector<GraphEventPortDataNode> _input_event_port_data_nodes {};

        explicit GraphNodeWrapper(
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
        , _outputs(build_mode == GraphNodeWrapperBuildMode::full
              ? make_output_port_configs(node.outputs())
              : std::vector<GraphOutputPortConfig>{})
        , _event_output_types(build_mode == GraphNodeWrapperBuildMode::full
              ? make_event_output_types(node.event_outputs())
              : std::vector<EventTypeId>{})
        , _internal_latency(node.internal_latency())
        , _max_block_size(node.max_block_size())
        , _ttl_samples(ttl_samples.value_or(0))
        , _has_ttl_samples(ttl_samples.has_value())
        , _node_default_ttl_samples(node.ttl_samples().value_or(0))
        , _has_node_default_ttl_samples(node.ttl_samples().has_value())
        , _can_skip_block(node.can_skip_block())
        , _node_id(build_mode == GraphNodeWrapperBuildMode::full
              ? node_id
              : std::string{})
        , _output_targets(build_mode == GraphNodeWrapperBuildMode::full
              ? primary_output_targets(output_targets)
              : std::vector<SampleOutputBinding>{})
        , _output_fanout_offsets(build_mode == GraphNodeWrapperBuildMode::full
              ? output_fanout_offsets(output_targets)
              : std::vector<size_t>{})
        , _fanout_targets(build_mode == GraphNodeWrapperBuildMode::full
              ? fanout_targets(output_targets)
              : std::vector<SampleOutputBinding>{})
        , _event_output_targets(build_mode == GraphNodeWrapperBuildMode::full
              ? std::move(event_output_targets)
              : std::vector<EventOutputBinding>{})
        , _input_port_data_nodes(build_mode == GraphNodeWrapperBuildMode::full
              ? make_input_port_data_nodes(
                    node_id, node.inputs(), input_plans, input_bindings)
              : std::vector<GraphPortDataNode>{})
        , _output_port_data_nodes(build_mode == GraphNodeWrapperBuildMode::full
              ? make_output_port_data_nodes(output_fanout_storage)
              : std::vector<GraphPortDataNode>{})
        , _input_event_port_data_nodes(
              build_mode == GraphNodeWrapperBuildMode::full
                  ? make_input_event_port_data_nodes(
                        node_id, input_event_configs)
                  : std::vector<GraphEventPortDataNode>{})
        {}

        // Kept for the small standalone layout test. Production compilation
        // supplies the complete source-to-many target representation above.
        explicit GraphNodeWrapper(
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

        static constexpr std::vector<std::vector<SampleOutputBinding>>
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

        static constexpr std::vector<GraphOutputPortConfig>
        make_output_port_configs(std::span<OutputConfig const> outputs)
        {
            std::vector<GraphOutputPortConfig> result;
            result.reserve(outputs.size());
            for (auto const& output : outputs) {
                result.push_back({
                    .channel_layout = output.channel_layout,
                    .history = output.history,
                });
            }
            return result;
        }

        static constexpr std::vector<EventTypeId>
        make_event_output_types(std::span<EventOutputConfig const> outputs)
        {
            std::vector<EventTypeId> result;
            result.reserve(outputs.size());
            for (auto const& output : outputs) {
                result.push_back(output.type);
            }
            return result;
        }

        static constexpr std::vector<SampleOutputBinding>
        primary_output_targets(
            std::span<std::vector<SampleOutputBinding> const> targets)
        {
            std::vector<SampleOutputBinding> result;
            result.reserve(targets.size());
            for (auto const& target : targets) {
                auto const& primary = target.empty()
                    ? SampleOutputBinding{}
                    : target.front();
                result.push_back(primary);
            }
            return result;
        }

        static constexpr std::vector<size_t> output_fanout_offsets(
            std::span<std::vector<SampleOutputBinding> const> targets)
        {
            std::vector<size_t> offsets{0};
            offsets.reserve(targets.size() + 1);
            for (auto const& target : targets) {
                offsets.push_back(offsets.back() +
                    (target.empty() ? 0 : target.size() - 1));
            }
            return offsets;
        }

        static constexpr std::vector<SampleOutputBinding>
        fanout_targets(
            std::span<std::vector<SampleOutputBinding> const> targets)
        {
            std::vector<SampleOutputBinding> result;
            for (auto const& output_targets : targets) {
                for (size_t i = 1; i < output_targets.size(); ++i) {
                    result.push_back(output_targets[i]);
                }
            }
            return result;
        }

        static constexpr std::vector<GraphPortDataNode> make_input_port_data_nodes(
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
                auto const& input = inputs[input_i];
                port_data_nodes.emplace_back(
                    port_data_export_id(node_id, input_i),
                    GraphPortStorageConfig {
                        .channel_layout = input.channel_layout,
                        .history = input.history,
                        .default_value = input_bindings[input_i].static_value
                            .value_or(input.default_value),
                    },
                    input_plans[input_i],
                    input_bindings[input_i].aliases,
                    input_bindings[input_i].owns_storage,
                    input_bindings[input_i].static_value.has_value()
                );
            }
            return port_data_nodes;
        }

        static constexpr std::vector<GraphPortDataNode> make_output_port_data_nodes(
            std::span<SampleBufferStorage const> storage)
        {
            std::vector<GraphPortDataNode> result;
            result.reserve(storage.size());
            for (auto const& entry : storage) {
                result.emplace_back(
                    entry.id,
                    GraphPortStorageConfig {
                        .channel_layout = entry.config.channel_layout,
                        .history = entry.config.history,
                        .default_value = entry.config.default_value,
                    },
                    InputPortPlan{.storage = entry.plan});
            }
            return result;
        }

        static constexpr std::vector<GraphEventPortDataNode> make_input_event_port_data_nodes(
            std::string const& node_id,
            std::span<EventInputConfig const> inputs
        )
        {
            std::vector<GraphEventPortDataNode> port_data_nodes;
            port_data_nodes.reserve(inputs.size());
            for (size_t input_i = 0; input_i < inputs.size(); ++input_i) {
                port_data_nodes.emplace_back(
                    event_port_data_export_id(node_id, input_i),
                    inputs[input_i].type
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
            auto const num_inputs = _input_port_data_nodes.size();
            auto const num_outputs = _outputs.size();
            auto const num_event_inputs = _input_event_port_data_nodes.size();
            auto const num_event_outputs = _event_output_types.size();

            ctx.nested_node_states(state.nested_node_states);
            ctx.local_array(state.inputs, num_inputs);
            ctx.local_array(state.outputs, num_outputs);
            ctx.local_array(state.fanout_outputs, _fanout_targets.size());
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
                    std::string(
                        _input_port_data_nodes[input_i]._port_data_id));
            }
            for (size_t input_i = 0; input_i < num_event_inputs; ++input_i) {
                ctx.require_export_array<EventSharedPortData>(
                    std::string(
                        _input_event_port_data_nodes[input_i]
                            ._port_data_id));
            }
            for (auto const& target : _output_targets) {
                if (!target.target.empty()) {
                    ctx.require_export_array<SharedPortData>(
                        target.target);
                }
            }
            for (auto const& target : _fanout_targets) {
                ctx.require_export_array<SharedPortData>(
                    target.target);
            }
            for (auto const& target : _event_output_targets) {
                if (!target.target.empty()) {
                    ctx.require_export_array<EventSharedPortData>(
                        target.target);
                }
            }
        }

        void initialize(InitializationContext<GraphNodeWrapper> const& ctx) const
        {
            auto& state = ctx.state();
            auto const node_id = _node_id;

            for (size_t input_i = 0;
                 input_i < _input_port_data_nodes.size();
                 ++input_i) {
                auto input_port_data = ctx.template resolve_exported_array_storage<SharedPortData>(
                    std::string(_input_port_data_nodes[input_i]._port_data_id)
                );
                IV_ASSERT(!input_port_data.empty(), "graph node wrapper input wiring must resolve the requested SharedPortData entry");
                std::construct_at(
                    &state.inputs[input_i],
                    const_cast<SharedPortData&>(input_port_data[0]),
                    _input_port_data_nodes[input_i]._storage.history,
                    _input_port_data_nodes[input_i]._input_plan.read_latency);
            }
            for (size_t input_i = 0;
                 input_i < _input_event_port_data_nodes.size();
                 ++input_i) {
                auto input_port_data = ctx.template resolve_exported_array_storage<EventSharedPortData>(
                    std::string(
                        _input_event_port_data_nodes[input_i]
                            ._port_data_id)
                );
                IV_ASSERT(!input_port_data.empty(), "graph node wrapper event input wiring must resolve the requested EventSharedPortData entry");
                std::construct_at(&state.event_inputs[input_i], const_cast<EventSharedPortData&>(input_port_data[0]));
            }

            for (size_t output_i = 0; output_i < _outputs.size(); ++output_i) {
                for (size_t fanout_i = _output_fanout_offsets[output_i];
                     fanout_i < _output_fanout_offsets[output_i + 1];
                     ++fanout_i) {
                    auto const& target = _fanout_targets[fanout_i];
                    auto target_port_data = ctx.template resolve_exported_array_storage<SharedPortData>(
                        target.target);
                    IV_ASSERT(!target_port_data.empty(), "graph fanout target wiring must resolve");
                    std::construct_at(
                        &state.fanout_outputs[fanout_i],
                        const_cast<SharedPortData&>(target_port_data[0]),
                        _outputs[output_i].history,
                        _outputs[output_i].channel_layout,
                        target.conversion);
                }
                auto const& target = _output_targets[output_i];
                if (target.target.empty()) {
                    throw std::logic_error(
                        "graph node wrapper output wiring is missing for node '" + node_id +
                        "' output " + std::to_string(output_i) + "'"
                    );
                }
                auto const target_id = target.target;
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
                    _outputs[output_i].history,
                    _outputs[output_i].channel_layout,
                    target.conversion
                );
            }
            for (size_t output_i = 0;
                 output_i < _event_output_types.size();
                 ++output_i) {
                auto const& target = _event_output_targets[output_i];
                if (target.target.empty()) {
                    std::construct_at(&state.event_outputs[output_i]);
                    continue;
                }
                auto const target_id = target.target;
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
                    _event_output_types[output_i],
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


        constexpr size_t resolve_default_ttl_samples(size_t default_ttl) const
        {
            if (_has_ttl_samples) return _ttl_samples;
            if (_has_node_default_ttl_samples) return _node_default_ttl_samples;
            return default_ttl;
        }
    };
}
