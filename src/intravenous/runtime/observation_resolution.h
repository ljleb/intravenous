#pragma once

#include <intravenous/runtime/graph_connections.h>
#include <intravenous/graph_jit/port_realization.h>
#include <intravenous/runtime/observation_types.h>
#include <intravenous/runtime/project_node_path_resolver.h>

#include <algorithm>

namespace iv {
inline bool resolve_observation_window_source(
    ConfiguredGraph const& graph,
    std::unordered_map<std::string, NodeInstancePlacement> const& placements,
    ObservationSource const& source, std::string_view operation_id,
    ObservationWindowQuery& query,
    graph_jit::ResolvedPortRealization const* realization = nullptr)
{
    if (source.kind != ObservationSourceKind::output) return false;
    auto channels = resolve_project_sample_output(graph, placements, source.node, source.member, source.channel);
    if (channels.size() != 1) return false;
    auto const selected = channels.front();
    auto const output = graph.node_bundles.resolve_sample_output(
        {selected.bundle, PortKind::sample, selected.port}).config;
    if (is_sequential_output(output) && output.realization != OutputRealization::buffered) return false;
    auto type = continuous_value_type_domain(output).sole_type();
    std::optional<graph_jit::ResolvedPortRealization> inferred;
    if (!type) {
        if (!realization || realization->bundle_count() < graph.node_bundles.size()) {
            auto resolved = graph_jit::resolve_port_realization(graph);
            if (!resolved) return false;
            inferred.emplace(std::move(*resolved));
            realization = &*inferred;
        }
        type = realization->output(selected.bundle, selected.port).value_type;
    }
    if (type != ContinuousValueTypeId::sample) return false;
    auto const& operations = continuous_value_type_info(*type).observation_operations;
    auto operation = std::ranges::find(operations, operation_id, &ObservationOperation::id);
    if (operation == operations.end() || !operation->capture_point || !operation->project) return false;
    query.output = {selected.bundle, PortKind::sample, selected.port};
    query.channel = selected.channel;
    query.operation = &*operation;
    return true;
}

inline CompiledObservationPlan resolve_observation_requirements(
    ConfiguredGraph const& graph,
    std::unordered_map<std::string, NodeInstancePlacement> const& placements,
    ObservationRequirements const& requirements,
    graph_jit::ResolvedPortRealization const* port_realization = nullptr)
{
    // Module interfaces declare allowed types. Resolve their actual type from
    // the graph's constraints; adding a meter must not choose a DSP value type.
    std::optional<graph_jit::ResolvedPortRealization> inferred_ports;
    if (port_realization && port_realization->bundle_count() < graph.node_bundles.size())
        port_realization = nullptr;
    bool attempted_resolution = false;
    auto value_type = [&](SampleOutputChannelId source, ContinuousOutputConfig const& output)
        -> std::optional<ContinuousValueTypeId> {
        if (auto sole = continuous_value_type_domain(output).sole_type()) return sole;
        if (!port_realization && !attempted_resolution) {
            attempted_resolution = true;
            auto resolved = graph_jit::resolve_port_realization(graph);
            if (resolved) {
                inferred_ports.emplace(std::move(*resolved));
                port_realization = &*inferred_ports;
            }
        }
        if (!port_realization) return std::nullopt;
        return port_realization->output(source.bundle, source.port).value_type;
    };
    CompiledObservationPlan result{.revision = requirements.revision};
    for (auto const& requirement : requirements.probes) {
        ResolvedObservationProbe probe{.id = requirement.id, .kind = requirement.source.kind};
        if (probe.kind == ObservationSourceKind::output) {
            auto channels = resolve_project_sample_output(graph, placements,
                requirement.source.node, requirement.source.member, requirement.source.channel);
            // Each result names one physical channel. Multichannel callers
            // use the explicit selectors advertised by capability discovery.
            if (channels.size() == 1) {
                auto const source = channels.front();
                auto const output = graph.node_bundles.resolve_sample_output(
                    {source.bundle, PortKind::sample, source.port}).config;
                if (is_sequential_output(output)
                    && value_type(source, output) == ContinuousValueTypeId::sample) {
                    auto const& type = continuous_value_type_info(ContinuousValueTypeId::sample);
                    auto operation = std::ranges::find(type.observation_operations,
                        requirement.operation, &ObservationOperation::id);
                    if (operation != type.observation_operations.end() && operation->capture_block) {
                        probe.operation = &*operation;
                        probe.bundle = source.bundle;
                        probe.port = source.port;
                        for (auto const& channel : channels) probe.channels.push_back(channel.channel);
                        probe.available = true;
                    }
                }
            }
        } else {
            auto matches = resolve_node_address(graph, placements, requirement.source.node);
            auto bundles = concrete_node_address_matches(graph, matches);
            if (bundles.size() == 1) {
                std::optional<NodeStateStructure> structure;
                std::size_t index = 0;
                graph.node_bundles.for_each_configured_bundle([&](ConfiguredNodeBundleView const& bundle) {
                    if (index++ != bundles.front()) return;
                    if (bundle.operations && bundle.operations->runtime.tick_block
                        && bundle.operations->runtime.state_structures)
                        structure = bundle.operations->runtime.state_structures->state;
                });
                if (structure) {
                    auto const& state = *structure;
                    auto field = std::ranges::find(state.fields, requirement.source.member, &NodeStateFieldStructure::name);
                    if (field != state.fields.end() && !field->bit_width && field->bit_offset % 8 == 0) {
                        for (auto const& codec : state_observation_codecs) {
                            if (field->type_name != codec.field_type_id || field->size_bits != codec.size_bits
                                || requirement.operation != codec.operation->id) continue;
                            probe.operation = codec.operation;
                            probe.bundle = bundles.front();
                            probe.state_field_offset = field->bit_offset / 8;
                            probe.state_field_size = field->size_bits / 8;
                            probe.available = true;
                        }
                    }
                }
            }
        }
        result.probes.push_back(probe);
    }
    return result;
}
} // namespace iv
