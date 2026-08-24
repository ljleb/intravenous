#pragma once

#include <intravenous/graph/build_types.h>
#include <intravenous/graph/event_port_data_node.h>
#include <intravenous/graph/static_storage.hpp>
#include <intravenous/graph/wiring.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace iv {
    struct StaticDormancySamplePort {
        StaticString export_id {};
        size_t history = 0;
    };

    struct StaticDormancyEventPort {
        StaticString export_id {};
    };

    struct StaticDormancyGroup {
        size_t parent_group = GRAPH_ID;
        size_t subtree_end_exclusive = 0;
        StaticSpan<size_t> member_nodes {};
        StaticSpan<size_t> wake_check_regions {};
        StaticSpan<StaticDormancySamplePort> sample_input_frontier {};
        StaticSpan<StaticDormancyEventPort> event_input_frontier {};
        StaticSpan<StaticDormancySamplePort> sample_output_frontier {};
        size_t ttl_samples = 0;
        bool has_ttl_samples = false;
        bool can_skip = false;
    };

    struct Graph {
        StaticString _graph_id {};
        StaticSpan<GraphSccWrapper> _scc_wrappers {};
        StaticSpan<GraphPortDataNode> _egress_port_data_nodes {};
        StaticSpan<GraphEventPortDataNode> _egress_event_port_data_nodes {};
        StaticSpan<GraphEdge> _edges {};
        StaticSpan<GraphEventEdge> _event_edges {};
        StaticSpan<StaticInputConfig> _public_inputs {};
        StaticSpan<StaticOutputConfig> _public_outputs {};
        StaticSpan<StaticEventInputConfig> _public_event_inputs {};
        StaticSpan<StaticEventOutputConfig> _public_event_outputs {};
        size_t _internal_latency;
        StaticSpan<StaticString> _node_ids {};
        StaticSpan<StaticDormancyGroup> _dormancy_groups {};
        StaticSpan<size_t> _group_sample_input_offsets {};
        StaticSpan<size_t> _group_event_input_offsets {};
        StaticSpan<size_t> _group_sample_output_offsets {};
        StaticSpan<size_t> _wake_check_group_offsets {};
        StaticSpan<size_t> _wake_check_groups {};

        consteval explicit Graph(GraphBuildArtifact artifact) :
            _graph_id(details::define_static_string(artifact.graph_id)),
            _scc_wrappers(details::define_static_span(artifact.scc_wrappers)),
            _egress_port_data_nodes(details::define_static_span(
                make_egress_port_data_nodes(
                    artifact.graph_id,
                    artifact.public_outputs,
                    artifact.public_output_buffer_plans))),
            _egress_event_port_data_nodes(details::define_static_span(
                make_egress_event_port_data_nodes(
                    artifact.graph_id,
                    artifact.public_event_outputs))),
            _edges(details::define_static_span(artifact.edges)),
            _event_edges(details::define_static_span(artifact.event_edges)),
            _public_inputs(details::define_static_configs<StaticInputConfig>(
                artifact.public_inputs)),
            _public_outputs(details::define_static_configs<StaticOutputConfig>(
                artifact.public_outputs)),
            _public_event_inputs(
                details::define_static_configs<StaticEventInputConfig>(
                    artifact.public_event_inputs)),
            _public_event_outputs(
                details::define_static_configs<StaticEventOutputConfig>(
                    artifact.public_event_outputs)),
            _internal_latency(artifact.internal_latency),
            _node_ids(freeze_strings(artifact.node_ids)),
            _dormancy_groups(),
            _group_sample_input_offsets(),
            _group_event_input_offsets(),
            _group_sample_output_offsets(),
            _wake_check_group_offsets(),
            _wake_check_groups()
        {
            auto dormancy_groups = std::move(artifact.dormancy_groups);
            if (!dormancy_groups.empty()) {
                std::vector<size_t> ordered_old_indices;
                ordered_old_indices.reserve(dormancy_groups.size());
                std::vector<std::uint8_t> emitted(dormancy_groups.size(), 0);
                while (ordered_old_indices.size() < dormancy_groups.size()) {
                    bool progressed = false;
                    for (size_t old_i = 0; old_i < dormancy_groups.size(); ++old_i) {
                        if (emitted[old_i] != 0) {
                            continue;
                        }
                        size_t const parent = dormancy_groups[old_i].parent_group;
                        if (parent == GRAPH_ID || emitted[parent] != 0) {
                            ordered_old_indices.push_back(old_i);
                            emitted[old_i] = 1;
                            progressed = true;
                        }
                    }
                    IV_ASSERT(progressed, "dormancy groups must form an acyclic hierarchy");
                }

                std::vector<size_t> new_index_by_old(dormancy_groups.size(), GRAPH_ID);
                for (size_t new_i = 0; new_i < ordered_old_indices.size(); ++new_i) {
                    new_index_by_old[ordered_old_indices[new_i]] = new_i;
                }

                std::vector<DormancyGroup> ordered_groups;
                ordered_groups.reserve(dormancy_groups.size());
                for (size_t old_i : ordered_old_indices) {
                    DormancyGroup group = std::move(dormancy_groups[old_i]);
                    group.parent_group = group.parent_group == GRAPH_ID ? GRAPH_ID : new_index_by_old[group.parent_group];
                    ordered_groups.push_back(std::move(group));
                }

                std::vector<std::vector<size_t>> children(ordered_groups.size());
                for (size_t group_i = 0; group_i < ordered_groups.size(); ++group_i) {
                    size_t const parent = ordered_groups[group_i].parent_group;
                    if (parent != GRAPH_ID) {
                        children[parent].push_back(group_i);
                    }
                }

                std::vector<DormancyGroup> dfs_groups;
                dfs_groups.reserve(ordered_groups.size());
                std::vector<size_t> dfs_index_by_old(ordered_groups.size(), GRAPH_ID);
                auto emit_group = [&](auto const& self, size_t old_group_i) -> void {
                    size_t const new_group_i = dfs_groups.size();
                    dfs_index_by_old[old_group_i] = new_group_i;
                    dfs_groups.push_back(std::move(ordered_groups[old_group_i]));
                    for (size_t child_i : children[old_group_i]) {
                        self(self, child_i);
                    }
                    dfs_groups[new_group_i].subtree_end_exclusive = dfs_groups.size();
                };
                for (size_t group_i = 0; group_i < ordered_groups.size(); ++group_i) {
                    if (ordered_groups[group_i].parent_group == GRAPH_ID) {
                        emit_group(emit_group, group_i);
                    }
                }
                for (size_t group_i = 0; group_i < dfs_groups.size(); ++group_i) {
                    size_t const parent = dfs_groups[group_i].parent_group;
                    dfs_groups[group_i].parent_group = parent == GRAPH_ID ? GRAPH_ID : dfs_index_by_old[parent];
                }
                dormancy_groups = std::move(dfs_groups);
            }

            std::vector<size_t> group_sample_input_offsets(
                dormancy_groups.size() + 1, 0);
            std::vector<size_t> group_event_input_offsets(
                dormancy_groups.size() + 1, 0);
            std::vector<size_t> group_sample_output_offsets(
                dormancy_groups.size() + 1, 0);
            for (size_t i = 0; i < dormancy_groups.size(); ++i) {
                group_sample_input_offsets[i + 1] = group_sample_input_offsets[i] + dormancy_groups[i].sample_input_frontier.size();
                group_event_input_offsets[i + 1] = group_event_input_offsets[i] + dormancy_groups[i].event_input_frontier.size();
                group_sample_output_offsets[i + 1] = group_sample_output_offsets[i] + dormancy_groups[i].sample_output_frontier.size();
            }

            std::vector<size_t> wake_check_group_offsets(
                _scc_wrappers.size + 1, 0);
            std::vector<size_t> wake_check_groups;
            std::vector<std::vector<size_t>> wake_groups_by_region(
                _scc_wrappers.size);
            for (size_t group_i = 0; group_i < dormancy_groups.size(); ++group_i) {
                for (size_t region_i : dormancy_groups[group_i].wake_check_regions) {
                    if (region_i < wake_groups_by_region.size()) {
                        wake_groups_by_region[region_i].push_back(group_i);
                    }
                }
            }
            for (size_t region_i = 0; region_i < wake_groups_by_region.size(); ++region_i) {
                auto& groups = wake_groups_by_region[region_i];
                std::sort(groups.begin(), groups.end());
                groups.erase(std::unique(groups.begin(), groups.end()), groups.end());
                wake_check_group_offsets[region_i + 1] = wake_check_group_offsets[region_i] + groups.size();
                wake_check_groups.insert(
                    wake_check_groups.end(), groups.begin(), groups.end());
            }

            _dormancy_groups = freeze_dormancy_groups(dormancy_groups);
            _group_sample_input_offsets =
                details::define_static_span(group_sample_input_offsets);
            _group_event_input_offsets =
                details::define_static_span(group_event_input_offsets);
            _group_sample_output_offsets =
                details::define_static_span(group_sample_output_offsets);
            _wake_check_group_offsets =
                details::define_static_span(wake_check_group_offsets);
            _wake_check_groups = details::define_static_span(wake_check_groups);
        }

        static consteval StaticSpan<StaticString> freeze_strings(
            std::span<std::string const> strings)
        {
            std::vector<StaticString> result;
            result.reserve(strings.size());
            for (auto const& string : strings) {
                result.push_back(details::define_static_string(string));
            }
            return details::define_static_span(result);
        }

        static consteval StaticSpan<StaticDormancySamplePort>
        freeze_dormancy_sample_ports(
            std::span<DormancySamplePort const> ports)
        {
            std::vector<StaticDormancySamplePort> result;
            result.reserve(ports.size());
            for (auto const& port : ports) {
                result.push_back({
                    .export_id = details::define_static_string(port.export_id),
                    .history = port.history,
                });
            }
            return details::define_static_span(result);
        }

        static consteval StaticSpan<StaticDormancyEventPort>
        freeze_dormancy_event_ports(
            std::span<DormancyEventPort const> ports)
        {
            std::vector<StaticDormancyEventPort> result;
            result.reserve(ports.size());
            for (auto const& port : ports) {
                result.push_back({
                    .export_id = details::define_static_string(port.export_id),
                });
            }
            return details::define_static_span(result);
        }

        static consteval StaticSpan<StaticDormancyGroup>
        freeze_dormancy_groups(std::span<DormancyGroup const> groups)
        {
            std::vector<StaticDormancyGroup> result;
            result.reserve(groups.size());
            for (auto const& group : groups) {
                result.push_back({
                    .parent_group = group.parent_group,
                    .subtree_end_exclusive = group.subtree_end_exclusive,
                    .member_nodes =
                        details::define_static_span(group.member_nodes),
                    .wake_check_regions =
                        details::define_static_span(group.wake_check_regions),
                    .sample_input_frontier = freeze_dormancy_sample_ports(
                        group.sample_input_frontier),
                    .event_input_frontier = freeze_dormancy_event_ports(
                        group.event_input_frontier),
                    .sample_output_frontier = freeze_dormancy_sample_ports(
                        group.sample_output_frontier),
                    .ttl_samples = group.ttl_samples.value_or(0),
                    .has_ttl_samples = group.ttl_samples.has_value(),
                    .can_skip = group.can_skip,
                });
            }
            return details::define_static_span(result);
        }

        struct State {
            std::span<std::span<std::byte>> scc_states;
            std::span<OutputPort> ingress_outputs;
            std::span<InputPort> egress_inputs;
            std::span<EventOutputPort> ingress_event_outputs;
            std::span<EventInputPort> egress_event_inputs;
            std::span<std::uint8_t> dormancy_group_dormant;
            std::span<std::uint32_t> dormancy_group_blocked_by_ancestors;
            std::span<size_t> dormancy_group_silent_samples_accumulated;
            std::span<size_t> dormancy_group_effective_ttl_samples;
            std::span<Sample> dormancy_remembered_constant_inputs;
            std::span<std::uint8_t> dormancy_remembered_constant_valid;
            std::span<std::uint32_t> dormancy_node_skip_depth;
            std::span<SharedPortData*> dormancy_sample_input_port_data;
            std::span<EventSharedPortData*> dormancy_event_input_port_data;
            std::span<SharedPortData*> dormancy_sample_output_port_data;
        };

        static consteval std::vector<GraphPortDataNode> make_egress_port_data_nodes(
            std::string const& graph_id,
            std::span<OutputConfig const> outputs,
            std::span<PortBufferPlan const> output_buffer_plans
        )
        {
            IV_ASSERT(outputs.size() == output_buffer_plans.size(), "graph egress port data must have one buffer plan per output");

            std::vector<GraphPortDataNode> port_data_nodes;
            port_data_nodes.reserve(outputs.size());
            for (size_t output_i = 0; output_i < outputs.size(); ++output_i) {
                port_data_nodes.emplace_back(
                    graph_port_data_export_id(graph_id, output_i),
                    InputConfig{
                        .name = outputs[output_i].name,
                        .channel_layout = outputs[output_i].channel_layout,
                    },
                    output_buffer_plans[output_i]
                );
            }
            return port_data_nodes;
        }

        static consteval std::vector<GraphEventPortDataNode> make_egress_event_port_data_nodes(
            std::string const& graph_id,
            std::span<EventOutputConfig const> outputs
        )
        {
            std::vector<GraphEventPortDataNode> port_data_nodes;
            port_data_nodes.reserve(outputs.size());
            for (size_t output_i = 0; output_i < outputs.size(); ++output_i) {
                port_data_nodes.emplace_back(
                    graph_event_port_data_export_id(graph_id, output_i),
                    EventInputConfig {
                        .name = outputs[output_i].name,
                        .type = outputs[output_i].type,
                    }
                );
            }
            return port_data_nodes;
        }

        std::string ingress_target_export_id(ConcretePortId target) const
        {
            if (target.node == GRAPH_ID) {
                return graph_port_data_export_id(_graph_id, target.port);
            }
            return port_data_export_id(_node_ids[target.node], target.port);
        }

        std::string ingress_event_target_export_id(ConcretePortId target) const
        {
            if (target.node == GRAPH_ID) {
                return graph_event_port_data_export_id(_graph_id, target.port);
            }
            return event_port_data_export_id(_node_ids[target.node], target.port);
        }

        constexpr auto inputs() const
        {
            std::vector<InputConfig> result;
            result.reserve(_public_inputs.size);
            for (auto const& input : _public_inputs)
                result.push_back(input.config());
            return result;
        }

        constexpr auto outputs() const
        {
            std::vector<OutputConfig> result;
            result.reserve(_public_outputs.size);
            for (auto const& output : _public_outputs)
                result.push_back(output.config());
            return result;
        }

        constexpr auto event_inputs() const
        {
            std::vector<EventInputConfig> result;
            result.reserve(_public_event_inputs.size);
            for (auto const& input : _public_event_inputs)
                result.push_back(input.config());
            return result;
        }

        constexpr auto event_outputs() const
        {
            std::vector<EventOutputConfig> result;
            result.reserve(_public_event_outputs.size);
            for (auto const& output : _public_event_outputs)
                result.push_back(output.config());
            return result;
        }

        auto num_inputs() const
        {
            return _public_inputs.size;
        }

        auto num_outputs() const
        {
            return _public_outputs.size;
        }

        auto num_event_inputs() const
        {
            return _public_event_inputs.size;
        }

        auto num_event_outputs() const
        {
            return _public_event_outputs.size;
        }

        size_t internal_latency() const
        {
            return _internal_latency;
        }

        size_t max_block_size() const
        {
            return MAX_BLOCK_SIZE;
        }

        bool has_group_dormancy() const
        {
            return !_dormancy_groups.empty();
        }

        void declare(DeclarationContext<Graph> const& ctx) const
        {
            auto const& state = ctx.state();
            ctx.local_array(state.ingress_outputs, num_inputs());
            ctx.local_array(state.ingress_event_outputs, num_event_inputs());
            if (has_group_dormancy()) {
                ctx.local_array(state.dormancy_group_dormant, _dormancy_groups.size);
                ctx.local_array(state.dormancy_group_blocked_by_ancestors, _dormancy_groups.size);
                ctx.local_array(state.dormancy_group_silent_samples_accumulated, _dormancy_groups.size);
                ctx.local_array(state.dormancy_group_effective_ttl_samples, _dormancy_groups.size);
                ctx.local_array(state.dormancy_remembered_constant_inputs, _group_sample_input_offsets.back());
                ctx.local_array(state.dormancy_remembered_constant_valid, _group_sample_input_offsets.back());
                ctx.local_array(state.dormancy_node_skip_depth, _node_ids.size);
                ctx.local_array(state.dormancy_sample_input_port_data, _group_sample_input_offsets.back());
                ctx.local_array(state.dormancy_event_input_port_data, _group_event_input_offsets.back());
                ctx.local_array(state.dormancy_sample_output_port_data, _group_sample_output_offsets.back());
                ctx.export_array(graph_dormancy_node_skip_export_id(_graph_id), state.dormancy_node_skip_depth);
            }
            ctx.nested_node_states(state.scc_states);
            for (auto const& scc : _scc_wrappers) {
                do_declare(scc, ctx);
            }
            for (auto const& port_data_node : _egress_port_data_nodes) {
                do_declare(port_data_node, ctx);
            }
            for (auto const& port_data_node : _egress_event_port_data_nodes) {
                do_declare(port_data_node, ctx);
            }
            ctx.local_array(state.egress_inputs, num_outputs());
            ctx.local_array(state.egress_event_inputs, num_event_outputs());
            for (size_t output_i = 0; output_i < num_outputs(); ++output_i) {
                ctx.require_export_array<SharedPortData>(
                    graph_port_data_export_id(_graph_id, output_i)
                );
            }
            for (size_t output_i = 0; output_i < num_event_outputs(); ++output_i) {
                ctx.require_export_array<EventSharedPortData>(
                    graph_event_port_data_export_id(_graph_id, output_i)
                );
            }
            for (GraphEdge const& edge : _edges) {
                if (edge.source.node == GRAPH_ID) {
                    ctx.require_export_array<SharedPortData>(
                        ingress_target_export_id(edge.target)
                    );
                }
            }
            for (GraphEventEdge const& edge : _event_edges) {
                if (edge.source.node == GRAPH_ID) {
                    ctx.require_export_array<EventSharedPortData>(
                        ingress_event_target_export_id(edge.target)
                    );
                }
            }
        }

        void initialize(InitializationContext<Graph> const& ctx) const
        {
            auto& state = ctx.state();
            if (has_group_dormancy()) {
                std::fill(state.dormancy_group_dormant.begin(), state.dormancy_group_dormant.end(), 0);
                std::fill(state.dormancy_group_blocked_by_ancestors.begin(), state.dormancy_group_blocked_by_ancestors.end(), 0);
                std::fill(state.dormancy_group_silent_samples_accumulated.begin(), state.dormancy_group_silent_samples_accumulated.end(), 0);
                std::fill(state.dormancy_remembered_constant_inputs.begin(), state.dormancy_remembered_constant_inputs.end(), 0.0f);
                std::fill(state.dormancy_remembered_constant_valid.begin(), state.dormancy_remembered_constant_valid.end(), 0);
                std::fill(state.dormancy_node_skip_depth.begin(), state.dormancy_node_skip_depth.end(), 0);
                std::fill(state.dormancy_sample_input_port_data.begin(), state.dormancy_sample_input_port_data.end(), nullptr);
                std::fill(state.dormancy_event_input_port_data.begin(), state.dormancy_event_input_port_data.end(), nullptr);
                std::fill(state.dormancy_sample_output_port_data.begin(), state.dormancy_sample_output_port_data.end(), nullptr);
                for (size_t group_i = 0; group_i < _dormancy_groups.size; ++group_i) {
                    auto const& group = _dormancy_groups[group_i];
                    state.dormancy_group_effective_ttl_samples[group_i] =
                        group.has_ttl_samples
                            ? group.ttl_samples
                            : ctx.default_silence_ttl_samples();
                }
            }

            for (size_t output_i = 0; output_i < num_outputs(); ++output_i) {
                auto egress_port_data = ctx.template resolve_exported_array_storage<SharedPortData>(
                    graph_port_data_export_id(_graph_id, output_i)
                );
                IV_ASSERT(!egress_port_data.empty(), "graph egress wiring must resolve the requested SharedPortData entry");
                std::construct_at(&state.egress_inputs[output_i], const_cast<SharedPortData&>(egress_port_data[0]), 0);
            }
            for (size_t output_i = 0; output_i < num_event_outputs(); ++output_i) {
                auto egress_port_data = ctx.template resolve_exported_array_storage<EventSharedPortData>(
                    graph_event_port_data_export_id(_graph_id, output_i)
                );
                IV_ASSERT(!egress_port_data.empty(), "graph egress event wiring must resolve the requested EventSharedPortData entry");
                std::construct_at(&state.egress_event_inputs[output_i], const_cast<EventSharedPortData&>(egress_port_data[0]));
            }

            for (GraphEdge const& edge : _edges) {
                if (edge.source.node == GRAPH_ID) {
                    auto consumer_port_data = ctx.template resolve_exported_array_storage<SharedPortData>(
                        ingress_target_export_id(edge.target)
                    );
                    IV_ASSERT(!consumer_port_data.empty(), "graph ingress wiring must resolve the requested SharedPortData entry");
                    std::construct_at(
                        &state.ingress_outputs[edge.source.port],
                        const_cast<SharedPortData&>(consumer_port_data[0]),
                        0,
                        _public_inputs[edge.source.port].channel_layout,
                        edge.conversion
                    );
                }
            }
            for (GraphEventEdge const& edge : _event_edges) {
                if (edge.source.node == GRAPH_ID) {
                    auto consumer_port_data = ctx.template resolve_exported_array_storage<EventSharedPortData>(
                        ingress_event_target_export_id(edge.target)
                    );
                    IV_ASSERT(!consumer_port_data.empty(), "graph ingress event wiring must resolve the requested EventSharedPortData entry");
                    std::construct_at(
                        &state.ingress_event_outputs[edge.source.port],
                        const_cast<EventSharedPortData&>(consumer_port_data[0]),
                        _public_event_inputs[edge.source.port].type,
                        edge.conversion
                    );
                }
            }

            if (has_group_dormancy()) {
                for (size_t group_i = 0; group_i < _dormancy_groups.size; ++group_i) {
                    auto const& group = _dormancy_groups[group_i];

                    size_t const sample_input_begin = _group_sample_input_offsets[group_i];
                    for (size_t i = 0; i < group.sample_input_frontier.size; ++i) {
                        auto port_data = ctx.template resolve_exported_array_storage<SharedPortData>(
                            std::string(
                                group.sample_input_frontier[i].export_id.view())
                        );
                        IV_ASSERT(!port_data.empty(), "graph dormancy sample input frontier must resolve");
                        state.dormancy_sample_input_port_data[sample_input_begin + i] = const_cast<SharedPortData*>(&port_data[0]);
                    }

                    size_t const event_input_begin = _group_event_input_offsets[group_i];
                    for (size_t i = 0; i < group.event_input_frontier.size; ++i) {
                        auto port_data = ctx.template resolve_exported_array_storage<EventSharedPortData>(
                            std::string(
                                group.event_input_frontier[i].export_id.view())
                        );
                        IV_ASSERT(!port_data.empty(), "graph dormancy event input frontier must resolve");
                        state.dormancy_event_input_port_data[event_input_begin + i] = const_cast<EventSharedPortData*>(&port_data[0]);
                    }

                    size_t const sample_output_begin = _group_sample_output_offsets[group_i];
                    for (size_t i = 0; i < group.sample_output_frontier.size; ++i) {
                        auto port_data = ctx.template resolve_exported_array_storage<SharedPortData>(
                            std::string(
                                group.sample_output_frontier[i].export_id.view())
                        );
                        IV_ASSERT(!port_data.empty(), "graph dormancy sample output frontier must resolve");
                        state.dormancy_sample_output_port_data[sample_output_begin + i] = const_cast<SharedPortData*>(&port_data[0]);
                    }
                }
            }
        }

        void tick_block(TickBlockContext<Graph> const& ctx) const
        {
            auto& state = ctx.state();
            push_input_blocks_to_private_outputs(state.ingress_outputs, ctx.inputs, ctx.block_size);
            if (!state.ingress_event_outputs.empty()) {
                push_input_events_to_private_outputs(
                    state.ingress_event_outputs,
                    ctx.event_inputs,
                    ctx.index,
                    ctx.block_size
                );
            }

            if (!has_group_dormancy()) {
                for (size_t scc_index = 0; scc_index < _scc_wrappers.size; ++scc_index) {
                    do_tick_block(_scc_wrappers[scc_index], {
                        TickContext<GraphSccWrapper> {
                            .inputs = {},
                            .outputs = {},
                            .event_inputs = {},
                            .event_outputs = {},
                            .sample_rate = ctx.sample_rate,
                            .scc_feedback_latency = 0,
                            .buffer = state.scc_states[scc_index]
                        },
                        ctx.index,
                        ctx.block_size,
                    });
                }

                push_private_inputs_to_output_blocks(ctx.outputs, state.egress_inputs, ctx.block_size);
                if (!state.egress_event_inputs.empty()) {
                    push_private_input_events_to_output_events(
                        ctx.event_outputs,
                        state.egress_event_inputs,
                        ctx.index,
                        ctx.block_size
                    );
                }
                return;
            }

            auto block_is_constant = [](BlockView<Sample> block, Sample value) {
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
            };

            auto sample_inputs_unchanged = [&](size_t group_i, bool& inputs_constant) {
                bool unchanged = true;
                inputs_constant = true;
                size_t const begin = _group_sample_input_offsets[group_i];
                size_t const end = _group_sample_input_offsets[group_i + 1];
                for (size_t flat_i = begin; flat_i < end; ++flat_i) {
                    size_t const local_i = flat_i - begin;
                    InputPort input(
                        *state.dormancy_sample_input_port_data[flat_i],
                        _dormancy_groups[group_i].sample_input_frontier[local_i].history
                    );
                    auto block = input.get_block(ctx.block_size);
                    if (block.empty()) {
                        state.dormancy_remembered_constant_valid[flat_i] = 0;
                        unchanged = false;
                        inputs_constant = false;
                        continue;
                    }

                    Sample const first = block[0];
                    bool const matches_previous =
                        state.dormancy_remembered_constant_valid[flat_i] != 0
                        && state.dormancy_remembered_constant_inputs[flat_i] == first;
                    bool const constant = block_is_constant(block, first);

                    state.dormancy_remembered_constant_valid[flat_i] = constant ? 1 : 0;
                    if (constant) {
                        state.dormancy_remembered_constant_inputs[flat_i] = first;
                    }

                    unchanged = unchanged && matches_previous && constant;
                    inputs_constant = inputs_constant && constant;
                }
                return unchanged;
            };

            auto event_inputs_unchanged = [&](size_t group_i) {
                size_t const begin = _group_event_input_offsets[group_i];
                size_t const end = _group_event_input_offsets[group_i + 1];
                for (size_t flat_i = begin; flat_i < end; ++flat_i) {
                    EventInputPort input(*state.dormancy_event_input_port_data[flat_i]);
                    if (input.get_block(ctx.index, ctx.block_size).size() != 0) {
                        return false;
                    }
                }
                return true;
            };

            auto sample_outputs_silent = [&](size_t group_i) {
                size_t const begin = _group_sample_output_offsets[group_i];
                size_t const end = _group_sample_output_offsets[group_i + 1];
                if (begin == end) {
                    return false;
                }
                for (size_t flat_i = begin; flat_i < end; ++flat_i) {
                    size_t const local_i = flat_i - begin;
                    InputPort output(
                        *state.dormancy_sample_output_port_data[flat_i],
                        _dormancy_groups[group_i].sample_output_frontier[local_i].history
                    );
                    if (!block_is_constant(output.get_block(ctx.block_size), 0.0f)) {
                        return false;
                    }
                }
                return true;
            };

            auto apply_group_transition = [&](size_t group_i, std::int32_t delta) {
                for (size_t node_i : _dormancy_groups[group_i].member_nodes) {
                    auto& depth = state.dormancy_node_skip_depth[node_i];
                    depth = static_cast<std::uint32_t>(static_cast<std::int64_t>(depth) + delta);
                }
                for (size_t child_i = group_i + 1; child_i < _dormancy_groups[group_i].subtree_end_exclusive; ++child_i) {
                    auto& blocked = state.dormancy_group_blocked_by_ancestors[child_i];
                    blocked = static_cast<std::uint32_t>(static_cast<std::int64_t>(blocked) + delta);
                }
            };

            for (size_t scc_index = 0; scc_index < _scc_wrappers.size; ++scc_index) {
                size_t const wake_begin = _wake_check_group_offsets[scc_index];
                size_t const wake_end = _wake_check_group_offsets[scc_index + 1];
                for (size_t flat_i = wake_begin; flat_i < wake_end; ++flat_i) {
                    size_t const group_i = _wake_check_groups[flat_i];
                    if (
                        state.dormancy_group_dormant[group_i] == 0
                        || state.dormancy_group_blocked_by_ancestors[group_i] != 0
                    ) {
                        continue;
                    }

                    bool inputs_constant = false;
                    bool const unchanged =
                        sample_inputs_unchanged(group_i, inputs_constant)
                        && event_inputs_unchanged(group_i);
                    if (!unchanged) {
                        state.dormancy_group_dormant[group_i] = 0;
                        state.dormancy_group_silent_samples_accumulated[group_i] = 0;
                        apply_group_transition(group_i, -1);
                    }
                }

                do_tick_block(_scc_wrappers[scc_index], {
                    TickContext<GraphSccWrapper> {
                        .inputs = {},
                        .outputs = {},
                        .event_inputs = {},
                        .event_outputs = {},
                        .sample_rate = ctx.sample_rate,
                        .scc_feedback_latency = 0,
                        .buffer = state.scc_states[scc_index]
                    },
                    ctx.index,
                    ctx.block_size,
                });
            }

            push_private_inputs_to_output_blocks(ctx.outputs, state.egress_inputs, ctx.block_size);
            if (!state.egress_event_inputs.empty()) {
                push_private_input_events_to_output_events(
                    ctx.event_outputs,
                    state.egress_event_inputs,
                    ctx.index,
                    ctx.block_size
                );
            }

            for (size_t group_i = 0; group_i < _dormancy_groups.size; ++group_i) {
                if (
                    state.dormancy_group_dormant[group_i] != 0
                    || state.dormancy_group_blocked_by_ancestors[group_i] != 0
                ) {
                    continue;
                }

                bool inputs_constant = false;
                bool const unchanged =
                    sample_inputs_unchanged(group_i, inputs_constant)
                    && event_inputs_unchanged(group_i);
                bool const silent = sample_outputs_silent(group_i);
                size_t const ttl_samples = state.dormancy_group_effective_ttl_samples[group_i];

                if (ttl_samples == std::numeric_limits<size_t>::max()) {
                    state.dormancy_group_silent_samples_accumulated[group_i] = 0;
                    continue;
                }

                if (inputs_constant && unchanged && silent) {
                    size_t const accumulated = state.dormancy_group_silent_samples_accumulated[group_i] + ctx.block_size;
                    state.dormancy_group_silent_samples_accumulated[group_i] = accumulated;
                    if (accumulated >= ttl_samples) {
                        state.dormancy_group_dormant[group_i] = 1;
                        apply_group_transition(group_i, +1);
                    }
                } else {
                    state.dormancy_group_silent_samples_accumulated[group_i] = 0;
                }
            }
        }
    };
}
