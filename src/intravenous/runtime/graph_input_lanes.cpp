#include <intravenous/runtime/graph_input_lanes.h>
#include <intravenous/runtime/graph_input_lanes/port_catalog.h>

#include <intravenous/basic_nodes/buffers.h>
#include <intravenous/basic_nodes/routing.h>
#include <intravenous/basic_lane_nodes/controls.h>
#include <intravenous/runtime/runtime_project_events.h>
#include <intravenous/runtime/task_ids.h>
#include <intravenous/runtime/uuid.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <iterator>
#include <stdexcept>
#include <utility>

#include <intravenous/runtime/graph_input_lanes/details.h>

namespace iv {
using namespace graph_input_lanes_details;

std::vector<GraphInputLanes::DesiredGraphPort>
GraphInputLanes::graph_input_port_descriptors_for(
    IvModuleInstance const &instance)
{
    return GraphInputLanesPortCatalog::graph_inputs(instance);
}

std::vector<GraphInputLanes::DesiredGraphPort>
GraphInputLanes::graph_output_port_descriptors_for(
    IvModuleInstance const &instance)
{
    return GraphInputLanesPortCatalog::graph_outputs(instance);
}

std::vector<GraphInputLanes::DesiredPublicGraphPort>
GraphInputLanes::public_graph_input_ports_for(
    IvModuleInstance const &instance)
{
    return GraphInputLanesPortCatalog::public_inputs(instance);
}

std::vector<GraphInputLanes::DesiredPublicGraphPort>
GraphInputLanes::public_graph_output_ports_for(
    IvModuleInstance const &instance)
{
    return GraphInputLanesPortCatalog::public_outputs(instance);
}
int GraphInputLanes::module_instance_numeric_id(std::string_view instance_id)
{
    auto const separator = instance_id.rfind(':');
    if (separator != std::string_view::npos) {
        int parsed = 0;
        auto const numeric = instance_id.substr(separator + 1);
        auto const [ptr, ec] = std::from_chars(
            numeric.data(),
            numeric.data() + numeric.size(),
            parsed);
        if (ec == std::errc{} && ptr == numeric.data() + numeric.size()) {
            return parsed;
        }
    }
    return hash_string(std::string(instance_id));
}

int GraphInputLanes::hash_string(std::string const &value)
{
    return static_cast<int>(std::hash<std::string>{}(value));
}

std::string GraphInputLanes::node_bundle_key(std::string_view virtual_node_id, size_t member_ordinal)
{
    return node_bundle_key_prefix(virtual_node_id) + std::to_string(member_ordinal);
}

std::string GraphInputLanes::node_bundle_key_prefix(std::string_view virtual_node_id)
{
    return std::string(virtual_node_id) + "\x1fmember:";
}

std::string GraphInputLanes::node_bundle_override_key(
    std::string_view virtual_node_id,
    size_t member_ordinal,
    size_t input_ordinal)
{
    return node_bundle_key(virtual_node_id, member_ordinal) + "\x1finput:" + std::to_string(input_ordinal);
}

std::string GraphInputLanes::desired_port_key(DesiredGraphPort const &port)
{
    auto const virtual_node_id = hash_string(port.port.virtual_node_id);
    auto const node_bundle_port_id = hash_string(
        port.port.virtual_node_id
        + "\x1fmember:"
        + (port.port.node_bundle_port_ordinal.has_value()
            ? std::to_string(*port.port.node_bundle_port_ordinal)
            : std::string("virtual")));

    return "instance:" + std::to_string(port.module_instance_id)
        + "\x1f" + "virtual:" + std::to_string(virtual_node_id)
        + "\x1f" + "node-bundle:" + std::to_string(node_bundle_port_id)
        + "\x1f" + "kind:" + std::to_string(static_cast<int>(port.port.port_kind == PortKind::event))
        + "\x1f" + "ordinal:" + std::to_string(port.port.port_ordinal)
        + "\x1f" + "channel:"
        + (port.port.sample_channel_type.has_value()
            ? std::to_string(static_cast<int>(*port.port.sample_channel_type))
            : std::string("none"));
}

std::string GraphInputLanes::graph_input_port_key(GraphInputPortDescriptor const &port)
{
    std::string key = std::to_string(hash_string(port.virtual_node_id));
    key += "\x1fmember:";
    key += port.node_bundle_port_ordinal.has_value()
        ? std::to_string(*port.node_bundle_port_ordinal)
        : "virtual";
    key += "\x1fkind:";
    key += port.port_kind == PortKind::sample ? "sample" : "event";
    key += "\x1fordinal:";
    key += std::to_string(port.port_ordinal);
    key += "\x1f" "channel:";
    key += port.sample_channel_type.has_value()
        ? std::to_string(static_cast<int>(*port.sample_channel_type))
        : "none";
    return key;
}

std::string GraphInputLanes::public_port_key(DesiredPublicGraphPort const &port)
{
    return public_port_identity_key(port);
}

std::string GraphInputLanes::public_sample_input_state_key(
    std::string_view instance_id,
    std::string_view source_identity,
    std::optional<size_t> member_ordinal)
{
    auto key = std::string(instance_id) + "\x1fpublic-source:" + std::string(source_identity);
    key += member_ordinal.has_value()
        ? "\x1fmember:" + std::to_string(*member_ordinal)
        : "\x1fmember:virtual";
    return key;
}

std::string GraphInputLanes::public_port_external_id(DesiredPublicGraphPort const &port)
{
    auto id = "iv.public:"
        + std::string(port.input ? "input" : "output")
        + ":instance:" + port.instance_id
        + ":kind:" + std::string(port.port_kind == PortKind::sample ? "sample" : "event")
        + ":";
    if (port.input
        && public_source_identity_hash(port).has_value()) {
        auto const source_identity_hash = *public_source_identity_hash(port);
        id += "source:" + std::to_string(source_identity_hash);
        id += port.node_bundle_port_ordinal.has_value()
            ? ":member:" + std::to_string(*port.node_bundle_port_ordinal)
            : ":member:virtual";
    } else {
        id += "ordinal:" + std::to_string(port.port_ordinal);
        if (auto const name_hash = public_port_name_hash(port); name_hash.has_value()) {
            id += ":name:" + std::to_string(*name_hash);
        }
    }
    return id + ":channel:"
        + (port.sample_channel_type.has_value()
            ? std::to_string(static_cast<int>(*port.sample_channel_type))
            : std::string("none"));
}

std::string GraphInputLanes::sample_default_value_key(
    std::string_view instance_id,
    GraphInputPortDescriptor const &port)
{
    return std::string(instance_id) + "\x1f" + graph_input_port_key(port);
}

std::string GraphInputLanes::instance_port_state_key(
    std::string_view instance_id,
    GraphInputPortDescriptor const &port)
{
    (void)instance_id;
    return graph_input_port_key(port);
}

GraphInputPortDescriptor GraphInputLanes::sample_input_descriptor(
    std::string const &node_id,
    std::optional<size_t> member_ordinal,
    size_t input_ordinal,
    ChannelTypeId channel_type)
{
    return GraphInputPortDescriptor{
        .virtual_node_id = node_id,
        .node_bundle_port_ordinal = member_ordinal,
        .port_kind = PortKind::sample,
        .port_ordinal = input_ordinal,
        .port_name = "",
        .port_type = "sample",
        .sample_channel_type = channel_type,
    };
}

LaneMetadata GraphInputLanes::graph_input_metadata(
    DesiredGraphPort const &port,
    bool knob,
    bool is_virtual,
    bool concrete,
    bool sample,
    bool event)
{
    LaneMetadata metadata;
    metadata.set_unit(std::string(metadata_dsp_graph));
    metadata.set_unit(std::string(metadata_graph_input));
    if (knob) {
        metadata.set_unit(std::string(metadata_knob));
    } else {
        metadata.set_unit(std::string(metadata_input));
    }
    if (is_virtual) {
        metadata.set_unit(std::string(metadata_virtual));
    }
    if (concrete) {
        metadata.set_unit(std::string(metadata_node_bundle));
    }
    if (sample) {
        metadata.set_unit(std::string(metadata_sample));
    }
    if (event) {
        metadata.set_unit(std::string(metadata_event));
    }
    metadata.set_int(std::string(metadata_module_instance_id), port.module_instance_id);
    metadata.set_int(std::string(metadata_virtual_node_id), hash_string(port.port.virtual_node_id));
    metadata.set_int(
        std::string(metadata_node_bundle_node_id),
        hash_string(
            port.port.virtual_node_id
            + "\x1fmember:"
            + (port.port.node_bundle_port_ordinal.has_value()
                ? std::to_string(*port.port.node_bundle_port_ordinal)
                : std::string("virtual"))));
    metadata.set_int(
        std::string(metadata_port_kind),
        static_cast<int>(port.port.port_kind == PortKind::event));
    metadata.set_int(
        std::string(metadata_port_ordinal),
        static_cast<int>(port.port.port_ordinal));
    if (port.port.sample_channel_type.has_value()) {
        metadata.set_int(
            std::string(metadata_channel_type),
            static_cast<int>(*port.port.sample_channel_type));
    }
    if (port.port.node_bundle_port_ordinal.has_value()) {
        metadata.set_int(
            std::string(metadata_node_bundle_port_ordinal),
            static_cast<int>(*port.port.node_bundle_port_ordinal));
    }
    return metadata;
}

LaneMetadata GraphInputLanes::graph_output_metadata(
    DesiredGraphPort const &port,
    bool is_virtual,
    bool concrete,
    bool sample,
    bool event)
{
    LaneMetadata metadata;
    metadata.set_unit(std::string(metadata_dsp_graph));
    metadata.set_unit(std::string(metadata_graph_output));
    metadata.set_unit(std::string(metadata_output));
    if (is_virtual) {
        metadata.set_unit(std::string(metadata_virtual));
    }
    if (concrete) {
        metadata.set_unit(std::string(metadata_node_bundle));
    }
    if (sample) {
        metadata.set_unit(std::string(metadata_sample));
    }
    if (event) {
        metadata.set_unit(std::string(metadata_event));
    }
    metadata.set_int(std::string(metadata_module_instance_id), port.module_instance_id);
    metadata.set_int(std::string(metadata_virtual_node_id), hash_string(port.port.virtual_node_id));
    metadata.set_int(
        std::string(metadata_node_bundle_node_id),
        hash_string(
            port.port.virtual_node_id
            + "\x1fmember:"
            + (port.port.node_bundle_port_ordinal.has_value()
                ? std::to_string(*port.port.node_bundle_port_ordinal)
                : std::string("virtual"))));
    metadata.set_int(
        std::string(metadata_port_kind),
        static_cast<int>(port.port.port_kind == PortKind::event));
    metadata.set_int(
        std::string(metadata_port_ordinal),
        static_cast<int>(port.port.port_ordinal));
    if (port.port.sample_channel_type.has_value()) {
        metadata.set_int(
            std::string(metadata_channel_type),
            static_cast<int>(*port.port.sample_channel_type));
    }
    if (port.port.node_bundle_port_ordinal.has_value()) {
        metadata.set_int(
            std::string(metadata_node_bundle_port_ordinal),
            static_cast<int>(*port.port.node_bundle_port_ordinal));
    }
    return metadata;
}

LaneMetadata GraphInputLanes::public_graph_port_metadata(
    DesiredPublicGraphPort const &port,
    bool sample,
    bool event)
{
    LaneMetadata metadata;
    metadata.set_unit(std::string(metadata_dsp_graph));
    metadata.set_unit(std::string(metadata_public));
    metadata.set_unit(std::string(port.input ? metadata_public_input : metadata_public_output));
    metadata.set_unit(std::string(port.input ? metadata_graph_input : metadata_graph_output));
    metadata.set_unit(std::string(port.input ? metadata_input : metadata_output));
    if (sample) {
        metadata.set_unit(std::string(metadata_sample));
    }
    if (event) {
        metadata.set_unit(std::string(metadata_event));
    }
    metadata.set_int(std::string(metadata_module_instance_id), port.module_instance_id);
    metadata.set_int(
        std::string(metadata_port_kind),
        static_cast<int>(port.port_kind == PortKind::event));
    metadata.set_int(
        std::string(metadata_port_ordinal),
        static_cast<int>(port.port_ordinal));
    if (port.input) if (auto const source_identity_hash = public_source_identity_hash(port); source_identity_hash.has_value()) {
        metadata.set_int(
            std::string(metadata_public_source_id),
            *source_identity_hash);
    }
    if (port.input && port.node_bundle_port_ordinal.has_value()) {
        metadata.set_int(
            std::string(metadata_node_bundle_port_ordinal),
            static_cast<int>(*port.node_bundle_port_ordinal));
    }
    if (!port.input) if (auto const name_hash = public_port_name_hash(port); name_hash.has_value()) {
        metadata.set_int(std::string(metadata_public_port_name), *name_hash);
    }
    if (port.sample_channel_type.has_value()) {
        metadata.set_int(
            std::string(metadata_channel_type),
            static_cast<int>(*port.sample_channel_type));
    }
    return metadata;
}

bool GraphInputLanes::lane_metadata_matches_port(
    LaneMetadata const &metadata,
    DesiredGraphPort const &port)
{
    auto const virtual_node_id = metadata.int_value(metadata_virtual_node_id);
    auto const node_bundle_port_id = metadata.int_value(metadata_node_bundle_node_id);
    auto const port_kind = metadata.int_value(metadata_port_kind);
    auto const port_ordinal = metadata.int_value(metadata_port_ordinal);
    auto const channel_type = metadata.int_value(metadata_channel_type);
    if (!virtual_node_id.has_value()
        || !node_bundle_port_id.has_value()
        || !port_kind.has_value()
        || !port_ordinal.has_value()) {
        return false;
    }

    auto const expected_virtual = hash_string(port.port.virtual_node_id);
    auto const expected_node_bundle = hash_string(
        port.port.virtual_node_id
        + "\x1fmember:"
        + (port.port.node_bundle_port_ordinal.has_value()
            ? std::to_string(*port.port.node_bundle_port_ordinal)
            : std::string("virtual")));

    return *virtual_node_id == expected_virtual
        && *node_bundle_port_id == expected_node_bundle
        && *port_kind == static_cast<int>(port.port.port_kind == PortKind::event)
        && *port_ordinal == static_cast<int>(port.port.port_ordinal)
        && channel_type
            == (port.port.sample_channel_type.has_value()
                ? std::optional<int>(static_cast<int>(*port.port.sample_channel_type))
                : std::nullopt);
}

bool GraphInputLanes::has_node_bundle_descriptor_for_port(
    std::span<DesiredGraphPort const> ports,
    DesiredGraphPort const &virtual_port)
{
    for (auto const &port : ports) {
        if (port.instance_id != virtual_port.instance_id) {
            continue;
        }
        if (port.port.virtual_node_id != virtual_port.port.virtual_node_id) {
            continue;
        }
        if (port.port.port_kind != virtual_port.port.port_kind) {
            continue;
        }
        if (port.port.port_ordinal != virtual_port.port.port_ordinal) {
            continue;
        }
        if (port.port.node_bundle_port_ordinal.has_value()) {
            return true;
        }
    }
    return false;
}

void GraphInputLanes::handle_iv_module_instance_builders_changed(
    IvModuleInstanceBuildersChanged const &diff,
    IvModuleInstanceBuildersAckBuilder *ack_builder)
{
    TimelineLaneBatchUpdate batch;
    {
        std::scoped_lock lock(mutex);
        for (auto const &created : diff.created) {
            if (created.instance == nullptr) {
                continue;
            }
            runtime_bindings_by_instance_id[created.instance->instance_id] =
                created.instance->runtime_bindings;
            desired_ports_by_instance_id[created.instance->instance_id] =
                graph_input_port_descriptors_for(*created.instance);
            desired_output_ports_by_instance_id[created.instance->instance_id] =
                graph_output_port_descriptors_for(*created.instance);
            desired_public_input_ports_by_instance_id[created.instance->instance_id] =
                public_graph_input_ports_for(*created.instance);
            desired_public_output_ports_by_instance_id[created.instance->instance_id] =
                public_graph_output_ports_for(*created.instance);
        }
        for (auto const &updated : diff.updated) {
            if (updated.instance == nullptr) {
                continue;
            }
            runtime_bindings_by_instance_id[updated.instance->instance_id] =
                updated.instance->runtime_bindings;
            desired_ports_by_instance_id[updated.instance->instance_id] =
                graph_input_port_descriptors_for(*updated.instance);
            desired_output_ports_by_instance_id[updated.instance->instance_id] =
                graph_output_port_descriptors_for(*updated.instance);
            desired_public_input_ports_by_instance_id[updated.instance->instance_id] =
                public_graph_input_ports_for(*updated.instance);
            desired_public_output_ports_by_instance_id[updated.instance->instance_id] =
                public_graph_output_ports_for(*updated.instance);
        }
        for (auto const &deleted_instance_id : diff.deleted_instance_ids) {
            desired_ports_by_instance_id.erase(deleted_instance_id);
            desired_output_ports_by_instance_id.erase(deleted_instance_id);
            desired_public_input_ports_by_instance_id.erase(deleted_instance_id);
            desired_public_output_ports_by_instance_id.erase(deleted_instance_id);
            runtime_bindings_by_instance_id.erase(deleted_instance_id);
            pending_runtime_binding_syncs.erase(deleted_instance_id);
        }
        refresh_desired_ports_locked();
        refresh_desired_output_ports_locked();
        refresh_desired_public_input_ports_locked();
        refresh_desired_public_output_ports_locked();
        (void)reconcile_ports_locked(&batch);
        reconcile_output_ports_locked(&batch);
        reconcile_public_ports_locked(&batch);
        for (auto const &created : diff.created) {
            if (created.instance == nullptr) continue;
            sync_runtime_bindings_locked(created.instance->instance_id);
            if (ack_builder != nullptr) {
                ack_builder->set_prerequisite_lanes(
                    created.instance->instance_id,
                    prerequisite_lanes_for_instance_locked(
                        created.instance->instance_id));
            }
        }
        for (auto const &updated : diff.updated) {
            if (updated.instance == nullptr) continue;
            sync_runtime_bindings_locked(updated.instance->instance_id);
            if (ack_builder != nullptr) {
                ack_builder->set_prerequisite_lanes(
                    updated.instance->instance_id,
                    prerequisite_lanes_for_instance_locked(
                        updated.instance->instance_id));
            }
        }
        queue_timeline_batch_locked(batch);
        if (ack_builder != nullptr) {
            ack_builder->set_version_index(current_update_version_index_);
        }
    }

    if (!diff.created.empty() || !diff.updated.empty() || !diff.deleted_instance_ids.empty()) {
        emit_debug_message(
            "graph input lanes builders changed: created="
            + std::to_string(diff.created.size())
            + " updated=" + std::to_string(diff.updated.size())
            + " deleted=" + std::to_string(diff.deleted_instance_ids.size())
            + " ackVersion="
            + std::to_string(ack_builder != nullptr ? current_update_version_index_ : diff.version_index));
    }

}

std::vector<IvModuleSourceIntrospectionLiveInputSnapshot>
GraphInputLanes::collect_live_input_snapshots(
    std::vector<IvModuleSourceIntrospectionLiveInputSnapshotRequest> const &requests)
{
    std::scoped_lock lock(mutex);
    std::vector<IvModuleSourceIntrospectionLiveInputSnapshot> snapshots;
    snapshots.reserve(requests.size());
    for (auto const &request : requests) {
        snapshots.push_back(IvModuleSourceIntrospectionLiveInputSnapshot{
            .virtual_node_id = request.virtual_node_id,
            .member_ordinal = request.member_ordinal,
            .input_ordinal = request.input_ordinal,
            .current_value = request.member_ordinal.has_value()
                ? live_input_value_or_locked(
                    request.virtual_node_id,
                    *request.member_ordinal,
                    request.input_ordinal,
                    request.fallback)
                : live_input_value_or_locked(
                    request.virtual_node_id,
                    request.input_ordinal,
                    request.fallback),
            .has_concrete_override = request.member_ordinal.has_value()
                ? node_bundle_live_input_overrides.contains(
                    node_bundle_override_key(
                        request.virtual_node_id,
                        *request.member_ordinal,
                        request.input_ordinal))
                : false,
        });
    }
    return snapshots;
}

GraphInputLaneBindings GraphInputLanes::query_graph_input_lane_bindings(
    ProjectGraphInputLaneBindingsRequest const &request)
{
    std::scoped_lock lock(mutex);
    GraphInputLaneBindings bindings;

    auto find_lane = [&](std::function<bool(ExistingTrackedLane const &)> const &matches) -> LaneId {
        for (auto const &tracked : tracked_lanes) {
            if (matches(tracked)) {
                return tracked.lane;
            }
        }
        return LaneId{};
    };

    for (auto const &port : request.ports) {
        if (port.port_kind == PortKind::sample && !port.node_bundle_port_ordinal.has_value()) {
            auto virtual_lane = find_lane([&](ExistingTrackedLane const &tracked) {
                return tracked.metadata.has_unit(metadata_knob)
                    && tracked.metadata.has_unit(metadata_virtual)
                    && tracked.metadata.has_unit(metadata_sample)
                    && tracked.metadata.int_value(metadata_virtual_node_id) == hash_string(port.virtual_node_id)
                    && tracked.metadata.int_value(metadata_port_kind) == 0
                    && tracked.metadata.int_value(metadata_port_ordinal) == static_cast<int>(port.port_ordinal)
                    && tracked.metadata.int_value(metadata_channel_type)
                        == (port.sample_channel_type.has_value()
                            ? std::optional<int>(static_cast<int>(*port.sample_channel_type))
                            : std::nullopt);
            });
            if (virtual_lane) {
                bindings.virtual_sample_knobs.push_back(GraphInputLaneBinding{
                    .port = port,
                    .knob_lane = virtual_lane,
                });
            }
        }
    }

    for (auto const &port : request.ports) {
        if (port.port_kind == PortKind::sample) {
            auto node_bundle_lane = find_lane([&](ExistingTrackedLane const &tracked) {
                return tracked.metadata.has_unit(metadata_knob)
                    && tracked.metadata.has_unit(metadata_node_bundle)
                    && tracked.metadata.has_unit(metadata_sample)
                    && tracked.metadata.int_value(metadata_virtual_node_id) == hash_string(port.virtual_node_id)
                    && tracked.metadata.int_value(metadata_port_kind) == 0
                    && tracked.metadata.int_value(metadata_port_ordinal) == static_cast<int>(port.port_ordinal)
                    && tracked.metadata.int_value(metadata_channel_type)
                        == (port.sample_channel_type.has_value()
                            ? std::optional<int>(static_cast<int>(*port.sample_channel_type))
                            : std::nullopt)
                    && tracked.metadata.int_value(metadata_node_bundle_port_ordinal)
                        == (port.node_bundle_port_ordinal.has_value()
                            ? std::optional<int>(static_cast<int>(*port.node_bundle_port_ordinal))
                            : std::nullopt);
            });
            auto graph_input_lane = find_lane([&](ExistingTrackedLane const &tracked) {
                return tracked.metadata.has_unit(metadata_input)
                    && tracked.metadata.has_unit(metadata_sample)
                    && tracked.metadata.int_value(metadata_virtual_node_id) == hash_string(port.virtual_node_id)
                    && tracked.metadata.int_value(metadata_port_kind) == 0
                    && tracked.metadata.int_value(metadata_port_ordinal) == static_cast<int>(port.port_ordinal)
                    && tracked.metadata.int_value(metadata_channel_type)
                        == (port.sample_channel_type.has_value()
                            ? std::optional<int>(static_cast<int>(*port.sample_channel_type))
                            : std::nullopt)
                    && tracked.metadata.int_value(metadata_node_bundle_port_ordinal)
                        == (port.node_bundle_port_ordinal.has_value()
                            ? std::optional<int>(static_cast<int>(*port.node_bundle_port_ordinal))
                            : std::nullopt);
            });
            if (node_bundle_lane || graph_input_lane) {
                std::optional<LaneId> virtual_knob;
                for (auto const &binding : bindings.virtual_sample_knobs) {
                    if (binding.port.virtual_node_id == port.virtual_node_id
                        && binding.port.port_kind == port.port_kind
                        && binding.port.port_ordinal == port.port_ordinal) {
                        virtual_knob = binding.knob_lane;
                        break;
                    }
                }
                bindings.sample_inputs.push_back(GraphInputLaneBinding{
                    .port = port,
                    .knob_lane = node_bundle_lane,
                    .graph_input_lane = graph_input_lane,
                    .virtual_knob_lane = virtual_knob,
                });
            }
            continue;
        }

        auto graph_input_lane = find_lane([&](ExistingTrackedLane const &tracked) {
            return tracked.metadata.has_unit(metadata_input)
                && tracked.metadata.has_unit(metadata_event)
                && tracked.metadata.int_value(metadata_virtual_node_id) == hash_string(port.virtual_node_id)
                && tracked.metadata.int_value(metadata_port_kind) == 1
                && tracked.metadata.int_value(metadata_port_ordinal) == static_cast<int>(port.port_ordinal)
                && tracked.metadata.int_value(metadata_node_bundle_port_ordinal)
                    == (port.node_bundle_port_ordinal.has_value()
                        ? std::optional<int>(static_cast<int>(*port.node_bundle_port_ordinal))
                        : std::nullopt);
        });
        if (graph_input_lane) {
            bindings.event_inputs.push_back(GraphInputLaneBinding{
                .port = port,
                .graph_input_lane = graph_input_lane,
            });
        }
    }

    return bindings;
}

void GraphInputLanes::handle_task_runner_after_pass(
    TasksRunnerAfterPass const &finished)
{
    std::vector<TimelineLaneBatchUpdate> timeline_batches;
    std::vector<std::string> binding_syncs;
    auto const update_version = finished.graph_revision + 1;
    {
        std::scoped_lock lock(mutex);
        current_update_version_index_ = update_version;
        timeline_batches = take_pending_timeline_batches_locked();
        binding_syncs.assign(
            pending_runtime_binding_syncs.begin(),
            pending_runtime_binding_syncs.end());
        pending_runtime_binding_syncs.clear();
    }
    for (auto const &batch : timeline_batches) {
        apply_timeline_batch(batch);
    }
    GraphInputLanesRuntimeDependenciesChanged dependencies{
        .version_index = update_version,
    };
    {
        std::scoped_lock lock(mutex);
        std::ranges::sort(binding_syncs);
        dependencies.instances.reserve(binding_syncs.size());
        for (auto const& instance_id : binding_syncs) {
            sync_runtime_bindings_locked(instance_id);
            dependencies.instances.push_back(GraphInputLanesRuntimeDependency{
                .instance_id = instance_id,
                .prerequisite_lanes =
                    prerequisite_lanes_for_instance_locked(instance_id),
            });
        }
    }
    if (!dependencies.instances.empty()) {
        IV_INVOKE_LINKER_EVENT(
            iv_runtime_graph_input_lanes_runtime_dependencies_changed_event,
            dependencies);
    }
    if (!timeline_batches.empty()) {
        emit_debug_message(
            "graph input lanes pass: revision="
            + std::to_string(finished.graph_revision)
            + " timelineBatches=" + std::to_string(timeline_batches.size()));
    }
}

void GraphInputLanes::handle_sample_block_published(
    LaneId lane,
    BorrowedSampleBlock const &block)
{
    publish_sample_output_block(lane, block);
}

void GraphInputLanes::handle_event_block_published(
    LaneId lane,
    std::span<TimedEvent const> events)
{
    publish_event_output_block(lane, events);
}

void GraphInputLanes::prepare_sample_output_block(LaneId lane)
{
    output_blocks_.prepare_sample(lane);
}

void GraphInputLanes::prepare_event_output_block(LaneId lane)
{
    output_blocks_.prepare_event(lane);
}

BorrowedSampleBlock GraphInputLanes::handle_sample_block_requested(LaneId lane) const
{
    return sample_output_block(lane);
}

std::span<TimedEvent const> GraphInputLanes::handle_event_block_requested(LaneId lane) const
{
    return event_output_block(lane);
}
} // namespace iv
