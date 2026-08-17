#include <intravenous/runtime/graph_input_lanes.h>
#include <intravenous/runtime/graph_input_lanes/details.h>

#include <intravenous/basic_nodes/buffers.h>
#include <intravenous/basic_nodes/routing.h>
#include <intravenous/basic_lane_nodes/controls.h>

namespace iv {
using namespace graph_input_lanes_details;

GraphInputLanes::BuilderCompletionDiff GraphInputLanes::complete_builder(
    std::string const &instance_id,
    GraphBuilder &builder)
{
    BuilderCompletionDiff diff;
    auto qualify_descriptor = [&](GraphInputPortDescriptor descriptor) {
        return with_runtime_virtual_node_id(std::move(descriptor), instance_id);
    };
    auto const virtual_ports = builder.virtual_ports();
    auto const public_sample_inputs = builder.public_sample_input_families();
    auto const public_event_inputs = builder.public_event_inputs();
    auto const public_sample_outputs = builder.public_sample_output_families();
    auto const public_event_outputs = builder.public_event_outputs();
    if (virtual_ports.sample_inputs.empty() && virtual_ports.event_inputs.empty()
        && virtual_ports.sample_outputs.empty() && virtual_ports.event_outputs.empty()
        && public_sample_inputs.families.empty() && public_event_inputs.empty()
        && public_sample_outputs.families.empty() && public_event_outputs.empty()) {
        return diff;
    }
    auto const module_instance_id = module_instance_numeric_id(instance_id);
    emit_debug_message(
        "graph input lanes builder ports: instance=" + instance_id
        + " virtualSampleInputs=" + std::to_string(virtual_ports.sample_inputs.size())
        + " virtualEventInputs=" + std::to_string(virtual_ports.event_inputs.size())
        + " virtualSampleOutputs=" + std::to_string(virtual_ports.sample_outputs.size())
        + " virtualEventOutputs=" + std::to_string(virtual_ports.event_outputs.size())
        + " publicSampleInputs=" + std::to_string(public_sample_inputs.families.size())
        + " publicEventInputs=" + std::to_string(public_event_inputs.size())
        + " publicSampleOutputs=" + std::to_string(public_sample_outputs.families.size())
        + " publicEventOutputs=" + std::to_string(public_event_outputs.size()));

    {
        std::scoped_lock lock(mutex);
        auto &instance_ports = desired_ports_by_instance_id[instance_id];
        instance_ports.clear();

        std::unordered_set<std::string> appended;
        auto append_port = [&](GraphInputPortDescriptor descriptor) {
            DesiredGraphInputPort desired{
                .instance_id = instance_id,
                .module_instance_id = module_instance_id,
                .port = std::move(descriptor),
            };
            auto const key = desired_port_key(desired);
            if (!appended.insert(key).second) {
                return;
            }
            instance_ports.push_back(std::move(desired));
        };

        auto sample_descriptor = [&](auto const &port) {
            return qualify_descriptor(GraphInputPortDescriptor{
                .virtual_node_id = port.id.virtual_node_id,
                .port_kind = PortKind::sample,
                .port_ordinal = port.id.port_ordinal,
                .port_name = port.config.name,
                .port_type = "sample",
                .sample_channel_type = port.config.channel_layout.channel_type,
            });
        };
        auto event_descriptor = [&](auto const &port) {
            return qualify_descriptor(GraphInputPortDescriptor{
                .virtual_node_id = port.id.virtual_node_id,
                .port_kind = PortKind::event,
                .port_ordinal = port.id.port_ordinal,
                .port_name = port.config.name,
                .port_type = details::event_type_name(port.config.type),
            });
        };
        for (auto const &input : virtual_ports.sample_inputs) {
            auto descriptor = sample_descriptor(input);
            sample_input_default_values[sample_default_value_key(instance_id, descriptor)] =
                input.config.default_value;
            append_port(descriptor);
            for (size_t bundle_ordinal = 0;
                 bundle_ordinal < input.node_bundle_ports.size(); ++bundle_ordinal) {
                auto bundle_descriptor = descriptor;
                bundle_descriptor.node_bundle_port_ordinal = bundle_ordinal;
                sample_input_default_values[
                    sample_default_value_key(instance_id, bundle_descriptor)] =
                    input.config.default_value;
                append_port(std::move(bundle_descriptor));
            }
        }
        for (auto const &input : virtual_ports.event_inputs) {
            auto descriptor = event_descriptor(input);
            append_port(descriptor);
            for (size_t bundle_ordinal = 0;
                 bundle_ordinal < input.node_bundle_ports.size(); ++bundle_ordinal) {
                auto bundle_descriptor = descriptor;
                bundle_descriptor.node_bundle_port_ordinal = bundle_ordinal;
                append_port(std::move(bundle_descriptor));
            }
        }

        auto &instance_output_ports = desired_output_ports_by_instance_id[instance_id];
        instance_output_ports.clear();
        std::unordered_set<std::string> appended_outputs;
        auto append_output_port = [&](GraphInputPortDescriptor descriptor) {
            DesiredGraphInputPort desired{
                .instance_id = instance_id,
                .module_instance_id = module_instance_id,
                .port = std::move(descriptor),
            };
            auto const key = desired_port_key(desired);
            if (!appended_outputs.insert(key).second) {
                return;
            }
            instance_output_ports.push_back(std::move(desired));
        };
        for (auto const &output : virtual_ports.sample_outputs) {
            auto descriptor = sample_descriptor(output);
            append_output_port(descriptor);
            for (size_t bundle_ordinal = 0;
                 bundle_ordinal < output.node_bundle_ports.size(); ++bundle_ordinal) {
                auto bundle_descriptor = descriptor;
                bundle_descriptor.node_bundle_port_ordinal = bundle_ordinal;
                append_output_port(std::move(bundle_descriptor));
            }
        }
        for (auto const &output : virtual_ports.event_outputs) {
            auto descriptor = event_descriptor(output);
            append_output_port(descriptor);
            for (size_t bundle_ordinal = 0;
                 bundle_ordinal < output.node_bundle_ports.size(); ++bundle_ordinal) {
                auto bundle_descriptor = descriptor;
                bundle_descriptor.node_bundle_port_ordinal = bundle_ordinal;
                append_output_port(std::move(bundle_descriptor));
            }
        }

        desired_public_input_ports_by_instance_id[instance_id] =
            public_graph_input_ports_for(instance_id, builder);
        desired_public_output_ports_by_instance_id[instance_id] =
            public_graph_output_ports_for(instance_id, builder);

        refresh_desired_ports_locked();
        refresh_desired_output_ports_locked();
        refresh_desired_public_input_ports_locked();
        refresh_desired_public_output_ports_locked();
        (void)reconcile_ports_locked(&diff.timeline_batch);
        reconcile_output_ports_locked(&diff.timeline_batch);
        reconcile_public_ports_locked(&diff.timeline_batch);
        queue_timeline_batch_locked(diff.timeline_batch);
    }

    std::unordered_map<std::string, SamplePortRef> sample_control_sources;
    for (auto const &input : virtual_ports.sample_inputs) {
        auto const descriptor = qualify_descriptor(GraphInputPortDescriptor{
            .virtual_node_id = input.id.virtual_node_id,
            .port_kind = PortKind::sample,
            .port_ordinal = input.id.port_ordinal,
            .port_name = input.config.name,
            .port_type = "sample",
            .sample_channel_type = input.config.channel_layout.channel_type,
        });
        auto const qualified_node_id = descriptor.virtual_node_id;
        auto const family_ordinal = input.id.port_ordinal;
        auto const default_value = input.config.default_value;
        (void)ensure_live_input_value_initialized_locked(
            qualified_node_id, family_ordinal, default_value);

        auto const virtual_default_value = live_input_value_or_locked(
            qualified_node_id, family_ordinal, default_value);
        for (size_t bundle_ordinal = 0;
             bundle_ordinal < input.node_bundle_ports.size(); ++bundle_ordinal) {
            auto const target = input.node_bundle_ports[bundle_ordinal];
            auto bundle_descriptor = descriptor;
            bundle_descriptor.node_bundle_port_ordinal = bundle_ordinal;
            auto const state_key = instance_port_state_key("", bundle_descriptor);
            (void)ensure_live_input_value_initialized_locked(
                node_bundle_key(qualified_node_id, bundle_ordinal), family_ordinal,
                virtual_default_value);

            auto state = builder.sample_input_is_connected(target)
                ? NodeBundleSampleInputState::disconnected
                : NodeBundleSampleInputState::virtual_follow;
            if (auto const it = node_bundle_sample_input_states_by_key.find(state_key);
                it != node_bundle_sample_input_states_by_key.end()) state = it->second;
            if (state == NodeBundleSampleInputState::disconnected) continue;

            LaneId prerequisite_lane {};
            std::vector<SamplePortRef> sources;
            auto const channel_total = channel_count(input.config.channel_layout.channel_type);
            sources.reserve(channel_total);
            if (state == NodeBundleSampleInputState::timeline_lane) {
                auto const bindings = query_graph_input_lane_bindings(
                    ProjectGraphInputLaneBindingsRequest{.ports = {bundle_descriptor}});
                if (bindings.sample_inputs.empty()) {
                    throw std::runtime_error("graph input lane completion could not resolve node-bundle sample input lane");
                }
                prerequisite_lane = bindings.sample_inputs.front().graph_input_lane;
            } else if (state == NodeBundleSampleInputState::virtual_follow) {
                auto const it = virtual_sample_knob_states_by_key.find(
                    graph_input_port_key(descriptor));
                auto const virtual_state = it != virtual_sample_knob_states_by_key.end()
                    ? it->second : VirtualSampleKnobState::overridden;
                if (virtual_state == VirtualSampleKnobState::timeline_lane) {
                    auto const bindings = query_graph_input_lane_bindings(
                        ProjectGraphInputLaneBindingsRequest{.ports = {descriptor}});
                    if (bindings.virtual_sample_knobs.empty()) {
                        throw std::runtime_error("virtual sample knob timeline lane could not be resolved");
                    }
                    prerequisite_lane = bindings.virtual_sample_knobs.front().knob_lane;
                }
            }
            for (size_t channel = 0; channel < channel_total; ++channel) {
                if (prerequisite_lane) {
                    auto const identity = LaneInputValue::nominal_identity(prerequisite_lane, channel);
                    if (auto const existing = sample_control_sources.find(identity);
                        existing != sample_control_sources.end()) {
                        sources.push_back(existing->second);
                    } else {
                        auto lane_input = builder.node<LaneInputValue>(prerequisite_lane, channel);
                        auto source = static_cast<SamplePortRef>(lane_input);
                        sample_control_sources.emplace(identity, source);
                        sources.push_back(std::move(source));
                    }
                } else if (state == NodeBundleSampleInputState::overridden) {
                    sources.push_back(builder.node<ValueSource>(live_input_value_ptr_or_locked(
                        node_bundle_key(qualified_node_id, bundle_ordinal), family_ordinal)));
                } else {
                    sources.push_back(builder.node<ValueSource>(live_input_value_ptr_or_locked(
                        qualified_node_id, family_ordinal)));
                }
            }
            builder.connect_sample_input(target, std::span<SamplePortRef const>(sources));
            builder.mark_runtime_filled_sample_input(target);
            if (prerequisite_lane) diff.prerequisite_lanes.push_back(prerequisite_lane);
        }
    }

    for (auto const &input : virtual_ports.event_inputs) {
        auto const descriptor = qualify_descriptor(GraphInputPortDescriptor{
            .virtual_node_id = input.id.virtual_node_id,
            .port_kind = PortKind::event,
            .port_ordinal = input.id.port_ordinal,
            .port_name = input.config.name,
            .port_type = details::event_type_name(input.config.type),
        });
        for (size_t bundle_ordinal = 0;
             bundle_ordinal < input.node_bundle_ports.size(); ++bundle_ordinal) {
            auto const target = input.node_bundle_ports[bundle_ordinal];
            auto bundle_descriptor = descriptor;
            bundle_descriptor.node_bundle_port_ordinal = bundle_ordinal;
            auto state = builder.event_input_is_connected(target)
                ? NodeBundleEventInputState::disconnected
                : NodeBundleEventInputState::virtual_follow;
            if (auto const it = node_bundle_event_input_states_by_key.find(
                    instance_port_state_key("", bundle_descriptor));
                it != node_bundle_event_input_states_by_key.end()) state = it->second;
            if (state == NodeBundleEventInputState::default_) {
                state = builder.event_input_is_connected(target)
                    ? NodeBundleEventInputState::disconnected
                    : NodeBundleEventInputState::virtual_follow;
            }
            if (state == NodeBundleEventInputState::disconnected) continue;
            auto const requested = state == NodeBundleEventInputState::timeline_lane
                ? bundle_descriptor : descriptor;
            auto const bindings = query_graph_input_lane_bindings(
                ProjectGraphInputLaneBindingsRequest{.ports = {requested}});
            if (bindings.event_inputs.empty()) {
                throw std::runtime_error("graph input lane completion could not resolve node-bundle event input lane");
            }
            auto source = builder.node<EventConcatenation>(0, input.config.type).event_port(0);
            builder.connect_event_input(target, source);
            builder.mark_runtime_filled_event_input(target);
            diff.prerequisite_lanes.push_back(bindings.event_inputs.front().graph_input_lane);
        }
    }

    std::unordered_map<std::uint64_t, NodeRef> sample_sink_by_lane;
    std::unordered_map<std::uint64_t, NodeRef> event_sink_by_lane;

    for (auto const &output : virtual_ports.sample_outputs) {
        auto const descriptor = qualify_descriptor(GraphInputPortDescriptor{
            .virtual_node_id = output.id.virtual_node_id,
            .port_kind = PortKind::sample,
            .port_ordinal = output.id.port_ordinal,
            .port_name = output.config.name,
            .port_type = "sample",
            .sample_channel_type = output.config.channel_layout.channel_type,
        });
        for (size_t bundle_ordinal = 0;
             bundle_ordinal < output.node_bundle_ports.size(); ++bundle_ordinal) {
            auto bundle_descriptor = descriptor;
            bundle_descriptor.node_bundle_port_ordinal = bundle_ordinal;
            DesiredGraphInputPort desired{
                .instance_id = instance_id,
                .module_instance_id = module_instance_id,
                .port = bundle_descriptor,
            };
            auto const state = effective_node_bundle_output_state_locked(desired);
            if (!state.has_value()) continue;
            auto const lane = *state == NodeBundleOutputState::timeline_lane
                ? graph_output_lane_for(bundle_descriptor, false)
                : graph_output_lane_for(descriptor, true);
            if (!lane) continue;
            auto it = sample_sink_by_lane.find(lane.value);
            if (it == sample_sink_by_lane.end()) {
                it = sample_sink_by_lane.emplace(
                    lane.value,
                    add_graph_sample_output_sink(
                        builder, lane, output.config.channel_layout.channel_type)).first;
            }
            builder.connect_sample_output(output.node_bundle_ports[bundle_ordinal], it->second);
        }
    }

    for (auto const &output : virtual_ports.event_outputs) {
        auto const descriptor = qualify_descriptor(GraphInputPortDescriptor{
            .virtual_node_id = output.id.virtual_node_id,
            .port_kind = PortKind::event,
            .port_ordinal = output.id.port_ordinal,
            .port_name = output.config.name,
            .port_type = details::event_type_name(output.config.type),
        });
        for (size_t bundle_ordinal = 0;
             bundle_ordinal < output.node_bundle_ports.size(); ++bundle_ordinal) {
            auto bundle_descriptor = descriptor;
            bundle_descriptor.node_bundle_port_ordinal = bundle_ordinal;
            DesiredGraphInputPort desired{
                .instance_id = instance_id,
                .module_instance_id = module_instance_id,
                .port = bundle_descriptor,
            };
            auto const state = effective_node_bundle_output_state_locked(desired);
            if (!state.has_value()) continue;
            auto const lane = *state == NodeBundleOutputState::timeline_lane
                ? graph_output_lane_for(bundle_descriptor, false)
                : graph_output_lane_for(descriptor, true);
            if (!lane) continue;
            auto it = event_sink_by_lane.find(lane.value);
            if (it == event_sink_by_lane.end()) {
                it = event_sink_by_lane.emplace(
                    lane.value,
                    builder.node<GraphEventOutputSink>(lane, output.config.type)).first;
            }
            it->second.connect_event_input(
                0, builder.event_output(output.node_bundle_ports[bundle_ordinal]));
        }
    }

    if (!public_sample_inputs.families.empty()
        || !public_event_inputs.empty()
        || !public_sample_outputs.families.empty()
        || !public_event_outputs.empty()) {
        GraphBuilder execution_builder;
        auto embedded = execution_builder.embed_subgraph(builder);

        {
            std::scoped_lock lock(mutex);
            for (auto const &port : desired_public_input_ports_by_instance_id[instance_id]) {
                auto const virtual_state_key = public_sample_input_state_key(
                    port.instance_id, port.source_identity, std::nullopt);
                auto const virtual_state_it = public_sample_input_states_by_key.find(virtual_state_key);
                auto const virtual_state = port.source_identity.empty()
                    ? ProjectSampleInputState::timeline_lane
                    : virtual_state_it == public_sample_input_states_by_key.end()
                        ? ProjectSampleInputState::timeline_lane
                        : virtual_state_it->second;
                auto const lane = port.port_kind == PortKind::sample
                    ? effective_public_sample_input_lane_locked(port)
                    : effective_public_event_input_lane_locked(port);
                auto const use_override_value = port.port_kind == PortKind::sample
                    && !port.source_identity.empty()
                    && virtual_state == ProjectSampleInputState::overridden
                    && (!lane.has_value() || !*lane);
                if ((!lane.has_value() || !*lane) && !use_override_value) {
                    continue;
                }
                if (port.port_kind == PortKind::sample) {
                    for (size_t channel_index = 0; channel_index < port.channels.size(); ++channel_index) {
                        auto const port_ordinal = port.channels[channel_index].port_ordinal;
                        if (!port_ordinal.has_value()) {
                            continue;
                        }
                        NodeRef source = use_override_value
                            ? execution_builder.node<ValueSource>(
                                &ensure_public_sample_input_value_locked(
                                    port.instance_id, port.source_identity, port.default_value)).node_ref()
                            : execution_builder.node<LaneInputValue>(*lane, channel_index).node_ref();
                        embedded.connect_input(*port_ordinal, source);
                    }
                } else if (port.event_type.has_value()) {
                    embedded.connect_event_input(
                        port.port_ordinal,
                        execution_builder.node<LaneInputEvents>(*lane, *port.event_type).event_port(0));
                }
                if (lane.has_value() && *lane) {
                    diff.prerequisite_lanes.push_back(*lane);
                }
            }

            std::unordered_set<std::string> wired_public_output_ports;
            for (auto const &port : desired_public_output_ports_by_instance_id[instance_id]) {
                auto const lane = public_graph_port_lane_for(port);
                if (!lane) {
                    continue;
                }
                // A public lane aggregates all source-span contributors for a
                // family. The embedded graph already sums those contributors;
                // wire its concrete channel outputs into one sink only.
                if (!wired_public_output_ports.insert(public_port_key(port)).second) {
                    continue;
                }
                if (port.port_kind == PortKind::sample) {
                    auto sink = add_graph_sample_output_sink(
                        execution_builder,
                        lane,
                        port.sample_channel_type.value_or(ChannelTypeId::mono));
                    for (size_t channel_index = 0; channel_index < port.channels.size(); ++channel_index) {
                        auto const port_ordinal = port.channels[channel_index].port_ordinal;
                        if (!port_ordinal.has_value()) {
                            continue;
                        }
                        sink.connect_input(channel_index, embedded[*port_ordinal]);
                    }
                } else if (port.event_type.has_value()) {
                    execution_builder.node<GraphEventOutputSink>(lane, *port.event_type)
                        .connect_event_input(0, embedded.event_port(port.port_ordinal));
                }
            }
        }

        execution_builder.outputs();
        builder = std::move(execution_builder);
    }

    std::ranges::sort(diff.prerequisite_lanes, {}, &LaneId::value);
    diff.prerequisite_lanes.erase(
        std::unique(diff.prerequisite_lanes.begin(), diff.prerequisite_lanes.end()),
        diff.prerequisite_lanes.end());
    return diff;
}


} // namespace iv
