#pragma once

#include <intravenous/project/graph_connections.h>
#include <intravenous/graph_jit/graph_jit.h>
#include <intravenous/graph_jit/port_realization.h>
#include <intravenous/observations/observation_types.h>
#include <intravenous/project/project_node_path_resolver.h>

#include <algorithm>

namespace iv {
inline bool input_has_tick_binding(ConfiguredGraph const& graph, NodeBundleHandle handle)
{
    auto const& bundle = graph.node_bundles.bundle(handle);
    return bundle.is_subgraph() || (bundle.is_concrete()
        && graph.node_bundles.materialize_concrete_description(handle).operations.runtime.tick_block);
}

inline std::optional<graph_jit::BackgroundPortIndex> observation_input_window_port(
    CompiledGraph const& graph, NodeBundlePortId input)
{
    for (auto const& [source, view] : graph.resolved_sample_input_views)
        if (source == input) { input = view; break; }
    auto const& plan = graph.background_evaluation_plan;
    auto const port = std::ranges::find_if(plan.ports, [&](auto const& candidate) {
        return candidate.direction == graph_jit::PortDirection::input && candidate.configured_port == input;
    });
    if (port == plan.ports.end() || port->continuous_geometry.value_type != ContinuousValueTypeId::sample
        || (!port->random_access_input && !port->replay_sequential_input)) return std::nullopt;
    auto const index = static_cast<graph_jit::BackgroundPortIndex>(port - plan.ports.begin());
    if (index >= plan.runtime.port_bindings.size() || !plan.runtime.port_bindings[index]) return std::nullopt;
    return index;
}

inline graph_jit::PublicCoverageInputView const* observation_public_coverage_input(
    CompiledGraph const& graph, NodeBundlePortId input)
{
    auto const& views = graph.background_evaluation_plan.public_coverage_inputs;
    auto const found = std::ranges::find(views, input, &graph_jit::PublicCoverageInputView::port);
    return found == views.end() ? nullptr : &*found;
}

inline graph_jit::PublicEventCoverageInputView const* observation_public_event_coverage_input(
    CompiledGraph const& graph, NodeBundlePortId input)
{
    auto const& views = graph.background_evaluation_plan.public_event_coverage_inputs;
    auto const found = std::ranges::find(views, input, &graph_jit::PublicEventCoverageInputView::port);
    return found == views.end() ? nullptr : &*found;
}

template<class Channel>
inline std::optional<Channel> observation_channel(std::vector<Channel> const& channels,
    std::optional<std::size_t> index)
{
    if (index && *index < channels.size()) return channels[*index];
    if (!index && channels.size() == 1) return channels.front();
    return std::nullopt;
}

inline auto resolve_observation_channel(ConfiguredGraph const& graph,
    std::unordered_map<std::string, NodeInstancePlacement> const& placements,
    ObservationSource const& source, auto resolve)
{
    bool const group = !source.node.path.empty()
        && std::holds_alternative<ProjectSymbolicNodeSelector>(source.node.path.back());
    return observation_channel(resolve(graph, placements, source.node, source.member,
        group ? std::nullopt : source.channel), group ? source.channel : std::nullopt);
}

inline bool observes_symbolic_output(ObservationSource const& source)
{
    if (source.kind != ObservationSourceKind::output || source.node.path.empty()) return false;
    auto const* selector = std::get_if<ProjectSymbolicNodeSelector>(&source.node.path.back());
    return selector && selector->type_identity.has_value();
}

inline std::vector<SampleOutputRoute> resolve_symbolic_output_observation(
    ConfiguredGraph const& graph,
    std::unordered_map<std::string, NodeInstancePlacement> const& placements,
    ObservationSource const& source)
{
    auto routes = resolve_project_sample_output_routes(graph, placements, source.node, source.member, source.channel);
    if (!source.channel && std::ranges::any_of(routes, [&](auto const& route) {
            auto const channel = route.channel;
            return channel_count(graph.node_bundles.resolve_sample_output(
                {channel.bundle, PortKind::sample, channel.port}).config.channel_layout) != 1;
        })) return {};
    return routes;
}

inline bool observation_outputs_share_pace(std::span<SampleOutputChannelId const> sources,
    graph_jit::ResolvedPortRealization const* realization)
{
    if (sources.size() <= 1) return true;
    if (!realization) return false;
    auto const first = sources.front();
    auto const pace = realization->output(first.bundle, first.port).absolute_pace;
    return std::ranges::all_of(sources, [&](auto source) {
        return realization->output(source.bundle, source.port).absolute_pace == pace;
    });
}

inline std::optional<graph_jit::PortStorageIndex> recorded_event_window_storage(
    graph_jit::BackgroundEvaluationPlan const& plan, NodeBundlePortId output)
{
    auto const found = std::ranges::find_if(plan.storage.ports, [&](auto const& storage) {
        return storage.storage == graph_jit::PortStorageKind::persisted_data
            && storage.kind == PortKind::event && storage.output_port
            && plan.ports[*storage.output_port].configured_port == output
            && plan.ports[*storage.output_port].persisted_tick_output;
    });
    if (found == plan.storage.ports.end()) return std::nullopt;
    return static_cast<graph_jit::PortStorageIndex>(found - plan.storage.ports.begin());
}

inline bool resolve_observation_window_source(
    ConfiguredGraph const& graph,
    std::unordered_map<std::string, NodeInstancePlacement> const& placements,
    ObservationSource const& source, std::string_view operation_id,
    ObservationWindowQuery& query,
    graph_jit::ResolvedPortRealization const* realization = nullptr,
    CompiledGraph const* compiled = nullptr)
{
    if (source.port_kind == PortKind::event) {
        if (compiled && (operation_id == recorded_event_timestamps_operation || operation_id == recorded_trigger_bins_operation)
            && source.kind == ObservationSourceKind::output && !source.channel) {
            auto const matches = resolve_node_address(graph, placements, source.node);
            auto const bundles = concrete_node_address_matches(graph, matches);
            if (bundles.size() != 1 || !graph.node_bundles.bundle(bundles.front()).is_concrete()) return false;
            auto const bundle = bundles.front();
            for (std::size_t index = 0; index < graph.node_bundles.bundle(bundle).event_output_count(); ++index) {
                NodeBundlePortId const output{bundle, PortKind::event, index};
                auto const config = graph.node_bundles.resolve_event_output(output).config;
                if (config.name != source.member) continue;
                if (operation_id == recorded_trigger_bins_operation && config.type != EventTypeId::trigger) return false;
                if (!recorded_event_window_storage(compiled->background_evaluation_plan, output)) return false;
                query.port = output;
                query.kind = source.kind;
                return true;
            }
            return false;
        }
        if (!compiled || operation_id != "beat-grid" || source.kind != ObservationSourceKind::input) return false;
        std::optional<NodeBundlePortId> port;
        for (auto const& match : resolve_node_address(graph, placements, source.node)) {
            if (match.kind != ProjectNodePathMatch::Kind::bundle) continue;
            auto const& bundle = graph.node_bundles.bundle(match.bundle);
            for (std::size_t index = 0; index < bundle.event_input_count(); ++index) {
                NodeBundlePortId const candidate{match.bundle, PortKind::event, index};
                auto const config = graph.node_bundles.resolve_event_input(candidate).config;
                if (config.name != source.member || config.type != EventTypeId::beat) continue;
                if (port) return false;
                port = candidate;
            }
        }
        auto const* input = port ? observation_public_event_coverage_input(*compiled, *port) : nullptr;
        if (!input || !input->reconstructible) return false;
        query.port = *port;
        query.kind = source.kind;
        return true;
    }
    if (source.kind == ObservationSourceKind::input) {
        if (!compiled) return false;
        auto const channel = resolve_observation_channel(graph, placements, source, resolve_project_sample_input);
        if (!channel) return false;
        auto const selected = *channel;
        NodeBundlePortId const port{selected.bundle, PortKind::sample, selected.port};
        auto const* public_input = observation_public_coverage_input(*compiled, port);
        if (!observation_input_window_port(*compiled, port) && (!public_input || !public_input->reconstructible))
            return false;
        auto const& operations = continuous_value_type_info(ContinuousValueTypeId::sample).observation_operations;
        auto const operation = std::ranges::find(operations, operation_id, &ObservationOperation::id);
        if (operation == operations.end() || !operation->capture_point || !operation->project) return false;
        query.port = port;
        query.kind = source.kind;
        query.channel = selected.channel;
        query.operation = &*operation;
        return true;
    }
    if (source.kind != ObservationSourceKind::output) return false;
    auto const symbolic_output = observes_symbolic_output(source);
    std::vector<SampleOutputChannelId> sources;
    if (symbolic_output) {
        for (auto const& route : resolve_symbolic_output_observation(graph, placements, source))
            if (!std::ranges::contains(sources, route.channel)) sources.push_back(route.channel);
    } else if (auto const channel = resolve_observation_channel(graph, placements, source, resolve_project_sample_output))
        sources.push_back(*channel);
    std::optional<graph_jit::ResolvedPortRealization> inferred;
    if (sources.size() > 1 && (!realization || realization->bundle_count() < graph.node_bundles.size())) {
        auto resolved = graph_jit::resolve_port_realization(graph);
        if (!resolved) return false;
        inferred.emplace(std::move(*resolved));
        realization = &*inferred;
    }
    if (!observation_outputs_share_pace(sources, realization)) return false;
    if (sources.empty() || !std::ranges::all_of(sources, [&](auto selected) {
            auto const output = graph.node_bundles.resolve_sample_output(
                {selected.bundle, PortKind::sample, selected.port}).config;
            if (is_sequential_output(output) && output.realization != OutputRealization::buffered) return false;
            auto type = continuous_value_type_domain(output).sole_type();
            if (!type) {
                if (!realization || realization->bundle_count() < graph.node_bundles.size()) {
                    auto resolved = graph_jit::resolve_port_realization(graph);
                    if (!resolved) return false;
                    inferred.emplace(std::move(*resolved));
                    realization = &*inferred;
                }
                type = realization->output(selected.bundle, selected.port).value_type;
            }
            return type == ContinuousValueTypeId::sample;
        })) return false;
    auto const& operations = continuous_value_type_info(ContinuousValueTypeId::sample).observation_operations;
    auto operation = std::ranges::find(operations, operation_id, &ObservationOperation::id);
    if (operation == operations.end() || !operation->capture_point || !operation->project) return false;
    query.port = {sources.front().bundle, PortKind::sample, sources.front().port};
    query.kind = source.kind;
    query.channel = sources.front().channel;
    if (symbolic_output && sources.size() > 1) query.output_sources = std::move(sources);
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
    auto ensure_realization = [&] {
        if (port_realization || attempted_resolution) return;
        attempted_resolution = true;
        auto resolved = graph_jit::resolve_port_realization(graph);
        if (!resolved) return;
        inferred_ports.emplace(std::move(*resolved));
        port_realization = &*inferred_ports;
    };
    auto value_type = [&](SampleOutputChannelId source, ContinuousOutputConfig const& output)
        -> std::optional<ContinuousValueTypeId> {
        if (auto sole = continuous_value_type_domain(output).sole_type()) return sole;
        ensure_realization();
        if (!port_realization) return std::nullopt;
        return port_realization->output(source.bundle, source.port).value_type;
    };
    CompiledObservationPlan result{.revision = requirements.revision};
    for (auto const& requirement : requirements.probes) {
        ResolvedObservationProbe probe{.id = requirement.id, .kind = requirement.source.kind};
        if (observes_symbolic_output(requirement.source)) {
            auto routes = resolve_symbolic_output_observation(graph, placements, requirement.source);
            std::vector<SampleOutputChannelId> channels;
            for (auto const& route : routes) channels.push_back(route.channel);
            if (channels.size() > 1) ensure_realization();
            auto const& operations = continuous_value_type_info(ContinuousValueTypeId::sample).observation_operations;
            auto const operation = std::ranges::find(operations, requirement.operation, &ObservationOperation::id);
            if (!channels.empty() && observation_outputs_share_pace(channels, port_realization)
                && operation != operations.end() && operation->capture_block
                && std::ranges::all_of(channels, [&](auto source) {
                    auto const output = graph.node_bundles.resolve_sample_output(
                        {source.bundle, PortKind::sample, source.port}).config;
                    return is_sequential_output(output) && value_type(source, output) == ContinuousValueTypeId::sample;
                })) {
                probe.operation = &*operation;
                probe.channels = {0};
                probe.output_sources = std::move(routes);
                probe.available = true;
            }
        } else if (probe.kind == ObservationSourceKind::output) {
            auto const channel = resolve_observation_channel(
                graph, placements, requirement.source, resolve_project_sample_output);
            // Each result names one physical channel. Multichannel callers
            // use the explicit selectors advertised by capability discovery.
            if (channel) {
                auto const source = *channel;
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
                        probe.channels = {source.channel};
                        probe.available = true;
                    }
                }
            }
        } else if (probe.kind == ObservationSourceKind::input) {
            auto const channel = resolve_observation_channel(
                graph, placements, requirement.source, resolve_project_sample_input);
            if (channel) {
                auto const source = *channel;
                auto const input = graph.node_bundles.resolve_sample_input(
                    {source.bundle, PortKind::sample, source.port}).config;
                auto type = continuous_value_type_domain(input).sole_type();
                if (!type) ensure_realization();
                if (!type && port_realization)
                    type = port_realization->input(source.bundle, source.port).value_type;
                auto const& operations = continuous_value_type_info(ContinuousValueTypeId::sample).observation_operations;
                auto const operation = std::ranges::find(operations, requirement.operation, &ObservationOperation::id);
                if (type == ContinuousValueTypeId::sample && is_sequential(input)
                    && input_has_tick_binding(graph, source.bundle)
                    && operation != operations.end() && operation->capture_block) {
                    probe.operation = &*operation;
                    probe.bundle = source.bundle;
                    probe.port = source.port;
                    probe.channels = {source.channel};
                    probe.available = true;
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
