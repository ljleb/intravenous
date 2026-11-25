#include <intravenous/graph_jit/bundle_migration.h>
#include <intravenous/graph_jit/connection_plan.h>

#include <intravenous/graph_jit/background_storage_plan.h>
#include <intravenous/graph_jit/connections/port_group_plan.h>

#include <intravenous/channel_layout.h>
#include <intravenous/ports.h>
#include <intravenous/sample.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <exception>
#include <limits>
#include <iterator>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <ranges>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace iv::graph_jit::detail {

std::optional<std::size_t> port_migration_index(
    ConfiguredGraph const& graph, NodeBundlePortId port,
    PortDirection direction, std::string_view name)
{
    if (name.empty()) return port.port_index;
    auto const& bundles = graph.node_bundles;
    auto const& bundle = bundles.bundle(port.node_bundle_handle);
    auto const boundary = bundle.subgraph_boundary_handle();
    auto const& ports = boundary ? bundles.bundle(*boundary).boundary_ports()
        : bundle.is_boundary() ? bundle.boundary_ports()
        : bundles.typed_ports(port.node_bundle_handle);
    auto const same_name_and_kind = [&](auto const& config) {
        return config.name == name && is_continuous(config) == (port.port_kind == PortKind::sample);
    };
    auto const unique = direction == PortDirection::input
        ? std::ranges::count_if(ports.inputs(), same_name_and_kind) == 1
        : std::ranges::count_if(ports.outputs(), same_name_and_kind) == 1;
    return unique ? std::nullopt : std::optional(port.port_index);
}

bool reads_output_gain_source_indices(ConfiguredGraph const& graph, NodeBundlePortId input)
{
    auto const& node = graph.node_bundles.bundle(input.node_bundle_handle);
    return node.output_gain_source() && input.port_kind == PortKind::sample
        && input.port_index == 0 && is_random_access(node.sample_input_descriptor(0).config);
}

PlannedSourceProduction source_production(
    ConnectionAnalysisPlan const& plan, NodeBundleHandle bundle) noexcept
{
    auto const node = std::ranges::find(plan.nodes, bundle, &PlannedGraphNode::bundle);
    if (node == plan.nodes.end()) {
        IV_ASSERT(bundle == plan.boundary_bundle, "source output has no callback-domain metadata");
        return PlannedSourceProduction::tick;
    }
    return node->tick_execution ? PlannedSourceProduction::tick : PlannedSourceProduction::tock;
}

PlannedDestinationAccess destination_access(
    InputAccessConfig const& access) noexcept
{
    return is_sequential(access)
        ? PlannedDestinationAccess::sequential
        : PlannedDestinationAccess::random_access;
}

std::vector<std::vector<std::size_t>> dependency_adjacency(
    ConfiguredGraph const& graph,
    ConnectionAnalysisPlan const& plan,
    bool sequential_only)
{
    std::vector<std::optional<std::size_t>> bundle_to_node(
        graph.node_bundles.size());
    for (std::size_t i = 0; i < plan.nodes.size(); ++i) {
        bundle_to_node[plan.nodes[i].bundle] = i;
    }

    std::vector<std::vector<std::size_t>> outgoing(plan.nodes.size());
    for (auto const& dependency : plan.dependencies) {
        if (sequential_only && !dependency.sequential_tick_dependency) continue;
        if (dependency.source_bundle >= bundle_to_node.size()
            || dependency.target_bundle >= bundle_to_node.size()
            || !bundle_to_node[dependency.source_bundle]
            || !bundle_to_node[dependency.target_bundle]) {
            continue;
        }
        outgoing[*bundle_to_node[dependency.source_bundle]].push_back(
            *bundle_to_node[dependency.target_bundle]);
    }
    for (auto& targets : outgoing) {
        std::ranges::sort(targets);
        targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    }
    return outgoing;
}

namespace {

std::size_t saturating_add(std::size_t a, std::size_t b) noexcept
{
    auto const max = std::numeric_limits<std::size_t>::max();
    return b > max - a ? max : a + b;
}

bool is_internal_bundle(
    NodeBundleHandle bundle,
    NodeBundleHandle boundary) noexcept
{
    return bundle != boundary;
}

std::expected<PlannedDeliveryMechanism, std::string> connection_delivery(
    PlannedSourceProduction production,
    OutputRealization realization,
    PlannedDestinationAccess access,
    bool source_intrinsically_replayable,
    std::string_view data,
    std::size_t connection_index)
{
    if (production == PlannedSourceProduction::tick
        && access == PlannedDestinationAccess::sequential) {
        return PlannedDeliveryMechanism::tick_to_sequential;
    }
    if (production == PlannedSourceProduction::tock
        && access == PlannedDestinationAccess::sequential) {
        return PlannedDeliveryMechanism::tock_to_sequential;
    }
    if (production == PlannedSourceProduction::tock) {
        return PlannedDeliveryMechanism::tock_to_random_access;
    }
    if (realization == OutputRealization::buffered) {
        return PlannedDeliveryMechanism::persisted_tick_to_random_access;
    }
    if (source_intrinsically_replayable) {
        return PlannedDeliveryMechanism::replayed_tick_to_random_access;
    }
    return std::unexpected(
        "GraphJit " + std::string(data) + " connection "
        + std::to_string(connection_index)
        + " requires random access to an unreproducible Tick/ephemeral source; "
          "an explicit recorder is required");
}

PlannedGraphNode const* planned_node_for_bundle(
    ConnectionAnalysisPlan const& plan, NodeBundleHandle bundle) noexcept
{
    auto const found = std::ranges::find_if(
        plan.nodes,
        [&](PlannedGraphNode const& node) { return node.bundle == bundle; });
    return found == plan.nodes.end() ? nullptr : std::addressof(*found);
}

PlannedGraphNode* planned_node_for_bundle(
    ConnectionAnalysisPlan& plan, NodeBundleHandle bundle) noexcept
{
    auto const found = std::ranges::find_if(
        plan.nodes,
        [&](PlannedGraphNode const& node) { return node.bundle == bundle; });
    return found == plan.nodes.end() ? nullptr : std::addressof(*found);
}

bool source_is_intrinsically_replayable(
    ConnectionAnalysisPlan const& plan, NodeBundleHandle bundle) noexcept
{
    auto const* node = planned_node_for_bundle(plan, bundle);
    return node && node->replay_mode != TickReplayMode::disabled;
}

void append_realtime_port_state(
    RealtimePortStateRequirements& requirements,
    NodeBundlePortId configured_port,
    PortDirection direction,
    std::string const& port_name,
    std::optional<std::size_t> port_index,
    std::optional<std::size_t> channel,
    RealtimePortStateRole role,
    std::size_t extent_samples,
    std::optional<StableConcreteNodeId> const& stable_node,
    std::size_t in_flight_samples = 0,
    bool keep_empty = false)
{
    if (extent_samples == 0 && in_flight_samples == 0 && !keep_empty) return;

    std::optional<StableRealtimePortStateId> stable_identity;
    if (stable_node) {
        stable_identity = StableRealtimePortStateId{
            .node = *stable_node,
            .direction = direction,
            .kind = configured_port.port_kind,
            .port_name = port_name,
            .port_index = port_index,
            .channel = channel,
            .role = role,
        };
    }
    requirements.states.push_back(RealtimePortStateRequirement{
        .configured_port = configured_port,
        .direction = direction,
        .channel = channel,
        .role = role,
        .extent_samples = extent_samples,
        .in_flight_samples = in_flight_samples,
        .stable_identity = std::move(stable_identity),
    });
}

void inventory_realtime_port_states(
    ConfiguredGraph const& graph,
    NodePorts const& ports,
    NodeBundleHandle bundle,
    ResolvedPortRealization const& realization,
    std::optional<StableConcreteNodeId> const& stable_node,
    RealtimePortStateRequirements& requirements)
{
    auto append_sample_states = [&](NodeBundlePortId port,
                                    PortDirection direction,
                                    std::string const& name,
                                    ChannelLayout layout,
                                    RealtimePortStateRole role,
                                    std::size_t extent) {
        if (extent == 0) return;
        auto const identity_index = stable_node
            ? port_migration_index(graph, port, direction, name) : std::nullopt;
        for (std::size_t channel = 0; channel < channel_count(layout); ++channel) {
            append_realtime_port_state(
                requirements,
                port,
                direction,
                name,
                identity_index,
                channel,
                role,
                extent,
                stable_node);
        }
    };

    auto const sample_inputs = ports.sample_inputs();
    for (std::size_t index = 0; index < sample_inputs.size(); ++index) {
        auto const& input = sample_inputs[index];
        if (!is_sequential(input)) continue;
        append_sample_states(
            NodeBundlePortId{bundle, PortKind::sample, index},
            PortDirection::input,
            input.name,
            input.channel_layout,
            RealtimePortStateRole::sequential_input_history,
            realization.input(bundle, index).history);
    }

    auto const event_inputs = ports.event_inputs();
    for (std::size_t index = 0; index < event_inputs.size(); ++index) {
        auto const& input = event_inputs[index];
        if (!is_sequential(input)) continue;
        auto const extent = realization.event_input(bundle, index).history;
        if (extent == 0) continue;
        auto const port = NodeBundlePortId{bundle, PortKind::event, index};
        append_realtime_port_state(
            requirements,
            port,
            PortDirection::input,
            input.name,
            stable_node ? port_migration_index(graph, port, PortDirection::input, input.name) : std::nullopt,
            std::nullopt,
            RealtimePortStateRole::sequential_input_history,
            extent,
            stable_node);
    }

    // Sample storage files each value at the index it was authored for, so a Tick
    // sample output owns one window: its last history + latency values before its
    // next tick. Latency only marks the newest of them as still revisable through
    // OutputPort::update(); it places nothing at or after the next tick. One
    // record keeps that window whole when a successor changes history or latency.
    auto const sample_outputs = ports.sample_outputs();
    for (std::size_t index = 0; index < sample_outputs.size(); ++index) {
        auto const& output = sample_outputs[index];
        if (!is_sequential_output(output)) continue;
        auto const& resolved = realization.output(bundle, index);
        append_sample_states(
            NodeBundlePortId{bundle, PortKind::sample, index},
            PortDirection::output,
            output.name,
            output.channel_layout,
            RealtimePortStateRole::tick_output_history,
            saturating_add(resolved.history, resolved.latency));
    }

    // A Tick event output owns the events at its last history + latency
    // positions before its next tick, as a sample output owns its values there:
    // one record for its stream.
    auto const event_outputs = ports.event_outputs();
    for (std::size_t index = 0; index < event_outputs.size(); ++index) {
        auto const& output = event_outputs[index];
        if (!is_sequential_output(output)) continue;
        auto const& resolved = realization.event_output(bundle, index);
        auto const extent = saturating_add(resolved.history, resolved.latency);
        if (extent == 0) continue;
        auto const port = NodeBundlePortId{bundle, PortKind::event, index};
        append_realtime_port_state(
            requirements,
            port,
            PortDirection::output,
            output.name,
            stable_node ? port_migration_index(graph, port, PortDirection::output, output.name) : std::nullopt,
            std::nullopt,
            RealtimePortStateRole::tick_output_history,
            extent,
            stable_node);
    }
}

std::expected<void, std::string> inventory_nodes(
    ConfiguredGraph const& graph,
    ResolvedPortRealization const& realization,
    ConnectionAnalysisPlan& plan,
    ConfiguredBundleInventory configured_bundles,
    std::span<std::optional<StableConcreteNodeId> const> identities)
{
    plan.boundary_bundle = graph.public_ports.boundary_handle();
    // A default-constructed ConfiguredGraph is the shell's semantic empty
    // identity and intentionally has no materialized boundary bundle.
    if (graph.node_bundles.size() == 0) return {};

    auto identity_for = [&](NodeBundleHandle bundle) -> std::optional<StableConcreteNodeId> {
        return bundle < identities.size() ? identities[bundle] : std::nullopt;
    };
    auto identity_for_port_node = [&](NodeBundleHandle generated, NodeBundlePortId port,
                                      PortDirection direction, std::string_view name) -> std::optional<StableConcreteNodeId> {
        auto const owner = identity_for(port.node_bundle_handle);
        if (!owner) return std::nullopt;
        return StableConcreteNodeId{
            .graph = std::string(graph.node_bundles.bundle(generated).type_identity()) + ':'
                + migration_identity_fields(*owner),
            .symbolic_node = migration_port_identity_fields(name,
                port_migration_index(graph, port, direction, name)),
        };
    };
    bool root_boundary_seen = false;
    std::size_t bundle = 0;
    std::string error;
    std::unordered_map<NodeBundleHandle, NodeBundlePortId> coverage_control_inputs;
    if (configured_bundles.snapshot) {
        for (auto const& control : graph.manual_controls)
            if (control.coverage_source) coverage_control_inputs.emplace(*control.coverage_source, control.input);
    }
    graph.node_bundles.for_each_configured_bundle(
        [&](ConfiguredNodeBundleView const& view) {
            auto const current = bundle++;
            if (!error.empty()) return;

            if (view.kind == ConfiguredNodeBundleKind::boundary) {
                root_boundary_seen = root_boundary_seen
                    || current == plan.boundary_bundle;
                return;
            }
            if (view.kind == ConfiguredNodeBundleKind::tiled
                || view.kind == ConfiguredNodeBundleKind::subgraph) {
                return;
            }
            if (!view.ports) {
                error = "concrete bundle has no port metadata during connection analysis";
                return;
            }
            if (!view.operations || !view.operations->valid()) {
                error = "concrete bundle has no valid node operations during connection analysis";
                return;
            }
            auto const has_tick_callback = view.operations->runtime.tick_block != nullptr;
            if (view.replay_mode != TickReplayMode::disabled
                && (!has_tick_callback
                    || view.operations->runtime.has_propagate_forward_coverage
                    || view.operations->runtime.has_propagate_reverse_coverage
                    || std::ranges::any_of(view.ports->sample_inputs(), [](auto const& config) {
                        return is_random_access(config);
                    }) || std::ranges::any_of(view.ports->event_inputs(), [](auto const& config) {
                        return is_random_access(config);
                    }))) {
                error = "GraphJit Tick replay requires pointwise Sequential inputs and generated coverage";
                return;
            }
            if (has_tick_callback && (std::ranges::any_of(view.ports->sample_inputs(), [](auto const& config) {
                    return is_random_access(config) && config.content_access != InputContentAccess::complete_buffer;
                }) || std::ranges::any_of(view.ports->event_inputs(), [](auto const& config) {
                    return is_random_access(config) && config.content_access != InputContentAccess::complete_buffer;
                }))) {
                error = "Tick Region/Coverage inputs initially require complete-buffer access";
                return;
            }
            auto const has_sequential_output = std::ranges::any_of(
                    view.ports->sample_outputs(),
                    [](auto const& output) { return is_sequential_output(output); })
                || std::ranges::any_of(view.ports->event_outputs(),
                    [](auto const& output) { return is_sequential_output(output); });
            auto const has_region_or_coverage_output = std::ranges::any_of(
                    view.ports->sample_outputs(),
                    [](auto const& output) { return is_random_access_output(output); })
                || std::ranges::any_of(view.ports->event_outputs(),
                    [](auto const& output) { return is_random_access_output(output); });
            if ((has_tick_callback && has_region_or_coverage_output)
                || (!has_tick_callback
                    && (has_sequential_output || !has_region_or_coverage_output))) {
                error = "Tick primitives declare only Sequential outputs; "
                    "Tock primitives declare only Region or Coverage outputs";
                return;
            }
            auto stable_identity = identity_for(current);
            if (auto control = coverage_control_inputs.find(current); control != coverage_control_inputs.end()) {
                auto const input = control->second;
                stable_identity = identity_for_port_node(current, input, PortDirection::input,
                    graph.node_bundles.resolve_sample_input(input).config.name);
            }
            if (view.output_gain_source && !stable_identity) {
                auto const source = *view.output_gain_source;
                if (auto derived = identity_for_port_node(current, source, PortDirection::output,
                        graph.node_bundles.resolve_sample_output(source).config.name))
                    stable_identity = std::move(derived);
            }
            std::optional<StableInputPortId> module_input;
            if (view.resolved_module_input) {
                auto const origin = *view.resolved_module_input;
                auto owner = identity_for(origin.node_bundle_handle);
                auto const name = origin.port_kind == PortKind::sample
                    ? view.ports->sample_input(0).name : view.ports->event_input(0).name;
                if (owner) module_input = StableInputPortId{
                    .node = std::move(*owner), .kind = origin.port_kind,
                    .port_name = name,
                    .port_index = port_migration_index(graph, origin, PortDirection::input, name),
                };
                if (module_input && configured_bundles.snapshot) {
                    stable_identity = StableConcreteNodeId{
                        .graph = "compiler.module_input:" + migration_identity_fields(module_input->node),
                        .symbolic_node = (module_input->kind == PortKind::sample ? "sample" : "event")
                            + migration_port_identity_fields(module_input->port_name, module_input->port_index),
                    };
                }
            }
            plan.nodes.push_back(PlannedGraphNode{
                .bundle = current,
                .stable_identity = stable_identity,
                .resolved_module_input = module_input,
                .internal_latency_samples = view.internal_latency_samples,
                .maximum_block_size = view.maximum_block_size,
                .sample_input_count = view.ports->sample_input_count(),
                .sample_output_count = view.ports->sample_output_count(),
                .event_input_count = view.ports->event_input_count(),
                .event_output_count = view.ports->event_output_count(),
                .tick_execution = has_tick_callback,
                .replay_mode = view.replay_mode,
                .authored_reverse_coverage = !has_tick_callback
                    && view.operations->runtime.has_propagate_reverse_coverage,
            });
            auto const first_state = plan.realtime_port_states.states.size();
            inventory_realtime_port_states(
                graph,
                *view.ports,
                current,
                realization,
                module_input ? std::optional(module_input->node) : stable_identity,
                plan.realtime_port_states);
            if (module_input) {
                for (std::size_t state = first_state; state < plan.realtime_port_states.states.size(); ++state) {
                    auto& requirement = plan.realtime_port_states.states[state];
                    requirement.resolved_module_input = module_input;
                    if (requirement.stable_identity) {
                        requirement.stable_identity->port_name = module_input->port_name;
                        requirement.stable_identity->port_index = module_input->port_index;
                    }
                }
            }
        });

    if (!error.empty()) return std::unexpected(std::move(error));
    if (!root_boundary_seen
        || plan.boundary_bundle >= graph.node_bundles.size()) {
        return std::unexpected(
            "connection analysis requires exactly one project boundary");
    }
    return {};
}

void append_dependency(
    ConnectionAnalysisPlan& plan,
    NodeBundleHandle source,
    NodeBundleHandle target,
    PlannedConnectionData data,
    PlannedSourceProduction production,
    OutputRealization realization,
    PlannedDestinationAccess access,
    PlannedDeliveryMechanism delivery,
    std::size_t connection_index)
{
    if (!is_internal_bundle(source, plan.boundary_bundle)
        || !is_internal_bundle(target, plan.boundary_bundle)) {
        return;
    }
    auto const duplicate = std::ranges::find_if(
        plan.dependencies,
        [&](DependencyEdgePlan const& edge) {
            return edge.source_bundle == source && edge.target_bundle == target
                && edge.data == data
                && edge.source_production == production
                && edge.source_realization == realization
                && edge.destination_access == access
                && edge.delivery == delivery
                && edge.configured_connection_index == connection_index;
        });
    if (duplicate == plan.dependencies.end()) {
        plan.dependencies.push_back(DependencyEdgePlan{
            .source_bundle = source,
            .target_bundle = target,
            .data = data,
            .source_production = production,
            .source_realization = realization,
            .destination_access = access,
            .delivery = delivery,
            .configured_connection_index = connection_index,
            .sequential_tick_dependency =
                delivery == PlannedDeliveryMechanism::tick_to_sequential,
        });
    }
}

struct RoutedSampleConnection {
    ConfiguredSampleConnection connection{};
    SampleChannelTransformPlan transform{};
    struct Contribution {
        std::vector<float> source_coefficients{};
        std::vector<float> target_coefficients{};
        std::optional<ConfiguredSampleConnectionDetach> detach{};
    };
    std::vector<Contribution> contributions{};
    std::size_t authored_connection_index = 0;
};

struct ResolvedSampleContribution {
    std::vector<std::pair<SampleOutputChannelId, float>> source_terms{};
    std::optional<ConfiguredSampleConnectionDetach> detach{};
    float output_coefficient = 1.0f;
};

struct ResolvedSampleChannel {
    std::vector<ResolvedSampleContribution> contributions{};
    bool crossed_structure = false;
};

void add_sample_source_term(
    std::vector<std::pair<SampleOutputChannelId, float>>& terms,
    SampleOutputChannelId source,
    float coefficient)
{
    if (coefficient == 0.0f) return;
    auto const found = std::ranges::find_if(
        terms, [&](auto const& term) { return term.first == source; });
    if (found == terms.end()) {
        terms.emplace_back(source, coefficient);
    } else {
        found->second += coefficient;
        if (found->second == 0.0f) terms.erase(found);
    }
}

std::expected<ResolvedSampleChannel, std::string> detach_sample_channel(
    ResolvedSampleChannel channel,
    ConfiguredSampleConnectionDetach detach)
{
    if (std::ranges::any_of(
            channel.contributions,
            [](ResolvedSampleContribution const& contribution) {
                return contribution.detach.has_value();
            })) {
        return std::unexpected(
            "GraphJit structural sample route contains two serial detach operations");
    }
    ResolvedSampleContribution detached{.detach = std::move(detach)};
    for (auto const& contribution : channel.contributions) {
        for (auto const& [source, coefficient] : contribution.source_terms) {
            add_sample_source_term(
                detached.source_terms,
                source,
                coefficient * contribution.output_coefficient);
        }
    }
    channel.contributions.clear();
    channel.contributions.push_back(std::move(detached));
    return channel;
}

void add_sample_channel(
    ResolvedSampleChannel& target,
    ResolvedSampleChannel const& source,
    float coefficient)
{
    if (coefficient == 0.0f) return;
    for (auto const& contribution : source.contributions) {
        if (!contribution.detach) {
            auto found = std::ranges::find_if(
                target.contributions,
                [](ResolvedSampleContribution const& candidate) {
                    return !candidate.detach;
                });
            if (found == target.contributions.end()) {
                target.contributions.push_back({});
                found = std::prev(target.contributions.end());
            }
            for (auto const& [base, base_coefficient] :
                 contribution.source_terms) {
                add_sample_source_term(
                    found->source_terms,
                    base,
                    coefficient * contribution.output_coefficient
                        * base_coefficient);
            }
            continue;
        }
        auto detached = contribution;
        detached.output_coefficient *= coefficient;
        if (detached.output_coefficient != 0.0f)
            target.contributions.push_back(std::move(detached));
    }
}

std::expected<ChannelTypeId, std::string> channel_type_with_count(
    std::size_t count)
{
    for (std::size_t index = 0;
         index < static_cast<std::size_t>(ChannelTypeId::count);
         ++index) {
        auto const type = static_cast<ChannelTypeId>(index);
        if (channel_count(type) == count) return type;
    }
    return std::unexpected(
        "GraphJit structural sample route has no registered channel type for "
        + std::to_string(count) + " channels");
}

std::optional<NodeBundleHandle> coverage_manual_source(ConfiguredGraph const& graph, NodeBundleHandle source)
{
    auto const control = std::ranges::find(graph.manual_controls, std::optional{source},
        &ConfiguredManualControl::coverage_source);
    return control == graph.manual_controls.end() ? std::nullopt : control->coverage_source;
}

enum class ConnectionView { authored, effective };

bool overrides_connection(ConfiguredManualControl const& control, ConnectionView view)
{
    return view == ConnectionView::effective && control.override_connections;
}

struct StructuralSampleRouteIndex {
    struct IncomingSample {
        ConfiguredSampleConnection const* connection;
        std::size_t target_channel;
    };

    std::vector<std::optional<NodeBundleHandle>> boundary_owner;
    std::map<std::pair<NodeBundleHandle, std::size_t>, std::size_t> boundary_controls;
    std::map<std::tuple<NodeBundleHandle, std::size_t, std::size_t>, std::vector<IncomingSample>> incoming_by_channel;
    std::vector<std::vector<bool>> connected_module_inputs;
};

std::expected<SampleInputChannelId, std::string> resolve_structural_sample_target(
    ConfiguredGraph const& graph, SampleInputChannelId target)
{
    if (target.bundle >= graph.node_bundles.size()) {
        return std::unexpected(
            "GraphJit structural sample route references a missing target bundle");
    }
    auto const& bundle = graph.node_bundles.bundle(target.bundle);
    if (!bundle.is_tiled()) return target;
    auto const members = bundle.tiled_members();
    if (target.channel >= members.size()) {
        return std::unexpected(
            "GraphJit tiled sample target channel is outside its member set");
    }
    return resolve_structural_sample_target(graph, SampleInputChannelId{
        .bundle = members[target.channel],
        .port = target.port,
        .channel = 0,
    });
}

std::expected<StructuralSampleRouteIndex, std::string> index_structural_sample_routes(
    ConfiguredGraph const& graph)
try {
    StructuralSampleRouteIndex index;
    index.boundary_owner.resize(graph.node_bundles.size());
    for (NodeBundleHandle bundle = 0;
         bundle < graph.node_bundles.size();
         ++bundle) {
        auto const& candidate = graph.node_bundles.bundle(bundle);
        if (!candidate.is_subgraph()) continue;
        auto const boundary = candidate.subgraph_boundary_handle();
        if (!boundary || *boundary >= index.boundary_owner.size()) {
            return std::unexpected(
                "GraphJit structural routing found an invalid subgraph boundary");
        }
        if (index.boundary_owner[*boundary]) {
            return std::unexpected(
                "GraphJit structural routing found a boundary with two owners");
        }
        index.boundary_owner[*boundary] = bundle;
    }

    for (std::size_t control_index = 0; control_index < graph.manual_controls.size(); ++control_index) {
        auto const& control = graph.manual_controls[control_index];
        if (control.coverage_source) continue;
        if (!graph.node_bundles.bundle(control.input.node_bundle_handle).is_subgraph()) continue;
        if (!index.boundary_controls.emplace(std::pair{control.input.node_bundle_handle, control.input.port_index}, control_index).second)
            return std::unexpected("GraphJit manual controls contain two values for one module input");
    }
    index.connected_module_inputs.resize(graph.node_bundles.size());
    for (auto const& connection : graph.connections.configured_sample_connections()) {
        auto const source_width = channel_count(connection.source_type);
        if (source_width == 0 || connection.source_channels.empty()
            || connection.source_channels.size() % source_width != 0) {
            return std::unexpected("GraphJit sample connection has an incomplete source layout");
        }
        for (std::size_t target_index = 0; target_index < connection.target_channels.size(); ++target_index) {
            auto channel = resolve_structural_sample_target(graph, connection.target_channels[target_index]);
            if (!channel) return std::unexpected(std::move(channel.error()));
            if (graph.node_bundles.bundle(channel->bundle).is_concrete()) continue;
            auto& ports = index.connected_module_inputs[channel->bundle];
            if (ports.size() <= channel->port) ports.resize(channel->port + 1);
            ports[channel->port] = true;
            index.incoming_by_channel[{channel->bundle, channel->port, channel->channel}]
                .push_back({&connection, target_index});
        }
    }
    return index;
} catch (std::exception const& error) {
    return std::unexpected("GraphJit structural sample routing failed: " + std::string(error.what()));
}

std::expected<std::vector<RoutedSampleConnection>, std::string>
build_structural_sample_routes(ConfiguredGraph const& graph,
    StructuralSampleRouteIndex const& routing,
    std::vector<ManualSampleRoutePlan>& manual_routes,
    ConnectionView view,
    bool public_coverage_inputs = false)
{
    auto const authored = graph.connections.configured_sample_connections();
    auto const root_boundary = graph.public_ports.boundary_handle();
    auto is_coverage_source = [](ConfiguredSampleConnection const& connection, ConfiguredManualControl const& control) {
        return control.coverage_source && std::ranges::all_of(connection.source_channels, [&](auto const& source) {
            return source.channel.bundle == *control.coverage_source;
        });
    };
    auto replaces_source = [&](SampleInputChannelId target, ConfiguredSampleConnection const& connection) {
        if (connection.source_owned) return false;
        return std::ranges::any_of(graph.manual_controls, [&](auto const& control) {
            return overrides_connection(control, view)
                && control.input.node_bundle_handle == target.bundle && control.input.port_index == target.port
                && !is_coverage_source(connection, control);
        });
    };

    auto is_terminal_target = [&](SampleInputChannelId target) {
        auto const& bundle = graph.node_bundles.bundle(target.bundle);
        if (public_coverage_inputs)
            return bundle.is_subgraph() && input_access(graph.node_bundles.resolve_sample_input(
                {target.bundle, PortKind::sample, target.port}).config) == InputAccessKind::coverage;
        return bundle.is_concrete() || target.bundle == root_boundary;
    };

    auto const& [boundary_owner, boundary_controls, incoming_by_channel, connected_module_inputs] = routing;
    auto manual_source = [&](SampleOutputChannelId source) -> std::optional<std::size_t> {
        if (source.bundle >= boundary_owner.size() || !boundary_owner[source.bundle]) return std::nullopt;
        auto found = boundary_controls.find({*boundary_owner[source.bundle], source.port});
        return found == boundary_controls.end() ? std::nullopt : std::optional(found->second);
    };

    std::vector<SampleOutputChannelId> resolving;
    auto resolve_output = [&](auto&& self, SampleOutputChannelId output)
        -> std::expected<ResolvedSampleChannel, std::string> {
        if (output.bundle >= graph.node_bundles.size()) {
            return std::unexpected(
                "GraphJit structural sample route references a missing source bundle");
        }
        if (std::ranges::contains(resolving, output)) {
            return std::unexpected(
                "GraphJit structural sample routing reached a cycle before a detach");
        }
        resolving.push_back(output);
        auto pop = [&] { resolving.pop_back(); };

        auto const& bundle = graph.node_bundles.bundle(output.bundle);
        if (bundle.is_concrete() || output.bundle == root_boundary) {
            pop();
            return ResolvedSampleChannel{
                .contributions = {ResolvedSampleContribution{
                    .source_terms = {{output, 1.0f}},
                }},
                .crossed_structure = false,
            };
        }
        if (bundle.is_tiled()) {
            auto const members = bundle.tiled_members();
            if (output.channel >= members.size()) {
                pop();
                return std::unexpected(
                    "GraphJit tiled sample source channel is outside its member set");
            }
            auto resolved = self(self, SampleOutputChannelId{
                .bundle = members[output.channel],
                .port = output.port,
                .channel = 0,
            });
            pop();
            if (resolved) resolved->crossed_structure = true;
            return resolved;
        }

        SampleInputChannelId upstream_target{};
        if (bundle.is_subgraph()) {
            auto const boundary = bundle.subgraph_boundary_handle();
            if (!boundary) {
                pop();
                return std::unexpected(
                    "GraphJit subgraph sample source lost its boundary");
            }
            upstream_target = SampleInputChannelId{
                .bundle = *boundary,
                .port = output.port,
                .channel = output.channel,
            };
        } else if (bundle.is_boundary()
                   && output.bundle != root_boundary) {
            if (output.bundle >= boundary_owner.size()
                || !boundary_owner[output.bundle]) {
                pop();
                return std::unexpected(
                    "GraphJit internal sample boundary has no owning subgraph");
            }
            upstream_target = SampleInputChannelId{
                .bundle = *boundary_owner[output.bundle],
                .port = output.port,
                .channel = output.channel,
            };
        } else {
            pop();
            return std::unexpected(
                "GraphJit structural sample source has no executable producer");
        }

        auto const found = incoming_by_channel.find({upstream_target.bundle, upstream_target.port, upstream_target.channel});
        auto const& connected_ports = connected_module_inputs[upstream_target.bundle];
        auto const connected = upstream_target.port < connected_ports.size() && connected_ports[upstream_target.port];
        ResolvedSampleChannel result;
        result.crossed_structure = true;
        auto const control_index = manual_source(output);
        if (control_index && (overrides_connection(graph.manual_controls[*control_index], view)
            || !connected
            || graph.manual_controls[*control_index].participation == ManualControlParticipation::participate_with_connections)) {
            result.contributions.push_back({.source_terms = {{output, 1.0f}}});
        }
        if (found == incoming_by_channel.end()) {
            pop();
            if (!result.contributions.empty()) return result;
            // A module can expose an optional Coverage input. Its internal
            // consumers receive no Regions when the public input has no source.
            if (bundle.is_boundary() && input_access(graph.node_bundles.resolve_sample_input(
                    {upstream_target.bundle, PortKind::sample, upstream_target.port}).config)
                    == InputAccessKind::coverage)
                return result;
            return std::unexpected("GraphJit structural sample boundary route has no source");
        }
        for (auto const& [connection, target_channel] : found->second) {
            if (replaces_source(upstream_target, *connection)) continue;
            auto const source_width = channel_count(connection->source_type);
            for (std::size_t source_channel = 0;
                 source_channel < connection->source_channels.size();
                 ++source_channel) {
                auto resolved = self(
                    self, connection->source_channels[source_channel].channel);
                if (!resolved) {
                    pop();
                    return std::unexpected(std::move(resolved.error()));
                }
                result.crossed_structure = result.crossed_structure
                    || resolved->crossed_structure;
                if (connection->source_channels[source_channel].detach) {
                    resolved = detach_sample_channel(
                        std::move(*resolved),
                        *connection->source_channels[source_channel].detach);
                    if (!resolved) {
                        pop();
                        return std::unexpected(std::move(resolved.error()));
                    }
                }
                auto const coefficient = channel_conversion_coefficient(
                    connection->source_type,
                    connection->target_type,
                    source_channel % source_width,
                    target_channel);
                if (coefficient == 0.0f) continue;
                add_sample_channel(result, *resolved, coefficient);
            }
        }
        pop();
        return result;
    };

    std::vector<RoutedSampleConnection> routes;
    for (std::size_t connection_index = 0;
         connection_index < authored.size();
         ++connection_index) {
        auto const& connection = authored[connection_index];
        std::vector<std::vector<SampleInputChannelId>> expanded_targets;
        expanded_targets.reserve(connection.target_channels.size());
        bool has_terminal_target = false;
        bool target_crossed_structure = false;
        for (auto const target : connection.target_channels) {
            auto resolved = resolve_structural_sample_target(graph, target);
            if (!resolved) return std::unexpected(std::move(resolved.error()));
            std::vector expanded{*resolved};
            // Mirror the connected source route before it reaches the
            // public input. The fallback sees coverage metadata without
            // replacing the original Region views or reading their samples.
            for (auto const& control : graph.manual_controls) {
                if (!control.coverage_source || overrides_connection(control, view) || is_coverage_source(connection, control)) continue;
                auto const match = std::ranges::find_if(expanded, [&](auto channel) {
                    return channel.bundle == control.input.node_bundle_handle && channel.port == control.input.port_index;
                });
                if (match != expanded.end()) expanded.push_back({*control.coverage_source, 0, match->channel});
                else if (target.bundle == control.input.node_bundle_handle && target.port == control.input.port_index)
                    expanded.push_back({*control.coverage_source, 0, target.channel});
            }
            // A live manual producer replaces project cables at this input.
            // The configured graph keeps them for the settled specialization.
            std::erase_if(expanded, [&](SampleInputChannelId channel) {
                return replaces_source(channel, connection);
            });
            target_crossed_structure = target_crossed_structure
                || expanded.size() != 1 || expanded.front() != target
                || (public_coverage_inputs && !is_terminal_target(target));
            has_terminal_target = has_terminal_target
                || std::ranges::any_of(expanded, is_terminal_target);
            expanded_targets.push_back(std::move(expanded));
        }
        if (!has_terminal_target) continue;

        auto const source_width = channel_count(connection.source_type);
        auto const multiple_sources = connection.source_channels.size() > source_width;
        std::vector<ResolvedSampleChannel> resolved_sources;
        resolved_sources.reserve(source_width);
        bool source_crossed_structure = multiple_sources;
        for (std::size_t index = 0; index < connection.source_channels.size(); ++index) {
            auto const& source = connection.source_channels[index];
            auto resolved = resolve_output(resolve_output, source.channel);
            if (!resolved) return std::unexpected(std::move(resolved.error()));
            if (source.detach) {
                resolved = detach_sample_channel(
                    std::move(*resolved), *source.detach);
                if (!resolved)
                    return std::unexpected(std::move(resolved.error()));
            }
            source_crossed_structure = source_crossed_structure
                || resolved->crossed_structure;
            if (index < source_width)
                resolved_sources.push_back(std::move(*resolved));
            else
                add_sample_channel(resolved_sources[index % source_width], *resolved, 1.0f);
        }

        auto const first_detach = connection.source_channels.front().detach;
        auto const uniform_direct_detach = std::ranges::all_of(
            connection.source_channels,
            [&](ConfiguredSampleConnectionSource const& source) {
                return source.detach == first_detach;
            });
        if (!source_crossed_structure && !target_crossed_structure
            && uniform_direct_detach) {
            routes.push_back(RoutedSampleConnection{
                .connection = connection,
                .authored_connection_index = connection_index,
            });
            continue;
        }

        for (std::size_t source_index = 0; source_index < resolved_sources.size(); ++source_index) {
            auto& resolved = resolved_sources[source_index];
            for (auto& contribution : resolved.contributions) {
                std::vector<NodeBundleHandle> ordinary_sources;
                for (auto const& [source, coefficient] : contribution.source_terms) {
                    if (coefficient != 0 && !manual_source(source) && !std::ranges::contains(ordinary_sources, source.bundle))
                        ordinary_sources.push_back(source.bundle);
                }
                bool initial_assigned = !ordinary_sources.empty();
                for (auto const& [source, coefficient] : contribution.source_terms) {
                    auto control = manual_source(source);
                    if (!control || coefficient == 0) continue;
                    for (std::size_t target_index = 0; target_index < expanded_targets.size(); ++target_index) {
                        auto const edge = channel_conversion_coefficient(connection.source_type,
                            connection.target_type, source_index, target_index) * contribution.output_coefficient;
                        if (edge == 0) continue;
                        for (auto const target : expanded_targets[target_index]) {
                            if (!is_terminal_target(target)) continue;
                            manual_routes.push_back({
                                .configured_connection_index = connection_index,
                                .control_index = *control,
                                .target = {target.bundle, PortKind::sample, target.port},
                                .target_channel = target.channel,
                                .coefficient = coefficient * edge,
                                .delay = contribution.detach ? contribution.detach->loop_extra_latency : 0,
                                .initial_value = contribution.detach ? contribution.detach->initial_value_override : std::nullopt,
                                .initial_coefficient = contribution.detach && !initial_assigned ? edge : 0,
                                .detach_sources = contribution.detach ? ordinary_sources : std::vector<NodeBundleHandle>{},
                            });
                        }
                    }
                    initial_assigned = true;
                }
                std::erase_if(contribution.source_terms, [&](auto const& term) { return manual_source(term.first).has_value(); });
            }
            std::erase_if(resolved.contributions, [](auto const& term) { return term.source_terms.empty(); });
        }
        if (std::ranges::all_of(resolved_sources, [](auto const& source) { return source.contributions.empty(); })) continue;

        auto const has_routed_detach = std::ranges::any_of(
            resolved_sources,
            [](ResolvedSampleChannel const& channel) {
                return std::ranges::any_of(
                    channel.contributions,
                    [](ResolvedSampleContribution const& contribution) {
                        return contribution.detach.has_value();
                    });
            });

        std::vector<SampleOutputChannelId> base_sources;
        for (auto const& resolved : resolved_sources) {
            for (auto const& contribution : resolved.contributions) {
                for (auto const& [source, coefficient] :
                     contribution.source_terms) {
                    if (coefficient != 0.0f
                        && !std::ranges::contains(base_sources, source)) {
                        base_sources.push_back(source);
                    }
                }
            }
        }
        if (base_sources.empty()) {
            return std::unexpected(
                "GraphJit structural sample route resolved to no producer channels");
        }
        std::vector<float> complete_transform;
        std::map<std::optional<NodeBundleHandle>, std::vector<std::size_t>> source_groups;
        if (!has_routed_detach) {
            // A projection intersects its source coverages. Separate the
            // manual projections so each contributes its own Regions.
            for (std::size_t source = 0; source < base_sources.size(); ++source)
                source_groups[coverage_manual_source(graph, base_sources[source].bundle)].push_back(source);
            complete_transform.assign(
                connection.target_channels.size() * base_sources.size(), 0.0f);
            for (std::size_t target = 0;
                 target < connection.target_channels.size();
                 ++target) {
                for (std::size_t semantic_source = 0;
                     semantic_source < resolved_sources.size();
                     ++semantic_source) {
                    auto const edge = channel_conversion_coefficient(
                        connection.source_type,
                        connection.target_type,
                        semantic_source,
                        target);
                    for (auto const& contribution :
                         resolved_sources[semantic_source].contributions) {
                        for (auto const& [source, coefficient] :
                             contribution.source_terms) {
                            auto const found = std::ranges::find(
                                base_sources, source);
                            if (found == base_sources.end()) continue;
                            auto const base = static_cast<std::size_t>(
                                std::distance(base_sources.begin(), found));
                            complete_transform[target * base_sources.size() + base]
                                += edge * contribution.output_coefficient
                                    * coefficient;
                        }
                    }
                }
            }
        }

        struct TargetGroup {
            NodeBundlePortId port{};
            std::vector<SampleInputChannelId> channels{};
            std::vector<std::size_t> semantic_rows{};
        };
        std::vector<TargetGroup> groups;
        for (std::size_t semantic_target = 0;
             semantic_target < expanded_targets.size();
             ++semantic_target) {
            for (auto const target : expanded_targets[semantic_target]) {
                if (!is_terminal_target(target)) continue;
                NodeBundlePortId const port{
                    target.bundle, PortKind::sample, target.port};
                auto found = std::ranges::find(
                    groups, port, &TargetGroup::port);
                if (found == groups.end()) {
                    groups.push_back(TargetGroup{.port = port});
                    found = std::prev(groups.end());
                }
                found->channels.push_back(target);
                found->semantic_rows.push_back(semantic_target);
            }
        }

        for (auto const& group : groups) {
            auto target_type = channel_type_with_count(group.channels.size());
            if (!target_type) return std::unexpected(std::move(target_type.error()));
            auto make_base_connection = [&](std::span<SampleOutputChannelId const> channels) {
                std::vector<ConfiguredSampleConnectionSource> sources;
                sources.reserve(channels.size());
                for (auto const source : channels)
                    sources.push_back({.channel = source});
                return ConfiguredSampleConnection{
                    // The explicit transform/contributions map a list of
                    // producer channels, not one synthetic speaker layout.
                    // Keep the authored semantic type as a label; the matrix
                    // dimensions are authoritative for a structural route.
                    .source_type = connection.source_type,
                    .source_channels = std::move(sources),
                    .target_type = *target_type,
                    .target_channels = group.channels,
                };
            };
            if (has_routed_detach) {
                std::vector<RoutedSampleConnection::Contribution> contributions;
                for (std::size_t semantic_source = 0;
                     semantic_source < resolved_sources.size();
                     ++semantic_source) {
                    for (auto const& resolved :
                         resolved_sources[semantic_source].contributions) {
                        RoutedSampleConnection::Contribution routed{
                            .source_coefficients =
                                std::vector<float>(base_sources.size(), 0.0f),
                            .target_coefficients =
                                std::vector<float>(group.channels.size(), 0.0f),
                            .detach = resolved.detach,
                        };
                        for (auto const& [source, coefficient] :
                             resolved.source_terms) {
                            auto const found = std::ranges::find(
                                base_sources, source);
                            if (found == base_sources.end()) continue;
                            routed.source_coefficients[
                                static_cast<std::size_t>(std::distance(
                                    base_sources.begin(), found))] += coefficient;
                        }
                        for (std::size_t target = 0;
                             target < group.semantic_rows.size();
                             ++target) {
                            routed.target_coefficients[target] =
                                resolved.output_coefficient
                                * channel_conversion_coefficient(
                                    connection.source_type,
                                    connection.target_type,
                                    semantic_source,
                                    group.semantic_rows[target]);
                        }
                        if (std::ranges::all_of(
                                routed.target_coefficients,
                                [](float value) { return value == 0.0f; })) {
                            continue;
                        }
                        auto const existing = std::ranges::find_if(
                            contributions,
                            [&](RoutedSampleConnection::Contribution const& candidate) {
                                return candidate.detach == routed.detach
                                    && candidate.source_coefficients
                                        == routed.source_coefficients;
                            });
                        if (existing == contributions.end()) {
                            contributions.push_back(std::move(routed));
                        } else {
                            for (std::size_t target = 0;
                                 target < existing->target_coefficients.size();
                                 ++target) {
                                existing->target_coefficients[target]
                                    += routed.target_coefficients[target];
                            }
                        }
                    }
                }
                routes.push_back(RoutedSampleConnection{
                    .connection = make_base_connection(base_sources),
                    .contributions = std::move(contributions),
                    .authored_connection_index = connection_index,
                });
                continue;
            }
            for (auto const& [manual, source_indices] : source_groups) {
                std::vector<SampleOutputChannelId> projected_sources;
                projected_sources.reserve(source_indices.size());
                SampleChannelTransformPlan transform{
                    .source_channel_count = source_indices.size(),
                    .target_channel_count = group.channels.size(),
                };
                for (auto const source : source_indices) projected_sources.push_back(base_sources[source]);
                for (auto const row : group.semantic_rows)
                    for (auto const source : source_indices)
                        transform.coefficients.push_back(complete_transform[row * base_sources.size() + source]);
                if (manual && std::ranges::all_of(transform.coefficients, [](float coefficient) { return coefficient == 0; }))
                    continue;
                routes.push_back(RoutedSampleConnection{
                    .connection = make_base_connection(projected_sources),
                    .transform = std::move(transform),
                    .authored_connection_index = connection_index,
                });
            }
        }
    }
    return routes;
}

std::expected<void, std::string> inventory_sample_connections(
    ConfiguredGraph const& graph,
    ResolvedPortRealization const& realization,
    ConnectionAnalysisPlan& plan,
    std::span<RoutedSampleConnection const> routed_connections)
{
    plan.sample_connections.reserve(routed_connections.size());
    for (std::size_t i = 0; i < routed_connections.size(); ++i) {
        auto const& routed = routed_connections[i];
        auto const& connection = routed.connection;
        if (connection.source_channels.empty() || connection.target_channels.empty()) {
            return std::unexpected(
                "configured sample connection has an empty port set");
        }
        if (!routed.transform.empty()
            && (routed.transform.source_channel_count
                    != connection.source_channels.size()
                || routed.transform.target_channel_count
                    != connection.target_channels.size()
                || routed.transform.coefficients.size()
                    != routed.transform.source_channel_count
                        * routed.transform.target_channel_count)) {
            return std::unexpected(
                "structural sample route has an invalid fused transform");
        }
        for (auto const& contribution : routed.contributions) {
            if (contribution.source_coefficients.size()
                    != connection.source_channels.size()
                || contribution.target_coefficients.size()
                    != connection.target_channels.size()) {
                return std::unexpected(
                    "structural sample route has an invalid detached contribution");
            }
        }
        auto const first_target = connection.target_channels.front();
        if (!std::ranges::all_of(
                connection.target_channels,
                [&](SampleInputChannelId target) {
                    return target.bundle == first_target.bundle
                        && target.port == first_target.port;
                })) {
            return std::unexpected(
                "configured sample connection target spans multiple input ports");
        }

        auto const& resolved_target = realization.input(
            first_target.bundle, first_target.port);
        auto const target_geometry = continuous_value_storage_geometry(
            resolved_target.value_type, resolved_target.size);
        if (!target_geometry) {
            return std::unexpected(
                "configured continuous connection has invalid target value geometry");
        }
        SampleConnectionPlan connection_plan{
            .configured_connection_index = routed.authored_connection_index,
            .source_type = connection.source_type,
            .source_channels = [&] {
                std::vector<SampleOutputChannelId> result;
                result.reserve(connection.source_channels.size());
                for (auto const& source : connection.source_channels)
                    result.push_back(source.channel);
                return result;
            }(),
            .target_type = connection.target_type,
            .transform = routed.transform,
            .target_channels = connection.target_channels,
            .target_port = NodeBundlePortId{
                first_target.bundle, PortKind::sample, first_target.port},
            .geometry = *target_geometry,
            .absolute_pace_exponent =
                resolved_target.absolute_pace.exponent(),
        };

        try {
            connection_plan.canonical_source_port =
                graph.node_bundles.sample_output_port_for_channels(
                    connection.source_type, connection_plan.source_channels);
            auto const target = graph.node_bundles.resolve_sample_input(
                connection_plan.target_port).config;
            connection_plan.target_layout = target.channel_layout;
            connection_plan.target_history =
                realization.input(first_target.bundle, first_target.port).history;
            auto const target_access = destination_access(input_access(target));
            connection_plan.destination_access = target_access;
            auto const source_indices = reads_output_gain_source_indices(graph, connection_plan.target_port);

            std::optional<ChannelLayout> canonical_source_layout;
            connection_plan.source_channel_timings.reserve(
                connection.source_channels.size());
            for (auto const& configured_source : connection.source_channels) {
                auto const source_channel = configured_source.channel;
                NodeBundlePortId const source_port{
                    source_channel.bundle, PortKind::sample, source_channel.port};
                auto const source =
                    graph.node_bundles.resolve_sample_output(source_port).config;
                auto const& resolved_source =
                    realization.output(source_channel.bundle, source_channel.port);
                auto const source_geometry = continuous_value_storage_geometry(
                    resolved_source.value_type, resolved_source.size);
                if (!source_geometry || *source_geometry != *target_geometry
                    || resolved_source.absolute_pace
                        != resolved_target.absolute_pace) {
                    return std::unexpected(
                        "configured continuous connection disagrees on value geometry or pace");
                }
                auto const source_history = resolved_source.history;
                auto const source_latency = resolved_source.latency;
                auto const production = source_production(plan, source_channel.bundle);
                auto delivery = connection_delivery(
                    production,
                    source.realization,
                    target_access,
                    source_is_intrinsically_replayable(plan, source_channel.bundle),
                    "sample",
                    i);
                if (!delivery) {
                    return std::unexpected(std::move(delivery.error()));
                }
                connection_plan.source_channel_timings.push_back(
                    SampleSourceChannelTimingPlan{
                        .source = source_channel,
                        .source_layout = source.channel_layout,
                        .detach = configured_source.detach,
                        .production = production,
                        .realization = source.realization,
                        .destination_access = target_access,
                        .delivery = *delivery,
                        .source_history = source_history,
                        .source_latency = source_latency,
                        .read_latency = source_indices ? 0 : source_latency,
                    });
                auto& timing = connection_plan.source_channel_timings.back();
                if (timing.detach) {
                    if (timing.detach->loop_extra_latency == 0)
                        return std::unexpected(
                            "GraphJit sample detach latency must be at least one sample");
                    if (timing.detach->initial_value_override) {
                        if (resolved_target.value_type
                            != continuous_value_type_id_v<Sample>) {
                            return std::unexpected(
                                "Sample detach initial-value override cannot initialize a non-Sample continuous connection");
                        }
                        timing.detach_initial_value.emplace(sizeof(Sample));
                        std::memcpy(
                            timing.detach_initial_value->data(),
                            &*timing.detach->initial_value_override,
                            sizeof(Sample));
                    }
                }
                connection_plan.source_history = std::max(
                    connection_plan.source_history, source_history);
                connection_plan.source_latency = std::max(
                    connection_plan.source_latency, source_latency);
                if (connection_plan.canonical_source_port
                    && *connection_plan.canonical_source_port == source_port) {
                    canonical_source_layout = source.channel_layout;
                }
            }
            connection_plan.canonical_source_layout = canonical_source_layout;
            connection_plan.read_latency = source_indices ? 0 : connection_plan.source_latency;
            connection_plan.routed_contributions.reserve(
                routed.contributions.size());
            for (auto const& routed_contribution : routed.contributions) {
                SampleRoutedContributionPlan contribution{
                    .source_coefficients =
                        routed_contribution.source_coefficients,
                    .target_coefficients =
                        routed_contribution.target_coefficients,
                    .detach = routed_contribution.detach,
                };
                if (contribution.detach
                    && contribution.detach->initial_value_override) {
                    if (resolved_target.value_type
                        != continuous_value_type_id_v<Sample>) {
                        return std::unexpected(
                            "Sample detach initial-value override cannot initialize a non-Sample continuous connection");
                    }
                    contribution.detach_initial_value.emplace(sizeof(Sample));
                    std::memcpy(
                        contribution.detach_initial_value->data(),
                        &*contribution.detach->initial_value_override,
                        sizeof(Sample));
                }
                connection_plan.routed_contributions.push_back(
                    std::move(contribution));
            }
            for (std::size_t source_index = 0;
                 source_index < connection_plan.source_channel_timings.size();
                 ++source_index) {
                auto const& source =
                    connection_plan.source_channel_timings[source_index];
                auto const has_ordinary_contribution =
                    connection_plan.routed_contributions.empty()
                    || std::ranges::any_of(
                        connection_plan.routed_contributions,
                        [&](SampleRoutedContributionPlan const& contribution) {
                            return !contribution.detach
                                && contribution.source_coefficients[source_index]
                                    != 0.0f;
                        });
                if (!source.detach && has_ordinary_contribution) {
                    append_dependency(
                        plan,
                        source.source.bundle,
                        first_target.bundle,
                        PlannedConnectionData::sample,
                        source.production,
                        source.realization,
                        source.destination_access,
                        source.delivery,
                        i);
                }
            }
            auto const first_detach = connection.source_channels.front().detach;
            auto const uniform_detach = first_detach
                && std::ranges::all_of(
                    connection.source_channels,
                    [&](ConfiguredSampleConnectionSource const& source) {
                        return source.detach == first_detach;
                    });
            if (uniform_detach) {
                connection_plan.detach = first_detach;
                connection_plan.detach_initial_value =
                    connection_plan.source_channel_timings.front()
                        .detach_initial_value;
            }
            connection_plan.external_boundary =
                first_target.bundle == plan.boundary_bundle
                || std::ranges::any_of(
                    connection.source_channels,
                    [&](ConfiguredSampleConnectionSource const& source) {
                        return source.channel.bundle == plan.boundary_bundle;
                    });
            auto const canonical_target_channels =
                graph.node_bundles.sample_input_channels(connection_plan.target_port);
            auto const whole_target =
                connection.target_type == target.channel_layout.channel_type
                && std::ranges::equal(
                    connection.target_channels, canonical_target_channels);
            auto const semantic_target_layout = whole_target
                ? target.channel_layout
                : ChannelLayout{
                    .channel_type = connection.target_type,
                    .sample_layout = SampleStreamLayout::planar,
                };
            connection_plan.requires_conversion =
                !routed.contributions.empty()
                || !routed.transform.empty()
                || !connection_plan.canonical_source_port
                || connection.source_type != connection.target_type
                || !canonical_source_layout
                || *canonical_source_layout != semantic_target_layout;
            if (connection_plan.requires_conversion
                && routed.transform.empty()
                && routed.contributions.empty()) {
                // Validate the semantic connection conversion. Partial target
                // contributions convert into their declared target type first;
                // target-port assembly is normalized separately below.
                auto const source_layout = canonical_source_layout.value_or(
                    ChannelLayout{
                        .channel_type = connection.source_type,
                        .sample_layout = SampleStreamLayout::planar,
                    });
                (void)ChannelConversionRegistry::plan(
                    source_layout, semantic_target_layout);
            }
            if (!routed.contributions.empty()) {
                connection_plan.canonical_source_port.reset();
                connection_plan.canonical_source_layout.reset();
            }
        } catch (std::exception const& e) {
            return std::unexpected(
                "sample connection analysis failed: " + std::string(e.what()));
        }
        plan.sample_connections.push_back(std::move(connection_plan));
    }
    return {};
}

std::expected<void, std::string> normalize_sample_target_projections(
    ConfiguredGraph const& graph,
    ConnectionAnalysisPlan& plan)
{
    if (plan.sample_connections.empty()) return {};

    std::vector<SampleConnectionPlan> normalized;
    normalized.reserve(plan.sample_connections.size());
    std::vector<bool> consumed(plan.sample_connections.size(), false);

    for (std::size_t first_index = 0;
         first_index < plan.sample_connections.size(); ++first_index) {
        if (consumed[first_index]) continue;
        auto const target_port =
            plan.sample_connections[first_index].target_port;

        std::vector<std::size_t> group_indices;
        for (std::size_t i = first_index;
             i < plan.sample_connections.size(); ++i) {
            if (!consumed[i]
                && plan.sample_connections[i].target_port == target_port) {
                group_indices.push_back(i);
            }
        }

        auto const target = graph.node_bundles.resolve_sample_input(target_port).config;
        auto const canonical_targets =
            graph.node_bundles.sample_input_channels(target_port);
        auto covers_whole_target = [&](SampleConnectionPlan const& connection) {
            return connection.target_type == target.channel_layout.channel_type
                && std::ranges::equal(
                    connection.target_channels, canonical_targets);
        };

        if (group_indices.size() == 1
            && covers_whole_target(plan.sample_connections[first_index])
            && ((plan.sample_connections[first_index].canonical_source_port
                    && plan.sample_connections[first_index].transform.empty())
                || !plan.sample_connections[first_index]
                        .routed_contributions.empty())) {
            consumed[first_index] = true;
            normalized.push_back(std::move(plan.sample_connections[first_index]));
            continue;
        }

        auto const target_channel_total = canonical_targets.size();
        std::vector<bool> claimed(target_channel_total, false);

        auto const& first = plan.sample_connections[first_index];
        auto target_history = first.target_history;
        auto const uses_routed_contributions = std::ranges::any_of(
            group_indices,
            [&](std::size_t connection_index) {
                auto const& connection =
                    plan.sample_connections[connection_index];
                return connection.detach
                    || !connection.routed_contributions.empty();
            });
        auto detach = uses_routed_contributions
            ? std::optional<ConfiguredSampleConnectionDetach>{}
            : first.detach;
        auto detach_initial_value = uses_routed_contributions
            ? std::optional<std::vector<std::byte>>{}
            : first.detach_initial_value;
        bool external_boundary = false;
        bool requires_block_materialization = false;

        SampleConnectionPlan merged{
            .configured_connection_index = first.configured_connection_index,
            // A normalized composition can contain heterogeneous semantic
            // source types. source_type is therefore only a conservative
            // aggregate label; projection_contributions below are authoritative.
            .source_type = target.channel_layout.channel_type,
            .target_type = target.channel_layout.channel_type,
            .target_layout = target.channel_layout,
            .target_channels = {
                canonical_targets.begin(), canonical_targets.end()},
            .target_port = target_port,
            .geometry = first.geometry,
            .absolute_pace_exponent = first.absolute_pace_exponent,
            .target_history = target_history,
            .destination_access = first.destination_access,
            .requires_conversion = true,
            .requires_block_materialization = false,
            .external_boundary = false,
            .detach = detach,
            .detach_initial_value = detach_initial_value,
        };

        for (auto const connection_index : group_indices) {
            auto const& connection = plan.sample_connections[connection_index];
            if (connection.target_layout != target.channel_layout
                || connection.geometry != first.geometry
                || connection.absolute_pace_exponent
                    != first.absolute_pace_exponent
                || connection.target_history != target_history
                || connection.destination_access != first.destination_access
                || (!uses_routed_contributions
                    && connection.detach != detach)) {
                return std::unexpected(
                    "GraphJit sample target-channel projections disagree on target semantics");
            }
            auto const explicit_routing = !connection.transform.empty()
                || !connection.routed_contributions.empty();
            if ((!explicit_routing && connection.source_channels.size()
                    != channel_count(connection.source_type))
                || connection.target_channels.size()
                    != channel_count(connection.target_type)
                || connection.source_channel_timings.size()
                    != connection.source_channels.size()) {
                return std::unexpected(
                    "GraphJit sample target-channel projection has inconsistent semantic channel counts");
            }

            SampleProjectionContributionPlan contribution{
                .source_type = connection.source_type,
                .target_type = connection.target_type,
                .transform = connection.transform,
            };
            auto const merged_source_begin =
                merged.source_channel_timings.size();
            contribution.source_channel_indices.reserve(
                connection.source_channel_timings.size());
            contribution.target_channels.reserve(
                connection.target_channels.size());

            for (std::size_t source_channel = 0;
                 source_channel < connection.source_channel_timings.size();
                 ++source_channel) {
                auto const flattened_index =
                    merged.source_channel_timings.size();
                merged.source_channels.push_back(
                    connection.source_channels[source_channel]);
                merged.source_channel_timings.push_back(
                    connection.source_channel_timings[source_channel]);
                contribution.source_channel_indices.push_back(flattened_index);
                merged.source_history = std::max(
                    merged.source_history,
                    connection.source_channel_timings[source_channel]
                        .source_history);
                merged.source_latency = std::max(
                    merged.source_latency,
                    connection.source_channel_timings[source_channel]
                        .source_latency);
            }

            for (auto const target_channel : connection.target_channels) {
                auto const found = std::ranges::find(
                    canonical_targets, target_channel);
                if (found == canonical_targets.end()) {
                    return std::unexpected(
                        "GraphJit sample target-channel projection references a foreign target channel");
                }
                auto const target_index = static_cast<std::size_t>(
                    std::distance(canonical_targets.begin(), found));
                claimed[target_index] = true;
                contribution.target_channels.push_back(target_index);
            }

            if (uses_routed_contributions) {
                auto const merged_source_count =
                    merged.source_channel_timings.size();
                for (auto& existing : merged.routed_contributions)
                    existing.source_coefficients.resize(
                        merged_source_count, 0.0f);
                auto append_routed_contribution = [&] (
                    std::span<float const> source_coefficients,
                    std::span<float const> target_coefficients,
                    std::optional<ConfiguredSampleConnectionDetach> contribution_detach,
                    std::optional<std::vector<std::byte>> contribution_initial_value,
                    std::optional<std::size_t> contribution_detach_region)
                    -> std::expected<void, std::string> {
                    if (source_coefficients.size()
                            != connection.source_channel_timings.size()
                        || target_coefficients.size()
                            != connection.target_channels.size()) {
                        return std::unexpected(
                            "GraphJit routed sample contribution has inconsistent channel counts");
                    }
                    SampleRoutedContributionPlan routed_contribution{
                        .source_coefficients =
                            std::vector<float>(merged_source_count, 0.0f),
                        .target_coefficients =
                            std::vector<float>(target_channel_total, 0.0f),
                        .detach = contribution_detach,
                        .detach_initial_value =
                            std::move(contribution_initial_value),
                        .detach_region = contribution_detach_region,
                    };
                    std::ranges::copy(
                        source_coefficients,
                        routed_contribution.source_coefficients.begin()
                            + static_cast<std::ptrdiff_t>(merged_source_begin));
                    for (std::size_t target = 0;
                         target < contribution.target_channels.size();
                         ++target) {
                        routed_contribution.target_coefficients[
                            contribution.target_channels[target]] =
                            target_coefficients[target];
                    }
                    merged.routed_contributions.push_back(
                        std::move(routed_contribution));
                    return {};
                };
                if (!connection.routed_contributions.empty()) {
                    for (auto const& source_contribution :
                         connection.routed_contributions) {
                        auto appended = append_routed_contribution(
                            source_contribution.source_coefficients,
                            source_contribution.target_coefficients,
                            source_contribution.detach,
                            source_contribution.detach_initial_value,
                            source_contribution.detach_region);
                        if (!appended) return appended;
                    }
                } else {
                    auto const local_source_count =
                        connection.source_channel_timings.size();
                    auto const local_target_count =
                        connection.target_channels.size();
                    for (std::size_t target = 0;
                         target < local_target_count;
                         ++target) {
                        std::vector<float> source_coefficients(
                            local_source_count, 0.0f);
                        for (std::size_t source = 0;
                             source < local_source_count;
                             ++source) {
                            source_coefficients[source] =
                                connection.transform.empty()
                                ? channel_conversion_coefficient(
                                    connection.source_type,
                                    connection.target_type,
                                    source,
                                    target)
                                : connection.transform.coefficients[
                                    target * local_source_count + source];
                        }
                        std::vector<float> target_coefficients(
                            local_target_count, 0.0f);
                        target_coefficients[target] = 1.0f;
                        auto appended = append_routed_contribution(
                            source_coefficients,
                            target_coefficients,
                            connection.detach,
                            connection.detach_initial_value,
                            connection.detach_region);
                        if (!appended) return appended;
                    }
                }
                consumed[connection_index] = true;
                external_boundary = external_boundary
                    || connection.external_boundary;
                requires_block_materialization =
                    requires_block_materialization
                    || connection.requires_block_materialization;
                continue;
            }

            // inventory_sample_connections() already validates the configured
            // conversion. Re-check the normalized semantic boundary here so a
            // future producer-layout change cannot silently alter composition.
            if (contribution.transform.empty()) {
                try {
                    (void)ChannelConversionRegistry::plan(
                        ChannelLayout{
                            .channel_type = contribution.source_type,
                            .sample_layout = SampleStreamLayout::planar,
                        },
                        ChannelLayout{
                            .channel_type = contribution.target_type,
                            .sample_layout = SampleStreamLayout::planar,
                        });
                } catch (std::exception const& e) {
                    return std::unexpected(
                        "sample target-channel projection analysis failed: "
                        + std::string(e.what()));
                }
            }

            merged.projection_contributions.push_back(std::move(contribution));
            external_boundary = external_boundary || connection.external_boundary;
            requires_block_materialization = requires_block_materialization
                || connection.requires_block_materialization;
            consumed[connection_index] = true;
        }

        if (!std::ranges::all_of(claimed, [](bool value) { return value; })) {
            return std::unexpected(
                "GraphJit sample target-channel projection does not cover every target channel");
        }

        merged.read_latency = reads_output_gain_source_indices(graph, merged.target_port)
            ? 0 : merged.source_latency;
        merged.requires_block_materialization = requires_block_materialization;
        merged.external_boundary = external_boundary;
        // A normalized projection is intentionally kept as a composition even
        // if its flattened channels happen to reconstruct one canonical port;
        // the contribution boundaries carry conversion/projection semantics.
        merged.canonical_source_port.reset();
        merged.canonical_source_layout.reset();
        normalized.push_back(std::move(merged));
    }

    plan.sample_connections = std::move(normalized);
    return {};
}

struct RoutedEventSource {
    ConfiguredEventConnectionSource source{};
    EventTypeId source_type = EventTypeId::empty;
    EventConversionPlan conversion{};
};

struct RoutedEventConnection {
    ConfiguredEventConnection connection{};
    std::vector<RoutedEventSource> sources{};
    EventConversionPlan conversion{};
    std::size_t authored_connection_index = 0;
};

struct ResolvedEventBranch {
    EventTypeId source_type = EventTypeId::empty;
    std::vector<ConfiguredEventConnectionSource> sources{};
    EventConversionPlan conversion{};
};

std::expected<EventConversionPlan, std::string> plan_event_route_conversion(
    ConfiguredGraph const& graph,
    ConfiguredEventConnection const& connection)
{
    std::optional<bool> timestamp_only;
    for (auto const target : connection.targets) {
        auto const input = graph.node_bundles.resolve_event_input(
            {target.bundle, PortKind::event, target.port}).config;
        if (input.type != connection.target_type
            || (timestamp_only && *timestamp_only != input.timestamp_only)) {
            return std::unexpected(
                "GraphJit event connection targets have inconsistent conversion requirements");
        }
        if (input.timestamp_only
            && (input.type != EventTypeId::trigger
                || input.access != InputAccessKind::coverage)) {
            return std::unexpected(
                "GraphJit timestamp-only event input must accept TriggerEvent Coverage");
        }
        timestamp_only = input.timestamp_only;
    }
    if (timestamp_only.value_or(false)) {
        return EventConversionRegistry::plan_timestamp_projection(
            connection.source_type);
    }
    return EventConversionRegistry::plan(
        connection.source_type, connection.target_type);
}

std::expected<EventConversionPlan, std::string> append_event_conversion(
    EventConversionPlan prefix,
    EventConversionPlan const& suffix)
{
    if (prefix.target_type != suffix.source_type
        || prefix.step_count + suffix.step_count
            > EventConversionPlan::max_steps) {
        return std::unexpected(
            "GraphJit structural event route has an invalid conversion chain");
    }
    for (std::size_t step = 0; step < suffix.step_count; ++step) {
        prefix.steps[prefix.step_count++] = suffix.steps[step];
    }
    prefix.target_type = suffix.target_type;
    return prefix;
}

std::expected<void, std::string> append_event_route_edge(
    ResolvedEventBranch& branch,
    EventConversionPlan const& edge,
    std::optional<ConfiguredEventConnectionDetach> detach)
{
    if (detach) {
        for (auto& source : branch.sources) {
            if (source.detach) {
                return std::unexpected(
                    "GraphJit structural event route contains two serial detach operations");
            }
            source.detach = detach;
        }
    }
    if (branch.conversion.target_type != edge.source_type) {
        EventConversionPlan bridge;
        try {
            bridge = EventConversionRegistry::plan(
                branch.conversion.target_type, edge.source_type);
        } catch (std::exception const& error) {
            return std::unexpected(
                "GraphJit structural event source conversion failed: "
                + std::string(error.what()));
        }
        auto bridged = append_event_conversion(
            std::move(branch.conversion), bridge);
        if (!bridged) return std::unexpected(std::move(bridged.error()));
        branch.conversion = std::move(*bridged);
    }
    auto appended = append_event_conversion(branch.conversion, edge);
    if (!appended) return std::unexpected(std::move(appended.error()));
    branch.conversion = std::move(*appended);
    return {};
}

std::expected<std::vector<RoutedEventConnection>, std::string>
build_structural_event_routes(ConfiguredGraph const& graph, bool public_coverage_inputs = false)
{
    auto const authored = graph.connections.configured_event_connections();
    auto const root_boundary = graph.public_ports.boundary_handle();

    std::vector<std::optional<NodeBundleHandle>> boundary_owner(
        graph.node_bundles.size());
    for (NodeBundleHandle bundle = 0;
         bundle < graph.node_bundles.size();
         ++bundle) {
        auto const& candidate = graph.node_bundles.bundle(bundle);
        if (!candidate.is_subgraph()) continue;
        auto const boundary = candidate.subgraph_boundary_handle();
        if (!boundary || *boundary >= boundary_owner.size()
            || boundary_owner[*boundary]) {
            return std::unexpected(
                "GraphJit structural event routing found an invalid subgraph boundary");
        }
        boundary_owner[*boundary] = bundle;
    }

    auto expand_target = [&](auto&& self, EventInputPortId target)
        -> std::expected<std::vector<EventInputPortId>, std::string> {
        if (target.bundle >= graph.node_bundles.size()) {
            return std::unexpected(
                "GraphJit structural event route references a missing target bundle");
        }
        auto const& bundle = graph.node_bundles.bundle(target.bundle);
        if (!bundle.is_tiled()) return std::vector{target};
        std::vector<EventInputPortId> result;
        for (auto const member : bundle.tiled_members()) {
            auto expanded = self(self, EventInputPortId{
                .bundle = member,
                .port = target.port,
            });
            if (!expanded) return std::unexpected(std::move(expanded.error()));
            result.insert(result.end(), expanded->begin(), expanded->end());
        }
        return result;
    };

    auto is_terminal_target = [&](EventInputPortId target) {
        auto const& bundle = graph.node_bundles.bundle(target.bundle);
        return bundle.is_concrete() || target.bundle == root_boundary;
    };

    std::vector<EventOutputPortId> resolving;
    auto resolve_output = [&](auto&& self, EventOutputPortId output)
        -> std::expected<std::vector<ResolvedEventBranch>, std::string> {
        if (output.bundle >= graph.node_bundles.size()) {
            return std::unexpected(
                "GraphJit structural event route references a missing source bundle");
        }
        if (std::ranges::contains(resolving, output)) {
            return std::unexpected(
                "GraphJit structural event routing reached a cycle before a detach");
        }
        resolving.push_back(output);
        auto pop = [&] { resolving.pop_back(); };
        auto const& bundle = graph.node_bundles.bundle(output.bundle);

        if (bundle.is_concrete() || output.bundle == root_boundary) {
            auto const type = graph.node_bundles.resolve_event_output(
                NodeBundlePortId{
                    output.bundle, PortKind::event, output.port}).config.type;
            pop();
            return std::vector{ResolvedEventBranch{
                .source_type = type,
                .sources = {{.port = output}},
                .conversion = EventConversionRegistry::plan(type, type),
            }};
        }
        if (bundle.is_tiled()) {
            std::vector<ResolvedEventBranch> result;
            for (auto const member : bundle.tiled_members()) {
                auto branches = self(self, EventOutputPortId{
                    .bundle = member,
                    .port = output.port,
                });
                if (!branches) {
                    pop();
                    return std::unexpected(std::move(branches.error()));
                }
                for (auto& branch : *branches)
                    result.push_back(std::move(branch));
            }
            pop();
            return result;
        }

        EventInputPortId upstream_target{};
        if (bundle.is_subgraph()) {
            auto const boundary = bundle.subgraph_boundary_handle();
            if (!boundary) {
                pop();
                return std::unexpected(
                    "GraphJit subgraph event source lost its boundary");
            }
            upstream_target = EventInputPortId{
                .bundle = *boundary,
                .port = output.port,
            };
        } else if (bundle.is_boundary()
                   && output.bundle != root_boundary) {
            if (!boundary_owner[output.bundle]) {
                pop();
                return std::unexpected(
                    "GraphJit internal event boundary has no owning subgraph");
            }
            upstream_target = EventInputPortId{
                .bundle = *boundary_owner[output.bundle],
                .port = output.port,
            };
        } else {
            pop();
            return std::unexpected(
                "GraphJit structural event source has no executable producer");
        }

        std::vector<ResolvedEventBranch> result;
        for (auto const& connection : authored) {
            bool reaches_target = false;
            for (auto const target : connection.targets) {
                auto expanded = expand_target(expand_target, target);
                if (!expanded) {
                    pop();
                    return std::unexpected(std::move(expanded.error()));
                }
                reaches_target = reaches_target
                    || std::ranges::contains(*expanded, upstream_target);
            }
            if (!reaches_target) continue;
            auto edge = plan_event_route_conversion(graph, connection);
            if (!edge) {
                pop();
                return std::unexpected(std::move(edge.error()));
            }
            for (auto const& source : connection.sources) {
                auto branches = self(self, source.port);
                if (!branches) {
                    pop();
                    return std::unexpected(std::move(branches.error()));
                }
                for (auto& branch : *branches) {
                    auto appended = append_event_route_edge(
                        branch, *edge, source.detach);
                    if (!appended) {
                        pop();
                        return std::unexpected(std::move(appended.error()));
                    }
                    result.push_back(std::move(branch));
                }
            }
        }
        pop();
        if (result.empty()) {
            return std::unexpected(
                "GraphJit structural event boundary route has no source");
        }
        return result;
    };

    std::vector<RoutedEventConnection> routes;
    for (std::size_t connection_index = 0;
         connection_index < authored.size();
         ++connection_index) {
        auto const& connection = authored[connection_index];
        std::vector<EventInputPortId> terminal_targets;
        for (auto const target : connection.targets) {
            auto expanded = expand_target(expand_target, target);
            if (!expanded) return std::unexpected(std::move(expanded.error()));
            for (auto const candidate : *expanded) {
                auto const passive = public_coverage_inputs && graph.node_bundles.bundle(candidate.bundle).is_subgraph()
                    && graph.node_bundles.resolve_event_input({candidate.bundle, PortKind::event, candidate.port}).config.access
                        == InputAccessKind::coverage;
                if (public_coverage_inputs ? passive : is_terminal_target(candidate))
                    terminal_targets.push_back(candidate);
            }
        }
        if (terminal_targets.empty()) continue;

        std::vector<ResolvedEventBranch> branches;
        for (auto const& source : connection.sources) {
            auto resolved = resolve_output(resolve_output, source.port);
            if (!resolved) return std::unexpected(std::move(resolved.error()));
            for (auto& branch : *resolved) {
                if (source.detach) {
                    for (auto& resolved_source : branch.sources) {
                        if (resolved_source.detach) {
                            return std::unexpected(
                                "GraphJit structural event route contains two serial detach operations");
                        }
                        resolved_source.detach = source.detach;
                    }
                }
                branches.push_back(std::move(branch));
            }
        }
        auto edge = plan_event_route_conversion(graph, connection);
        if (!edge) return std::unexpected(std::move(edge.error()));
        RoutedEventConnection route{
            .connection = ConfiguredEventConnection{
                .source_type = connection.source_type,
                .target_type = connection.target_type,
                .targets = std::move(terminal_targets),
            },
            .conversion = *edge,
            .authored_connection_index = connection_index,
        };
        for (auto& branch : branches) {
            if (branch.conversion.target_type != connection.source_type) {
                EventConversionPlan bridge;
                try {
                    bridge = EventConversionRegistry::plan(
                        branch.conversion.target_type,
                        connection.source_type);
                } catch (std::exception const& error) {
                    return std::unexpected(
                        "GraphJit structural event fan-in conversion failed: "
                        + std::string(error.what()));
                }
                auto bridged = append_event_conversion(
                    std::move(branch.conversion), bridge);
                if (!bridged)
                    return std::unexpected(std::move(bridged.error()));
                branch.conversion = std::move(*bridged);
            }
            if (branch.conversion.source_type != branch.source_type) {
                return std::unexpected(
                    "GraphJit structural event source has an inconsistent conversion");
            }
            for (auto& source : branch.sources) {
                route.connection.sources.push_back(source);
                route.sources.push_back(RoutedEventSource{
                    .source = std::move(source),
                    .source_type = branch.source_type,
                    .conversion = branch.conversion,
                });
            }
        }
        routes.push_back(std::move(route));
    }
    return routes;
}

std::expected<void, std::string> inventory_event_connections(
    ConfiguredGraph const& graph,
    ResolvedPortRealization const& realization,
    ConnectionAnalysisPlan& plan,
    std::span<RoutedEventConnection const> routed_connections)
{
    plan.event_connections.reserve(routed_connections.size());
    for (std::size_t i = 0; i < routed_connections.size(); ++i) {
        auto const& routed = routed_connections[i];
        auto const& connection = routed.connection;
        if (connection.sources.empty() || connection.targets.empty()) {
            return std::unexpected(
                "configured event connection has an empty port set");
        }
        if (routed.sources.size() != connection.sources.size()) {
            return std::unexpected(
                "structural event route lost its per-source conversion records");
        }
        EventConnectionPlan connection_plan{
            .configured_connection_index = routed.authored_connection_index,
            .source_type = connection.source_type,
            .target_type = connection.target_type,
            .targets = connection.targets,
        };

        try {
            connection_plan.source_plans.reserve(connection.sources.size());
            connection_plan.sources.reserve(connection.sources.size());
            for (std::size_t source_index = 0;
                 source_index < connection.sources.size(); ++source_index) {
                auto const& configured_source = connection.sources[source_index];
                auto const& routed_source = routed.sources[source_index];
                auto const source_id = configured_source.port;
                if (routed_source.source != configured_source) {
                    return std::unexpected(
                        "structural event route source records disagree");
                }
                connection_plan.sources.push_back(source_id);
                NodeBundlePortId const source_port{
                    source_id.bundle, PortKind::event, source_id.port};
                auto const source =
                    graph.node_bundles.resolve_event_output(source_port).config;
                if (source.type != routed_source.source_type
                    || routed_source.conversion.source_type != source.type
                    || routed_source.conversion.target_type
                        != connection.source_type) {
                    return std::unexpected(
                        "structural event source conversion disagrees with its concrete output");
                }
                if (!EventConversionRegistry::is_nonexpanding(
                        routed_source.conversion)) {
                    return std::unexpected(
                        "implicit event source conversion must be non-expanding");
                }
                auto const& resolved_source =
                    realization.event_output(source_id.bundle, source_id.port);
                auto const history = resolved_source.history;
                auto const latency = resolved_source.latency;
                connection_plan.source_history = std::max(
                    connection_plan.source_history, history);
                connection_plan.source_latency = std::max(
                    connection_plan.source_latency, latency);
                if (!is_valid_event_buffer_rate(source.max_events_per_index)) {
                    return std::unexpected(
                        "event output max_events_per_index must be finite and nonnegative");
                }
                connection_plan.max_events_per_index += source.max_events_per_index;
                if (!is_valid_event_buffer_rate(connection_plan.max_events_per_index)) {
                    return std::unexpected(
                        "event connection aggregate max_events_per_index is not representable");
                }
                connection_plan.source_plans.push_back(EventSourcePlan{
                    .source = source_id,
                    .source_type = source.type,
                    .conversion = routed_source.conversion,
                    .production = source_production(plan, source_id.bundle),
                    .realization = source.realization,
                    .history = history,
                    .latency = latency,
                    .max_events_per_index = source.max_events_per_index,
                    .detach = configured_source.detach,
                });
            }

            connection_plan.target_plans.reserve(connection.targets.size());
            for (auto const target_id : connection.targets) {
                NodeBundlePortId const target_port{
                    target_id.bundle, PortKind::event, target_id.port};
                auto const target =
                    graph.node_bundles.resolve_event_input(target_port).config;
                auto const history =
                    realization.event_input(target_id.bundle, target_id.port).history;
                connection_plan.target_history = std::max(
                    connection_plan.target_history, history);
                connection_plan.target_plans.push_back(EventTargetPlan{
                    .target = target_id,
                    .access = destination_access(input_access(target)),
                    .history = history,
                });
            }

            connection_plan.deliveries.reserve(
                connection_plan.source_plans.size()
                * connection_plan.target_plans.size());
            for (std::size_t source_index = 0;
                 source_index < connection_plan.source_plans.size();
                 ++source_index) {
                auto const& source = connection_plan.source_plans[source_index];
                for (std::size_t target_index = 0;
                     target_index < connection_plan.target_plans.size();
                     ++target_index) {
                    auto const& target = connection_plan.target_plans[target_index];
                    auto delivery = connection_delivery(
                        source.production,
                        source.realization,
                        target.access,
                        source_is_intrinsically_replayable(
                            plan, source.source.bundle),
                        "event",
                        i);
                    if (!delivery) {
                        return std::unexpected(std::move(delivery.error()));
                    }
                    connection_plan.deliveries.push_back(EventDeliveryPlan{
                        .source_index = source_index,
                        .target_index = target_index,
                        .mechanism = *delivery,
                    });
                }
            }

            for (std::size_t source_index = 0;
                 source_index < connection_plan.source_plans.size();
                 ++source_index) {
                auto const& source = connection_plan.source_plans[source_index];
                if (source.detach
                    && source.detach->loop_extra_latency == 0) {
                    return std::unexpected(
                        "GraphJit event detach latency must be at least one sample");
                }
                for (auto const& delivery : connection_plan.deliveries) {
                    if (delivery.source_index != source_index
                        || source.detach) continue;
                    auto const& target =
                        connection_plan.target_plans[delivery.target_index];
                    append_dependency(
                        plan,
                        source.source.bundle,
                        target.target.bundle,
                        PlannedConnectionData::event,
                        source.production,
                        source.realization,
                        target.access,
                        delivery.mechanism,
                        i);
                }
            }
            if (!connection_plan.source_plans.empty()) {
                auto const& first_detach =
                    connection_plan.source_plans.front().detach;
                auto const uniform_detach = first_detach
                    && std::ranges::all_of(
                        connection_plan.source_plans,
                        [&](EventSourcePlan const& source) {
                            return source.detach == first_detach;
                        });
                if (uniform_detach)
                    connection_plan.detach = first_detach;
                connection_plan.requires_source_composition =
                    has_detached_source(connection_plan) && !uniform_detach;
            }
            connection_plan.external_boundary =
                std::ranges::any_of(
                    connection.sources,
                    [&](ConfiguredEventConnectionSource const& source) {
                        return source.port.bundle == plan.boundary_bundle;
                    })
                || std::ranges::any_of(
                    connection.targets,
                    [&](EventInputPortId target) {
                        return target.bundle == plan.boundary_bundle;
                    });
            connection_plan.conversion = routed.conversion;
            if (connection_plan.conversion.source_type
                    != connection_plan.source_type
                || connection_plan.conversion.target_type
                    != connection_plan.target_type) {
                return std::unexpected(
                    "structural event route has inconsistent conversion types");
            }
            if (!EventConversionRegistry::is_nonexpanding(
                    connection_plan.conversion)) {
                return std::unexpected(
                    "implicit event conversion must be non-expanding");
            }
            connection_plan.requires_conversion =
                connection_plan.conversion.size() != 0
                || std::ranges::any_of(
                    connection_plan.source_plans,
                    [](EventSourcePlan const& source) {
                        return source.conversion.size() != 0;
                    })
                || (!connection_plan.detach
                    && connection_plan.sources.size() != 1);
        } catch (std::exception const& e) {
            return std::unexpected(
                "event connection analysis failed: " + std::string(e.what()));
        }
        plan.event_connections.push_back(std::move(connection_plan));
    }
    return {};
}

struct ReplayDependencyFact {
    NodeBundleHandle source_bundle = 0;
    NodeBundlePortId source_port{};
    NodeBundlePortId target_port{};
    PlannedConnectionData data = PlannedConnectionData::sample;
    PlannedSourceProduction production = PlannedSourceProduction::tick;
    OutputRealization realization = OutputRealization::computed;
    std::size_t configured_connection_index = 0;
};

bool belongs_to_feedback_scc(ConnectionAnalysisPlan const& plan, NodeBundleHandle bundle)
{
    auto const& background = plan.background;
    if (bundle >= background.bundle_to_semantic_node.size()) return false;
    auto const semantic = background.bundle_to_semantic_node[bundle];
    return semantic && background.semantic_sccs[background.semantic_nodes[*semantic].scc].cyclic;
}

std::expected<void, std::string> derive_contextual_replayability(
    ConfiguredGraph const& graph,
    ConnectionAnalysisPlan& plan)
{
    std::vector<std::vector<ReplayDependencyFact>> incoming(
        graph.node_bundles.size());

    for (auto const& connection : plan.sample_connections) {
        auto const target_bundle = connection.target_port.node_bundle_handle;
        if (!is_internal_bundle(target_bundle, plan.boundary_bundle)
            || target_bundle >= incoming.size()) {
            continue;
        }
        for (auto const& source : connection.source_channel_timings) {
            incoming[target_bundle].push_back(ReplayDependencyFact{
                .source_bundle = source.source.bundle,
                .source_port = NodeBundlePortId{
                    source.source.bundle, PortKind::sample, source.source.port},
                .target_port = connection.target_port,
                .data = PlannedConnectionData::sample,
                .production = source.production,
                .realization = source.realization,
                .configured_connection_index =
                    connection.configured_connection_index,
            });
        }
    }
    for (auto const& connection : plan.event_connections) {
        for (auto const& delivery : connection.deliveries) {
            auto const& source = connection.source_plans[delivery.source_index];
            auto const& target = connection.target_plans[delivery.target_index];
            if (!is_internal_bundle(target.target.bundle, plan.boundary_bundle)
                || target.target.bundle >= incoming.size()) {
                continue;
            }
            incoming[target.target.bundle].push_back(ReplayDependencyFact{
                .source_bundle = source.source.bundle,
                .source_port = NodeBundlePortId{
                    source.source.bundle, PortKind::event, source.source.port},
                .target_port = NodeBundlePortId{
                    target.target.bundle, PortKind::event, target.target.port},
                .data = PlannedConnectionData::event,
                .production = source.production,
                .realization = source.realization,
                .configured_connection_index =
                    connection.configured_connection_index,
            });
        }
    }

    enum class VisitState : std::uint8_t { unseen, visiting, proven };
    std::vector<VisitState> state(graph.node_bundles.size(), VisitState::unseen);
    std::vector<NodeBundleHandle> stack;

    auto data_name = [](PlannedConnectionData data) -> std::string_view {
        return data == PlannedConnectionData::sample ? "sample" : "event";
    };
    auto port_description = [&](ReplayDependencyFact const& dependency) {
        return std::string(data_name(dependency.data)) + " connection "
            + std::to_string(dependency.configured_connection_index)
            + " from bundle " + std::to_string(dependency.source_bundle)
            + " port " + std::to_string(dependency.source_port.port_index)
            + " to bundle "
            + std::to_string(dependency.target_port.node_bundle_handle)
            + " port " + std::to_string(dependency.target_port.port_index);
    };

    auto prove = [&](auto&& self, NodeBundleHandle bundle)
        -> std::expected<void, std::string> {
        auto* node = planned_node_for_bundle(plan, bundle);
        if (!node || !is_internal_bundle(bundle, plan.boundary_bundle)) {
            return std::unexpected(
                "GraphJit contextual replay reached an unavailable live source bundle "
                + std::to_string(bundle));
        }
        if (belongs_to_feedback_scc(plan, bundle)) {
            return std::unexpected("GraphJit contextual replay cannot traverse feedback SCC member bundle "
                + std::to_string(bundle) + "; an explicit recorder is required");
        }
        if (node->replay_mode == TickReplayMode::disabled) {
            return std::unexpected(
                "GraphJit contextual replay requires bundle "
                + std::to_string(bundle)
                + " to be intrinsically replayable or separated by a persisted boundary");
        }
        if (bundle >= state.size()) {
            return std::unexpected(
                "GraphJit contextual replay references an invalid node bundle");
        }
        if (state[bundle] == VisitState::proven) return {};
        if (state[bundle] == VisitState::visiting) {
            std::string cycle;
            auto const first = std::ranges::find(stack, bundle);
            for (auto it = first; it != stack.end(); ++it) {
                if (!cycle.empty()) cycle += " -> ";
                cycle += std::to_string(*it);
            }
            if (!cycle.empty()) cycle += " -> ";
            cycle += std::to_string(bundle);
            return std::unexpected(
                "GraphJit contextual replay dependency cycle [" + cycle
                + "] requires a persisted boundary or explicit recorder");
        }

        state[bundle] = VisitState::visiting;
        stack.push_back(bundle);
        for (auto const& dependency : incoming[bundle]) {
            // Authored Tock production is already a background producer. A
            // finalized persisted Tick output is a stored boundary and must not
            // traverse back into its live producer.
            if (dependency.production == PlannedSourceProduction::tock
                || dependency.realization == OutputRealization::buffered) {
                continue;
            }

            auto const* source = planned_node_for_bundle(
                plan, dependency.source_bundle);
            if (!source
                || !is_internal_bundle(
                    dependency.source_bundle, plan.boundary_bundle)) {
                stack.pop_back();
                state[bundle] = VisitState::unseen;
                return std::unexpected(
                    "GraphJit contextual replay of bundle "
                    + std::to_string(bundle) + " depends on unavailable live "
                    + port_description(dependency)
                    + "; an explicit recorder is required");
            }
            if (source->replay_mode == TickReplayMode::disabled) {
                stack.pop_back();
                state[bundle] = VisitState::unseen;
                return std::unexpected(
                    "GraphJit contextual replay of bundle "
                    + std::to_string(bundle) + " depends on unreproducible Tick/ephemeral "
                    + port_description(dependency)
                    + "; an explicit recorder is required");
            }
            if (auto upstream = self(self, dependency.source_bundle); !upstream) {
                stack.pop_back();
                state[bundle] = VisitState::unseen;
                return upstream;
            }
        }
        stack.pop_back();
        state[bundle] = VisitState::proven;
        node->contextually_replayable = true;
        return {};
    };

    for (auto& connection : plan.sample_connections) {
        for (auto& source : connection.source_channel_timings) {
            if (source.delivery
                != PlannedDeliveryMechanism::replayed_tick_to_random_access) {
                continue;
            }
            if (auto replay = prove(prove, source.source.bundle); !replay) {
                return replay;
            }
            source.contextually_replayable = true;
        }
    }
    for (auto& connection : plan.event_connections) {
        for (auto& delivery : connection.deliveries) {
            if (delivery.mechanism
                != PlannedDeliveryMechanism::replayed_tick_to_random_access) {
                continue;
            }
            auto const& source = connection.source_plans[delivery.source_index];
            if (auto replay = prove(prove, source.source.bundle); !replay) {
                return replay;
            }
            delivery.contextually_replayable = true;
        }
    }
    return {};
}

std::expected<void, std::string> validate_contextual_replay_realization(
    ConfiguredGraph const& graph,
    ResolvedPortRealization const& realization,
    ConnectionAnalysisPlan const& plan)
{
    for (auto const& node : plan.nodes) {
        if (!node.contextually_replayable) continue;
        auto const bundle = node.bundle;
        for (std::size_t port = 0; port < node.sample_input_count; ++port) {
            auto const config = graph.node_bundles.resolve_sample_input(
                {bundle, PortKind::sample, port}).config;
            if (!is_sequential(config)) continue;
            auto const& selected = realization.input(bundle, port);
            if (selected.value_type != continuous_value_type_id_v<Sample>) {
                return std::unexpected(
                    "GraphJit background Tick replay does not yet support continuous type '"
                    + std::string(continuous_value_type_info(
                        selected.value_type).name)
                    + "'");
            }
            if (selected.absolute_pace != PortPace{}) {
                return std::unexpected(
                    "GraphJit background Tick replay does not yet support a non-project-rate continuous port");
            }
        }
        for (std::size_t port = 0; port < node.sample_output_count; ++port) {
            auto const config = graph.node_bundles.resolve_sample_output(
                {bundle, PortKind::sample, port}).config;
            if (!is_sequential_output(config)
                || config.realization != OutputRealization::computed) {
                continue;
            }
            auto const& selected = realization.output(bundle, port);
            if (selected.value_type != continuous_value_type_id_v<Sample>) {
                return std::unexpected(
                    "GraphJit background Tick replay does not yet support continuous type '"
                    + std::string(continuous_value_type_info(
                        selected.value_type).name)
                    + "'");
            }
            if (selected.absolute_pace != PortPace{}) {
                return std::unexpected(
                    "GraphJit background Tick replay does not yet support a non-project-rate continuous port");
            }
        }
    }
    return {};
}


bool has_path(
    std::vector<std::vector<std::size_t>> const& outgoing,
    std::size_t start,
    std::size_t goal)
{
    if (start == goal) return true;
    std::vector<bool> seen(outgoing.size(), false);
    std::vector<std::size_t> queue{start};
    seen[start] = true;
    for (std::size_t i = 0; i < queue.size(); ++i) {
        for (auto const target : outgoing[queue[i]]) {
            if (target == goal) return true;
            if (!seen[target]) {
                seen[target] = true;
                queue.push_back(target);
            }
        }
    }
    return false;
}

bool is_acyclic(std::vector<std::vector<std::size_t>> const& outgoing)
{
    std::vector<std::size_t> indegree(outgoing.size(), 0);
    for (auto const& targets : outgoing) {
        for (auto const target : targets) ++indegree[target];
    }
    std::vector<std::size_t> ready;
    for (std::size_t node = 0; node < indegree.size(); ++node) {
        if (indegree[node] == 0) ready.push_back(node);
    }
    for (std::size_t i = 0; i < ready.size(); ++i) {
        for (auto const target : outgoing[ready[i]]) {
            if (--indegree[target] == 0) ready.push_back(target);
        }
    }
    return ready.size() == outgoing.size();
}

std::expected<void, std::string> validate_explicit_graph_is_acyclic(
    ConfiguredGraph const& graph,
    ConnectionAnalysisPlan const& plan)
{
    if (!is_acyclic(dependency_adjacency(graph, plan, false))) {
        return std::unexpected(
            "GraphJit graph contains an implicit cycle; use detach() to break feedback explicitly");
    }
    return {};
}

std::expected<void, std::string> validate_detach_delivery_mechanisms(
    ConnectionAnalysisPlan const& plan)
{
    for (auto const& connection : plan.sample_connections) {
        if (has_detached_source(connection)
            && !is_entirely_realtime_delivery(connection)) {
            return std::unexpected(
                "GraphJit sample detach requires Tick -> Sequential transport");
        }
    }
    for (auto const& connection : plan.event_connections) {
        if (has_detached_source(connection)
            && !is_entirely_realtime_delivery(connection)) {
            return std::unexpected(
                "GraphJit event detach requires Tick -> Sequential transport");
        }
    }
    return {};
}

std::vector<NodeBundleHandle> unique_source_bundles(
    std::span<SampleOutputChannelId const> sources)
{
    std::vector<NodeBundleHandle> bundles;
    bundles.reserve(sources.size());
    for (auto const source : sources) bundles.push_back(source.bundle);
    std::ranges::sort(bundles);
    bundles.erase(std::unique(bundles.begin(), bundles.end()), bundles.end());
    return bundles;
}

std::vector<NodeBundleHandle> detached_sample_source_bundles(
    SampleConnectionPlan const& connection)
{
    std::vector<NodeBundleHandle> bundles;
    for (std::size_t source_index = 0;
         source_index < connection.source_channel_timings.size();
         ++source_index) {
        auto const directly_detached =
            connection.source_channel_timings[source_index].detach.has_value();
        auto const routed_detached = std::ranges::any_of(
            connection.routed_contributions,
            [&](SampleRoutedContributionPlan const& contribution) {
                return contribution.detach
                    && source_index < contribution.source_coefficients.size()
                    && contribution.source_coefficients[source_index] != 0.0f;
            });
        if (directly_detached || routed_detached)
            bundles.push_back(
                connection.source_channel_timings[source_index].source.bundle);
    }
    std::ranges::sort(bundles);
    bundles.erase(std::unique(bundles.begin(), bundles.end()), bundles.end());
    return bundles;
}

std::expected<BackgroundEvaluationPlan, std::string> build_semantic_background_plan(
    ConfiguredGraph const& graph,
    ConnectionAnalysisPlan const& plan)
{
    BackgroundEvaluationPlan result;
    result.bundle_to_semantic_node.resize(graph.node_bundles.size());
    result.bundle_to_background_node.resize(graph.node_bundles.size());
    result.semantic_nodes.reserve(plan.nodes.size());
    for (std::size_t i = 0; i < plan.nodes.size(); ++i) {
        auto const bundle = plan.nodes[i].bundle;
        result.bundle_to_semantic_node[bundle] = i;
        result.semantic_nodes.push_back(SemanticNodePlan{.bundle = bundle});
    }
    if (plan.nodes.empty()) return result;

    // Semantic cycle membership restores every authored detach edge. Detach
    // changes same-slice execution, not the logical data dependency.
    auto outgoing = dependency_adjacency(graph, plan, false);
    auto append_edge = [&](NodeBundleHandle source_bundle,
                           NodeBundleHandle target_bundle) {
        if (source_bundle >= result.bundle_to_semantic_node.size()
            || target_bundle >= result.bundle_to_semantic_node.size()
            || !result.bundle_to_semantic_node[source_bundle]
            || !result.bundle_to_semantic_node[target_bundle]) {
            return;
        }
        outgoing[*result.bundle_to_semantic_node[source_bundle]].push_back(
            *result.bundle_to_semantic_node[target_bundle]);
    };
    for (auto const& connection : plan.sample_connections) {
        if (!has_detached_source(connection)) continue;
        for (auto const source : detached_sample_source_bundles(connection)) {
            append_edge(source, connection.target_port.node_bundle_handle);
        }
    }
    for (auto const& connection : plan.event_connections) {
        for (auto const& source : connection.source_plans) {
            if (!source.detach) continue;
            for (auto const target : connection.targets) {
                append_edge(source.source.bundle, target.bundle);
            }
        }
    }
    for (auto& targets : outgoing) {
        std::ranges::sort(targets);
        targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    }

    auto const unvisited = std::numeric_limits<std::size_t>::max();
    std::vector<std::size_t> index(plan.nodes.size(), unvisited);
    std::vector<std::size_t> lowlink(plan.nodes.size());
    std::vector<std::size_t> component_of(plan.nodes.size(), unvisited);
    std::vector<bool> on_stack(plan.nodes.size(), false);
    std::vector<std::size_t> stack;
    std::vector<std::vector<std::size_t>> components;
    std::size_t next_index = 0;

    auto strongconnect = [&](auto&& self, std::size_t node) -> void {
        index[node] = next_index;
        lowlink[node] = next_index;
        ++next_index;
        stack.push_back(node);
        on_stack[node] = true;
        for (auto const target : outgoing[node]) {
            if (index[target] == unvisited) {
                self(self, target);
                lowlink[node] = std::min(lowlink[node], lowlink[target]);
            } else if (on_stack[target]) {
                lowlink[node] = std::min(lowlink[node], index[target]);
            }
        }
        if (lowlink[node] != index[node]) return;

        auto const component_index = components.size();
        auto& component = components.emplace_back();
        while (true) {
            auto const member = stack.back();
            stack.pop_back();
            on_stack[member] = false;
            component_of[member] = component_index;
            component.push_back(member);
            if (member == node) break;
        }
    };
    for (std::size_t node = 0; node < plan.nodes.size(); ++node) {
        if (index[node] == unvisited) strongconnect(strongconnect, node);
    }

    // Tarjan discovery order is an implementation detail. Retain SCCs in one
    // deterministic condensation-topological order so later scheduling,
    // background planning, and optimization can all reuse the same identities.
    std::vector<std::vector<std::size_t>> component_outgoing(components.size());
    std::vector<std::size_t> component_indegree(components.size(), 0);
    for (std::size_t source = 0; source < outgoing.size(); ++source) {
        auto const source_component = component_of[source];
        for (auto const target : outgoing[source]) {
            auto const target_component = component_of[target];
            if (source_component == target_component) continue;
            component_outgoing[source_component].push_back(target_component);
        }
    }
    for (auto& targets : component_outgoing) {
        std::ranges::sort(targets);
        targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
        for (auto const target : targets) ++component_indegree[target];
    }
    std::vector<std::size_t> minimum_node(components.size(), unvisited);
    for (std::size_t component = 0; component < components.size(); ++component) {
        std::ranges::sort(components[component]);
        minimum_node[component] = components[component].front();
    }
    std::vector<std::size_t> ready;
    auto insert_ready = [&](std::size_t component) {
        auto const position = std::ranges::lower_bound(
            ready,
            minimum_node[component],
            {},
            [&](std::size_t candidate) { return minimum_node[candidate]; });
        ready.insert(position, component);
    };
    for (std::size_t component = 0; component < components.size(); ++component) {
        if (component_indegree[component] == 0) insert_ready(component);
    }
    std::vector<std::size_t> old_components_in_order;
    old_components_in_order.reserve(components.size());
    while (!ready.empty()) {
        auto const component = ready.front();
        ready.erase(ready.begin());
        old_components_in_order.push_back(component);
        for (auto const target : component_outgoing[component]) {
            if (--component_indegree[target] == 0) insert_ready(target);
        }
    }
    if (old_components_in_order.size() != components.size()) {
        return std::unexpected(
            "GraphJit internal semantic SCC condensation is cyclic");
    }

    std::vector<std::size_t> new_component_of_old(components.size());
    result.semantic_sccs.resize(components.size());
    result.semantic_condensation_order.resize(components.size());
    std::iota(
        result.semantic_condensation_order.begin(),
        result.semantic_condensation_order.end(),
        std::size_t{0});
    for (std::size_t component = 0;
         component < old_components_in_order.size(); ++component) {
        new_component_of_old[old_components_in_order[component]] = component;
    }
    for (std::size_t component = 0;
         component < old_components_in_order.size(); ++component) {
        auto const old_component = old_components_in_order[component];
        auto& retained = result.semantic_sccs[component];
        retained.nodes = components[old_component];
        retained.cyclic = retained.nodes.size() > 1;
        for (auto const node : retained.nodes) {
            result.semantic_nodes[node].scc = component;
            retained.cyclic = retained.cyclic
                || std::ranges::contains(outgoing[node], node);
        }
        for (auto const old_target : component_outgoing[old_component]) {
            retained.outgoing.push_back(new_component_of_old[old_target]);
        }
        std::ranges::sort(retained.outgoing);
    }
    for (std::size_t source = 0;
         source < result.semantic_sccs.size(); ++source) {
        for (auto const target : result.semantic_sccs[source].outgoing) {
            result.semantic_sccs[target].incoming.push_back(source);
        }
    }

    auto validate_edge = [&](NodeBundleHandle source_bundle,
                             NodeBundleHandle target_bundle,
                             std::string_view data,
                             std::size_t connection_index)
        -> std::expected<void, std::string> {
        if (source_bundle >= result.bundle_to_semantic_node.size()
            || target_bundle >= result.bundle_to_semantic_node.size()
            || !result.bundle_to_semantic_node[source_bundle]
            || !result.bundle_to_semantic_node[target_bundle]) {
            return {};
        }
        auto const source = *result.bundle_to_semantic_node[source_bundle];
        auto const target = *result.bundle_to_semantic_node[target_bundle];
        auto const source_scc = result.semantic_nodes[source].scc;
        if (source_scc != result.semantic_nodes[target].scc) return {};

        auto const& component = result.semantic_sccs[source_scc].nodes;
        std::string participants;
        for (auto const member : component) {
            if (!participants.empty()) participants += ", ";
            participants += std::to_string(result.semantic_nodes[member].bundle);
        }
        return std::unexpected(
            "GraphJit background " + std::string(data) + " connection "
            + std::to_string(connection_index) + " from bundle "
            + std::to_string(source_bundle) + " to bundle "
            + std::to_string(target_bundle)
            + " participates in semantic SCC [" + participants + "]");
    };

    for (auto const& connection : plan.sample_connections) {
        for (auto const& source : connection.source_channel_timings) {
            if (uses_realtime_storage(source.delivery)) continue;
            if (auto valid = validate_edge(
                    source.source.bundle,
                    connection.target_port.node_bundle_handle,
                    "sample",
                    connection.configured_connection_index);
                !valid) {
                return std::unexpected(std::move(valid.error()));
            }
        }
    }
    for (auto const& connection : plan.event_connections) {
        for (auto const& delivery : connection.deliveries) {
            if (uses_realtime_storage(delivery.mechanism)) continue;
            auto const& source = connection.source_plans[delivery.source_index];
            auto const& target = connection.target_plans[delivery.target_index];
            if (auto valid = validate_edge(
                    source.source.bundle,
                    target.target.bundle,
                    "event",
                    connection.configured_connection_index);
                !valid) {
                return std::unexpected(std::move(valid.error()));
            }
        }
    }
    return result;
}

// Feedback SCCs are full by contract, without an independent contribution.
// Outside feedback, prove simple full input unions once at compile time so an
// ordinary gain on a full live stream needs no background coverage tracking.
void mark_live_sequential_outputs(
    ConfiguredGraph const& graph,
    ConnectionAnalysisPlan& plan,
    ConnectionView view)
{
    struct Clause { std::size_t target; std::size_t remaining; };
    std::vector<Clause> clauses;
    std::vector<std::vector<std::size_t>> waiting(plan.nodes.size());
    std::vector<std::optional<std::size_t>> node_index(graph.node_bundles.size());
    std::vector<std::size_t> ready;
    for (std::size_t i = 0; i < plan.nodes.size(); ++i) {
        auto& node = plan.nodes[i];
        node_index[node.bundle] = i;
        node.full_live_tick_coverage = node.tick_execution
            && (belongs_to_feedback_scc(plan, node.bundle)
                || node.replay_mode != TickReplayMode::input_union);
        if (node.full_live_tick_coverage) ready.push_back(i);
    }
    auto target_index = [&](NodeBundleHandle bundle) -> std::optional<std::size_t> {
        if (bundle >= node_index.size() || !node_index[bundle]) return {};
        auto index = *node_index[bundle];
        auto const& node = plan.nodes[index];
        if (!node.tick_execution || node.full_live_tick_coverage
            || node.replay_mode != TickReplayMode::input_union) return {};
        return index;
    };
    auto append = [&](std::size_t target, std::vector<std::size_t> sources) {
        if (sources.empty()) return;
        std::ranges::sort(sources);
        sources.erase(std::unique(sources.begin(), sources.end()), sources.end());
        auto const clause = clauses.size();
        clauses.push_back({target, sources.size()});
        for (auto source : sources) waiting[source].push_back(clause);
    };
    for (auto const& connection : plan.sample_connections) {
        auto target = target_index(connection.target_port.node_bundle_handle);
        if (!target || connection.destination_access != PlannedDestinationAccess::sequential) continue;
        auto projection = [&](std::span<std::size_t const> indices) {
            std::vector<std::size_t> sources;
            for (auto index : indices) {
                if (index >= connection.source_channel_timings.size()) return;
                auto const& source = connection.source_channel_timings[index];
                if (source.production != PlannedSourceProduction::tick
                    || !uses_realtime_storage(source.delivery)
                    || source.source.bundle >= node_index.size()
                    || !node_index[source.source.bundle]) return;
                sources.push_back(*node_index[source.source.bundle]);
            }
            append(*target, std::move(sources));
        };
        if (!connection.routed_contributions.empty()) {
            for (auto const& contribution : connection.routed_contributions) {
                if (!std::ranges::any_of(contribution.target_coefficients,
                        [](float value) { return value != 0; })) continue;
                std::vector<std::size_t> indices;
                for (std::size_t i = 0; i < contribution.source_coefficients.size(); ++i)
                    if (contribution.source_coefficients[i] != 0) indices.push_back(i);
                projection(indices);
            }
        } else if (!connection.projection_contributions.empty()) {
            for (auto const& contribution : connection.projection_contributions)
                projection(contribution.source_channel_indices);
        } else {
            std::vector<std::size_t> indices(connection.source_channel_timings.size());
            std::iota(indices.begin(), indices.end(), 0);
            projection(indices);
        }
    }
    for (auto const& connection : plan.event_connections) {
        for (auto const& delivery : connection.deliveries) {
            auto const& to = connection.target_plans[delivery.target_index];
            auto target = target_index(to.target.bundle);
            if (!target || to.access != PlannedDestinationAccess::sequential) continue;
            auto const& source = connection.source_plans[delivery.source_index];
            if (source.production == PlannedSourceProduction::tick
                && uses_realtime_storage(delivery.mechanism)
                && source.source.bundle < node_index.size()
                && node_index[source.source.bundle])
                append(*target, {*node_index[source.source.bundle]});
        }
    }
    for (auto const& control : graph.manual_controls) {
        if (control.participation != ManualControlParticipation::participate_with_connections
            || overrides_connection(control, view) || control.input.port_kind != PortKind::sample) continue;
        auto target = target_index(control.input.node_bundle_handle);
        if (!target || !std::ranges::any_of(plan.sample_connections,
                [&](auto const& connection) { return connection.target_port == control.input; })) continue;
        if (!plan.nodes[*target].full_live_tick_coverage) {
            plan.nodes[*target].full_live_tick_coverage = true;
            ready.push_back(*target);
        }
    }
    for (std::size_t cursor = 0; cursor < ready.size(); ++cursor) {
        for (auto index : waiting[ready[cursor]]) {
            auto& clause = clauses[index];
            if (--clause.remaining != 0) continue;
            auto& target = plan.nodes[clause.target];
            if (target.full_live_tick_coverage) continue;
            target.full_live_tick_coverage = true;
            ready.push_back(clause.target);
        }
    }
}

std::expected<void, std::string> plan_background_connections(
    ConfiguredGraph const& graph,
    ResolvedPortRealization const& realization,
    ConnectionAnalysisPlan const& connections,
    BackgroundEvaluationPlan& plan,
    std::vector<std::size_t>& normalized_connection_indices,
    std::size_t kernel_block_size,
    ConnectionView view)
{
    plan.intrinsic_replay_candidates.clear();
    for (auto const& node : connections.nodes) {
        if (node.replay_mode != TickReplayMode::disabled
            && is_internal_bundle(node.bundle, connections.boundary_bundle)) {
            plan.intrinsic_replay_candidates.push_back(node.bundle);
        }
    }

    // Forward coverage is needed by live Sequential connections as well as
    // replay. A declaration can publish sparse coverage without any consumer
    // requesting background replay of its Tick values.
    auto tracks_tick_coverage = [](PlannedGraphNode const& node) {
        return node.tick_execution && !node.full_live_tick_coverage
            && node.replay_mode == TickReplayMode::input_union;
    };
    std::vector<bool> needs_background_node(graph.node_bundles.size(), false);
    auto require_bundle = [&](NodeBundleHandle bundle) {
        if (bundle < needs_background_node.size()
            && is_internal_bundle(bundle, connections.boundary_bundle)) {
            needs_background_node[bundle] = true;
        }
    };

    for (auto const& node : connections.nodes) {
        auto const bundle = node.bundle;
        bool relevant = !node.tick_execution || node.contextually_replayable || tracks_tick_coverage(node);
        for (std::size_t port = 0; port < node.sample_input_count; ++port) {
            relevant = relevant || is_random_access(
                graph.node_bundles.resolve_sample_input(
                    {bundle, PortKind::sample, port}).config);
        }
        for (std::size_t port = 0; port < node.event_input_count; ++port) {
            relevant = relevant || is_random_access(
                graph.node_bundles.resolve_event_input(
                    {bundle, PortKind::event, port}).config);
        }
        for (std::size_t port = 0; port < node.sample_output_count; ++port) {
            auto const config = graph.node_bundles.resolve_sample_output(
                {bundle, PortKind::sample, port}).config;
            relevant = relevant || is_random_access_output(config)
                || config.realization == OutputRealization::buffered;
        }
        for (std::size_t port = 0; port < node.event_output_count; ++port) {
            auto const config = graph.node_bundles.resolve_event_output(
                {bundle, PortKind::event, port}).config;
            relevant = relevant || is_random_access_output(config)
                || config.realization == OutputRealization::buffered;
        }
        if (relevant) require_bundle(bundle);
    }
    for (auto const& connection : connections.sample_connections) {
        for (auto const& source : connection.source_channel_timings) {
            if (uses_realtime_storage(source.delivery)) continue;
            require_bundle(source.source.bundle);
            require_bundle(connection.target_port.node_bundle_handle);
        }
    }
    for (auto const& connection : connections.event_connections) {
        for (auto const& delivery : connection.deliveries) {
            if (uses_realtime_storage(delivery.mechanism)) continue;
            require_bundle(connection.source_plans[delivery.source_index].source.bundle);
            require_bundle(connection.target_plans[delivery.target_index].target.bundle);
        }
    }

    for (auto const& connection : connections.sample_connections)
        if (std::ranges::any_of(connection.source_channel_timings, [&](auto const& source) {
                auto node = planned_node_for_bundle(connections, source.source.bundle);
                return node && tracks_tick_coverage(*node);
            })) require_bundle(connection.target_port.node_bundle_handle);
    for (auto const& connection : connections.event_connections)
        for (auto const& delivery : connection.deliveries) {
            auto const& source = connection.source_plans[delivery.source_index];
            auto node = planned_node_for_bundle(connections, source.source.bundle);
            if (node && tracks_tick_coverage(*node))
                require_bundle(connection.target_plans[delivery.target_index].target.bundle);
        }

    for (std::size_t semantic_node = 0;
         semantic_node < connections.nodes.size(); ++semantic_node) {
        auto const& node = connections.nodes[semantic_node];
        if (node.bundle >= needs_background_node.size()
            || !needs_background_node[node.bundle]) {
            continue;
        }
        auto const background_node = plan.nodes.size();
        plan.bundle_to_background_node[node.bundle] = background_node;
        plan.semantic_nodes[semantic_node].background_node = background_node;

        bool const authored_tock = !node.tick_execution;

        plan.nodes.push_back(BackgroundNodePlan{
            .bundle = node.bundle,
            .semantic_node = semantic_node,
            .semantic_scc = plan.semantic_nodes[semantic_node].scc,
            .stable_identity = node.stable_identity,
            .resolved_module_input = node.resolved_module_input,
            .authored_tock_execution = authored_tock,
            .replays_tick = node.contextually_replayable,
            .full_live_tick_coverage = node.full_live_tick_coverage,
            .tracks_live_tick_coverage = tracks_tick_coverage(node),
            .authored_forward_coverage = authored_tock,
            .authored_reverse_coverage = authored_tock && node.authored_reverse_coverage,
            .uses_replay_forward_coverage = node.contextually_replayable,
            .uses_replay_reverse_coverage = node.contextually_replayable,
            .replay_mode = node.replay_mode,
            .uses_imported_tick_block_for_replay = node.contextually_replayable,
            .replay_maximum_block_size = node.contextually_replayable
                ? std::min(kernel_block_size, node.maximum_block_size)
                : 0,
        });
    }

    struct PortRoles {
        bool random_access_input = false;
        bool tick_sequential_input = false;
        bool replay_sequential_input = false;
        bool authored_tock_output = false;
        bool persisted_tick_output = false;
        bool replayed_tick_output = false;
        bool coverage_input = false;
        bool coverage_output = false;
    };

    struct PortKey {
        NodeBundlePortId port{};
        PortDirection direction = PortDirection::input;
    };
    struct PortKeyLess {
        bool operator()(PortKey const& lhs, PortKey const& rhs) const noexcept
        {
            NodeBundlePortIdLess const port_less;
            if (port_less(lhs.port, rhs.port)) return true;
            if (port_less(rhs.port, lhs.port)) return false;
            return lhs.direction < rhs.direction;
        }
    };
    std::map<PortKey, BackgroundPortIndex, PortKeyLess>
        port_by_port;

    auto allocate_input_accumulators = [&](BackgroundNodePlan& node_plan) {
        PortAccumulatorIndices indices;
        auto& node_accumulators = node_plan.accumulators;
        if (node_accumulators.input_change_count == 0) {
            node_accumulators.input_change_begin =
                plan.accumulators.input_change_count;
            node_accumulators.input_requirement_begin =
                plan.accumulators.input_requirement_count;
        }
        indices.input_change = plan.accumulators.input_change_count++;
        indices.input_requirement = plan.accumulators.input_requirement_count++;
        ++node_accumulators.input_change_count;
        ++node_accumulators.input_requirement_count;
        return indices;
    };
    auto allocate_output_accumulators = [&](BackgroundNodePlan& node_plan) {
        PortAccumulatorIndices indices;
        auto& node_accumulators = node_plan.accumulators;
        if (node_accumulators.output_change_count == 0) {
            node_accumulators.output_change_begin =
                plan.accumulators.output_change_count;
            node_accumulators.output_requirement_begin =
                plan.accumulators.output_requirement_count;
        }
        indices.output_change = plan.accumulators.output_change_count++;
        indices.output_requirement = plan.accumulators.output_requirement_count++;
        ++node_accumulators.output_change_count;
        ++node_accumulators.output_requirement_count;
        return indices;
    };

    auto ensure_port = [&](NodeBundlePortId port,
                               PortDirection direction,
                               PortRoles roles)
        -> std::expected<BackgroundPortIndex, std::string> {
        if (port.node_bundle_handle >= plan.bundle_to_background_node.size()
            || !plan.bundle_to_background_node[port.node_bundle_handle]) {
            return std::unexpected(
                "GraphJit background topology references a node with no background plan entry");
        }
        auto const background_node = *plan.bundle_to_background_node[port.node_bundle_handle];
        auto& node_plan = plan.nodes[background_node];

        PortKey const key{port, direction};
        if (auto const found = port_by_port.find(key);
            found != port_by_port.end()) {
            auto& port = plan.ports[found->second];
            port.random_access_input = port.random_access_input
                || roles.random_access_input;
            port.tick_sequential_input = port.tick_sequential_input
                || roles.tick_sequential_input;
            port.replay_sequential_input = port.replay_sequential_input
                || roles.replay_sequential_input;
            port.authored_tock_output = port.authored_tock_output
                || roles.authored_tock_output;
            port.persisted_tick_output = port.persisted_tick_output
                || roles.persisted_tick_output;
            port.replayed_tick_output = port.replayed_tick_output
                || roles.replayed_tick_output;
            if (roles.tick_sequential_input
                && !std::ranges::contains(
                    plan.tick_sequential_inputs, found->second)) {
                plan.tick_sequential_inputs.push_back(found->second);
            }
            return found->second;
        }

        std::string name;
        std::optional<OutputRealization> output_realization;
        InputAccessKind configured_input_access = InputAccessKind::sequential;
        InputContentAccess configured_content_access = InputContentAccess::windowed;
        OutputProductionKind configured_output_production =
            OutputProductionKind::sequential;
        ChannelLayout sample_layout{};
        ContinuousValueStorageGeometry continuous_geometry{};
        std::int32_t absolute_pace_exponent = 0;
        EventTypeId event_type = EventTypeId::empty;
        double max_events_per_index = 0.0;
        std::optional<std::size_t> max_event_count;
        Sample neutral{};
        Sample fallback{};
        std::optional<Sample> manual_contribution;
        std::size_t sequential_history = 0;
        std::size_t output_history = 0;
        std::size_t output_latency = 0;
        if (direction == PortDirection::input) {
            if (port.port_kind == PortKind::sample) {
                auto const config = graph.node_bundles.resolve_sample_input(port).config;
                name = config.name;
                sample_layout = config.channel_layout;
                configured_input_access = input_access(config);
                configured_content_access = config.content_access;
                auto const& resolved = realization.input(
                    port.node_bundle_handle, port.port_index);
                continuous_geometry = *continuous_value_storage_geometry(
                    resolved.value_type, resolved.size);
                absolute_pace_exponent = resolved.absolute_pace.exponent();
                if (is_sequential(config)
                    && resolved.value_type == ContinuousValueTypeId::sample) {
                    auto const& metadata = sequential_input_metadata<Sample>(
                        sequential_input_properties(config));
                    neutral = metadata.neutral_value;
                    fallback = neutral;
                    auto const connected = std::ranges::any_of(connections.sample_connections,
                        [&](auto const& connection) { return connection.target_port == port; });
                    auto control = std::ranges::find(graph.manual_controls, port, &ConfiguredManualControl::input);
                    if (connected && control != graph.manual_controls.end()
                        && (overrides_connection(*control, view)
                            || control->participation == ManualControlParticipation::participate_with_connections))
                        manual_contribution = control->settled_value;
                    if (control != graph.manual_controls.end() && !manual_contribution)
                        fallback = control->settled_value;
                    else if (!connected) fallback = metadata.default_value;
                }
                sequential_history =
                    realization.input(
                        port.node_bundle_handle, port.port_index).history;
            } else {
                auto const config = graph.node_bundles.resolve_event_input(port).config;
                name = config.name;
                event_type = config.type;
                configured_input_access = input_access(config);
                configured_content_access = config.content_access;
                sequential_history =
                    realization.event_input(
                        port.node_bundle_handle, port.port_index).history;
            }
        } else {
            if (port.port_kind == PortKind::sample) {
                auto const config = graph.node_bundles.resolve_sample_output(port).config;
                name = config.name;
                output_realization = config.realization;
                sample_layout = config.channel_layout;
                configured_output_production = output_production(config);
                auto const& resolved = realization.output(
                    port.node_bundle_handle, port.port_index);
                continuous_geometry = *continuous_value_storage_geometry(
                    resolved.value_type, resolved.size);
                absolute_pace_exponent = resolved.absolute_pace.exponent();
                output_history = resolved.history;
                output_latency = resolved.latency;
            } else {
                auto const config = graph.node_bundles.resolve_event_output(port).config;
                name = config.name;
                output_realization = config.realization;
                event_type = config.type;
                configured_output_production = output_production(config);
                max_events_per_index = config.max_events_per_index;
                max_event_count = config.max_event_count;
                auto const& resolved = realization.event_output(
                    port.node_bundle_handle, port.port_index);
                output_history = resolved.history;
                output_latency = resolved.latency;
            }
        }

        PortAccumulatorIndices indices;
        if (direction == PortDirection::input
            && (roles.random_access_input || roles.replay_sequential_input
                || roles.coverage_input)) {
            indices = allocate_input_accumulators(node_plan);
        } else if (direction == PortDirection::output) {
            indices = allocate_output_accumulators(node_plan);
        }

        auto const port_index = plan.ports.size();
        std::optional<StableOutputPortId> stable_output;
        std::optional<StableInputPortId> stable_input;
        if (direction == PortDirection::output
            && node_plan.stable_identity) {
            stable_output = StableOutputPortId{
                .node = *node_plan.stable_identity,
                .kind = port.port_kind,
                .port_name = name,
                .port_index = port_migration_index(graph, port, direction, name),
            };
        } else if (direction == PortDirection::input
            && node_plan.stable_identity) {
            stable_input = StableInputPortId{
                .node = *node_plan.stable_identity,
                .kind = port.port_kind,
                .port_name = name,
                .port_index = port_migration_index(graph, port, direction, name),
            };
        }
        if (direction == PortDirection::input && node_plan.resolved_module_input)
            stable_input = node_plan.resolved_module_input;
        plan.ports.push_back(BackgroundPortPlan{
            .node = background_node,
            .configured_port = port,
            .kind = port.port_kind,
            .direction = direction,
            .name = std::move(name),
            .input_access = configured_input_access,
            .content_access = configured_content_access,
            .output_production = configured_output_production,
            .random_access_input = roles.random_access_input,
            .tick_sequential_input = roles.tick_sequential_input,
            .replay_sequential_input = roles.replay_sequential_input,
            .authored_tock_output = roles.authored_tock_output,
            .persisted_tick_output = roles.persisted_tick_output,
            .replayed_tick_output = roles.replayed_tick_output,
            .full_live_tick_coverage = direction == PortDirection::output
                && configured_output_production == OutputProductionKind::sequential
                && node_plan.full_live_tick_coverage,
            .sample_neutral_value = neutral,
            .sample_fallback_value = fallback,
            .sample_manual_contribution = manual_contribution,
            .sequential_history = sequential_history,
            .output_history = output_history,
            .output_latency = output_latency,
            .realization = output_realization,
            .stable_identity = std::move(stable_output),
            .resolved_module_input = direction == PortDirection::output
                ? node_plan.resolved_module_input : std::nullopt,
            .stable_input_identity = std::move(stable_input),
            .sample_layout = sample_layout,
            .continuous_geometry = continuous_geometry,
            .absolute_pace_exponent = absolute_pace_exponent,
            .ordering = port.port_kind == PortKind::sample
                ? (direction == PortDirection::input
                    ? realization.input(
                        port.node_bundle_handle, port.port_index).ordering
                    : realization.output(
                        port.node_bundle_handle, port.port_index).ordering)
                : (direction == PortDirection::input
                    ? realization.event_input(
                        port.node_bundle_handle, port.port_index).ordering
                    : realization.event_output(
                        port.node_bundle_handle, port.port_index).ordering),
            .event_type = event_type,
            .max_events_per_index = max_events_per_index,
            .max_event_count = max_event_count,
            .accumulators = indices,
        });
        port_by_port.emplace(key, port_index);
        if (direction == PortDirection::input) {
            node_plan.inputs.push_back(port_index);
            if (roles.tick_sequential_input) {
                plan.tick_sequential_inputs.push_back(port_index);
            }
        } else {
            node_plan.outputs.push_back(port_index);
            if (roles.authored_tock_output || roles.persisted_tick_output
                || roles.replayed_tick_output) {
                plan.requestable_outputs.push_back(port_index);
            }
        }
        return port_index;
    };

    for (BackgroundNodeIndex background_node = 0;
         background_node < plan.nodes.size(); ++background_node) {
        auto const semantic_node = plan.nodes[background_node].semantic_node;
        auto const& node = connections.nodes[semantic_node];
        auto const bundle = node.bundle;
        for (std::size_t port = 0; port < node.sample_input_count; ++port) {
            NodeBundlePortId const id{bundle, PortKind::sample, port};
            auto const config = graph.node_bundles.resolve_sample_input(id).config;
            PortRoles roles{
                .random_access_input = is_random_access(config),
                .replay_sequential_input = node.contextually_replayable
                    && is_sequential(config),
                .coverage_input = tracks_tick_coverage(node),
            };
            if (!roles.random_access_input && !roles.replay_sequential_input
                && !roles.coverage_input) continue;
            auto port_index = ensure_port(
                id, PortDirection::input, roles);
            if (!port_index) return std::unexpected(std::move(port_index.error()));
        }
        for (std::size_t port = 0; port < node.event_input_count; ++port) {
            NodeBundlePortId const id{bundle, PortKind::event, port};
            auto const config = graph.node_bundles.resolve_event_input(id).config;
            PortRoles roles{
                .random_access_input = is_random_access(config),
                .replay_sequential_input = node.contextually_replayable
                    && is_sequential(config),
                .coverage_input = tracks_tick_coverage(node),
            };
            if (!roles.random_access_input && !roles.replay_sequential_input
                && !roles.coverage_input) continue;
            auto port_index = ensure_port(
                id, PortDirection::input, roles);
            if (!port_index) return std::unexpected(std::move(port_index.error()));
        }
        for (std::size_t port = 0; port < node.sample_output_count; ++port) {
            NodeBundlePortId const id{bundle, PortKind::sample, port};
            auto const config = graph.node_bundles.resolve_sample_output(id).config;
            PortRoles roles{
                .authored_tock_output = !node.tick_execution,
                .persisted_tick_output = node.tick_execution
                    && config.realization == OutputRealization::buffered,
                .replayed_tick_output = node.tick_execution
                    && config.realization == OutputRealization::computed
                    && node.contextually_replayable,
                .coverage_output = is_sequential_output(config) && tracks_tick_coverage(node),
            };
            if (!roles.authored_tock_output && !roles.persisted_tick_output
                && !roles.replayed_tick_output && !roles.coverage_output) continue;
            auto port_index = ensure_port(
                id, PortDirection::output, roles);
            if (!port_index) return std::unexpected(std::move(port_index.error()));
        }
        for (std::size_t port = 0; port < node.event_output_count; ++port) {
            NodeBundlePortId const id{bundle, PortKind::event, port};
            auto const config = graph.node_bundles.resolve_event_output(id).config;
            PortRoles roles{
                .authored_tock_output = !node.tick_execution,
                .persisted_tick_output = node.tick_execution
                    && config.realization == OutputRealization::buffered,
                .replayed_tick_output = node.tick_execution
                    && config.realization == OutputRealization::computed
                    && node.contextually_replayable,
                .coverage_output = is_sequential_output(config) && tracks_tick_coverage(node),
            };
            if (!roles.authored_tock_output && !roles.persisted_tick_output
                && !roles.replayed_tick_output && !roles.coverage_output) continue;
            auto port_index = ensure_port(
                id, PortDirection::output, roles);
            if (!port_index) return std::unexpected(std::move(port_index.error()));
        }
    }

    plan.requires_live_tick_coverage = !plan.tick_sequential_inputs.empty()
        || std::ranges::any_of(plan.nodes,
            [](auto const& node) { return node.tracks_live_tick_coverage; });

    auto port_for = [&](NodeBundlePortId port,
                            PortDirection direction)
        -> std::expected<BackgroundPortIndex, std::string> {
        auto const found = port_by_port.find(PortKey{port, direction});
        if (found == port_by_port.end()) {
            return std::unexpected(
                "GraphJit background connection references an unavailable port");
        }
        return found->second;
    };
    auto append_unique_port = [](std::vector<NodeBundlePortId>& ports,
                                 NodeBundlePortId port) {
        if (!std::ranges::contains(ports, port)) ports.push_back(port);
    };
    auto append_unique_coverage_port = [](std::vector<BackgroundPortIndex>& ports,
                                          BackgroundPortIndex port) {
        if (!std::ranges::contains(ports, port)) {
            ports.push_back(port);
        }
    };
    auto append_connection = [&](BackgroundConnectionPlan connection,
                                 std::size_t normalized_index) {
        auto const index = plan.connections.size();
        for (auto const port : connection.source_coverage_ports) {
            auto& outgoing_connections =
                plan.ports[port].outgoing_connections;
            if (!std::ranges::contains(outgoing_connections, index)) {
                outgoing_connections.push_back(index);
            }
        }
        for (auto const port : connection.target_coverage_ports) {
            auto& incoming_connections =
                plan.ports[port].incoming_connections;
            if (!std::ranges::contains(incoming_connections, index)) {
                incoming_connections.push_back(index);
            }
        }
        plan.connections.push_back(std::move(connection));
        normalized_connection_indices.push_back(normalized_index);
    };
    auto append_background_dependency = [&](BackgroundDependencyPlan dependency) {
        auto const duplicate = std::ranges::find_if(
            plan.background_dependencies,
            [&](BackgroundDependencyPlan const& existing) {
                return existing.source_node == dependency.source_node
                    && existing.target_node == dependency.target_node
                    && existing.source_port == dependency.source_port
                    && existing.target_port == dependency.target_port
                    && existing.kind == dependency.kind
                    && existing.delivery == dependency.delivery
                    && existing.source_is_stored_boundary
                        == dependency.source_is_stored_boundary;
            });
        if (duplicate == plan.background_dependencies.end()) {
            plan.background_dependencies.push_back(std::move(dependency));
        }
    };

    auto node_is_contextually_replayable = [&](NodeBundleHandle bundle) {
        auto const* node = planned_node_for_bundle(connections, bundle);
        return node && node->contextually_replayable;
    };
    auto tracks_coverage_for_bundle = [&](NodeBundleHandle bundle) {
        auto const* node = planned_node_for_bundle(connections, bundle);
        return node && tracks_tick_coverage(*node);
    };
    auto background_node_for_bundle = [&](NodeBundleHandle bundle)
        -> std::optional<BackgroundNodeIndex> {
        if (bundle >= plan.bundle_to_background_node.size()) return std::nullopt;
        return plan.bundle_to_background_node[bundle];
    };

    for (std::size_t normalized_index = 0;
         normalized_index < connections.sample_connections.size(); ++normalized_index) {
        auto const& connection = connections.sample_connections[normalized_index];
        auto const target_bundle = connection.target_port.node_bundle_handle;
        auto const target_replay = node_is_contextually_replayable(target_bundle);
        auto const has_background_delivery = std::ranges::any_of(
            connection.source_channel_timings,
            [](SampleSourceChannelTimingPlan const& source) {
                return !uses_realtime_storage(source.delivery);
            });
        auto const sparse_source = std::ranges::any_of(connection.source_channel_timings,
            [&](auto const& source) { return tracks_coverage_for_bundle(source.source.bundle); });
        if (!has_background_delivery && !target_replay && !sparse_source
            && !tracks_coverage_for_bundle(target_bundle)) continue;

        bool tick_sequential_target = std::ranges::any_of(
            connection.source_channel_timings,
            [](SampleSourceChannelTimingPlan const& source) {
                return source.delivery
                    == PlannedDeliveryMechanism::tock_to_sequential;
            });
        if (sparse_source && !tick_sequential_target
            && is_internal_bundle(target_bundle, connections.boundary_bundle)) {
            auto ensured = ensure_port(connection.target_port, PortDirection::input,
                PortRoles{.coverage_input = true});
            if (!ensured) return std::unexpected(std::move(ensured.error()));
        }
        if (tick_sequential_target
            && is_internal_bundle(target_bundle, connections.boundary_bundle)) {
            auto ensured_port = ensure_port(
                connection.target_port,
                PortDirection::input,
                PortRoles{.tick_sequential_input = true});
            if (!ensured_port) return std::unexpected(std::move(ensured_port.error()));
        }

        BackgroundConnectionPlan background{
            .configured_connection_index = connection.configured_connection_index,
            .kind = PortKind::sample,
            .sample_source_type = connection.source_type,
            .sample_target_type = connection.target_type,
            .sample_source_channels = connection.source_channels,
            .sample_target_channels = connection.target_channels,
            .requires_conversion = connection.requires_conversion,
        };
        background.sample_source_port_by_channel.reserve(
            connection.source_channel_timings.size());
        background.sample_read_latencies.reserve(
            connection.source_channel_timings.size());
        background.sample_deliveries.reserve(
            connection.source_channel_timings.size());

        for (auto const& source : connection.source_channel_timings) {
            background.sample_deliveries.push_back(source.delivery);
            background.sample_read_latencies.push_back(target_replay ? 0 : source.read_latency);
            auto const background_source = !uses_realtime_storage(source.delivery)
                || target_replay || tracks_coverage_for_bundle(source.source.bundle);
            if (!background_source) {
                background.sample_source_port_by_channel.push_back(std::nullopt);
                continue;
            }
            NodeBundlePortId const configured_port{
                source.source.bundle, PortKind::sample, source.source.port};
            auto background_port = port_for(
                configured_port, PortDirection::output);
            if (!background_port) {
                return std::unexpected(std::move(background_port.error()));
            }
            background.sample_source_port_by_channel.push_back(*background_port);
            append_unique_port(background.source_ports, configured_port);
            append_unique_coverage_port(
                background.source_coverage_ports, *background_port);
        }

        if (is_internal_bundle(target_bundle, connections.boundary_bundle)) {
            auto target_port = port_for(
                connection.target_port, PortDirection::input);
            if (!target_port) {
                return std::unexpected(std::move(target_port.error()));
            }
            background.sample_target_port = *target_port;
            background.target_ports.push_back(connection.target_port);
            background.target_coverage_ports.push_back(*target_port);
        }
        for (auto const& projection : connection.projection_contributions) {
            background.sample_projections.push_back(SampleProjectionPlan{
                .source_type = projection.source_type,
                .source_channel_indices = projection.source_channel_indices,
                .target_type = projection.target_type,
                .target_channels = projection.target_channels,
                .transform_source_channel_count =
                    projection.transform.source_channel_count,
                .transform_target_channel_count =
                    projection.transform.target_channel_count,
                .transform_coefficients = projection.transform.coefficients,
            });
        }
        append_connection(std::move(background), normalized_index);

        auto const target_node = background_node_for_bundle(target_bundle);
        if (!target_node) continue;
        for (auto const& source : connection.source_channel_timings) {
            auto const materialized = !uses_realtime_storage(source.delivery);
            auto const replay_sequential = target_replay
                && uses_realtime_storage(source.delivery);
            if (!materialized && !replay_sequential) continue;
            auto const source_node = background_node_for_bundle(source.source.bundle);
            if (!source_node) continue;
            NodeBundlePortId const source_port{
                source.source.bundle, PortKind::sample, source.source.port};
            append_background_dependency(BackgroundDependencyPlan{
                .source_node = *source_node,
                .target_node = *target_node,
                .source_port = source_port,
                .target_port = connection.target_port,
                .kind = replay_sequential
                    ? BackgroundDependencyKind::replay_sequential
                    : BackgroundDependencyKind::materialized_delivery,
                .delivery = source.delivery,
                .source_is_stored_boundary =
                    source.production == PlannedSourceProduction::tick
                    && source.realization == OutputRealization::buffered,
            });
        }
    }

    for (std::size_t normalized_index = 0;
         normalized_index < connections.event_connections.size(); ++normalized_index) {
        auto const& connection = connections.event_connections[normalized_index];
        bool any_relevant = false;
        for (auto const& delivery : connection.deliveries) {
            auto const& target = connection.target_plans[delivery.target_index];
            any_relevant = any_relevant
                || !uses_realtime_storage(delivery.mechanism)
                || node_is_contextually_replayable(target.target.bundle)
                || tracks_coverage_for_bundle(target.target.bundle)
                || tracks_coverage_for_bundle(connection.source_plans[delivery.source_index].source.bundle);
        }
        if (!any_relevant) continue;

        BackgroundConnectionPlan background{
            .configured_connection_index = connection.configured_connection_index,
            .kind = PortKind::event,
            .event_source_type = connection.source_type,
            .event_target_type = connection.target_type,
            .event_conversion = connection.conversion,
            .requires_conversion = connection.requires_conversion,
        };

        for (auto const& delivery : connection.deliveries) {
            auto const& source = connection.source_plans[delivery.source_index];
            auto const& target = connection.target_plans[delivery.target_index];
            auto const target_replay =
                node_is_contextually_replayable(target.target.bundle);
            auto const sparse_source = tracks_coverage_for_bundle(source.source.bundle);
            auto const relevant = !uses_realtime_storage(delivery.mechanism)
                || target_replay; // Coverage-only edges do not request sample replay.
            if (sparse_source && is_internal_bundle(target.target.bundle, connections.boundary_bundle)) {
                auto ensured = ensure_port({target.target.bundle, PortKind::event, target.target.port},
                    PortDirection::input, PortRoles{.coverage_input = true});
                if (!ensured) return std::unexpected(std::move(ensured.error()));
            }

            NodeBundlePortId const source_configured_port{
                source.source.bundle, PortKind::event, source.source.port};
            NodeBundlePortId const target_configured_port{
                target.target.bundle, PortKind::event, target.target.port};
            if (delivery.mechanism
                    == PlannedDeliveryMechanism::tock_to_sequential
                && is_internal_bundle(
                    target.target.bundle, connections.boundary_bundle)) {
                auto ensured_port = ensure_port(
                    target_configured_port,
                    PortDirection::input,
                    PortRoles{.tick_sequential_input = true});
                if (!ensured_port) {
                    return std::unexpected(std::move(ensured_port.error()));
                }
            }

            std::optional<BackgroundPortIndex> source_port;
            std::optional<BackgroundPortIndex> target_port;
            auto const source_has_coverage = !uses_realtime_storage(delivery.mechanism)
                || target_replay || tracks_coverage_for_bundle(source.source.bundle);
            if (source_has_coverage && is_internal_bundle(
                    source.source.bundle, connections.boundary_bundle)) {
                auto port = port_for(
                    source_configured_port, PortDirection::output);
                if (!port) return std::unexpected(std::move(port.error()));
                source_port = *port;
                append_unique_port(background.source_ports, source_configured_port);
                append_unique_coverage_port(background.source_coverage_ports, *port);
            }
            if (is_internal_bundle(
                    target.target.bundle, connections.boundary_bundle)) {
                auto const found = port_by_port.find(PortKey{
                    target_configured_port, PortDirection::input});
                if (found != port_by_port.end()) {
                    target_port = found->second;
                    append_unique_port(background.target_ports, target_configured_port);
                    append_unique_coverage_port(
                        background.target_coverage_ports, found->second);
                } else if (relevant) {
                    return std::unexpected(
                        "GraphJit background event delivery lost its target port");
                }
            }
            std::size_t live_read_latency = 0;
            if (uses_realtime_storage(delivery.mechanism)) {
                auto const detach = connection.detach ? connection.detach->loop_extra_latency : 0;
                if (delivery.read_latency > std::numeric_limits<std::size_t>::max() - detach)
                    return std::unexpected("GraphJit live event coverage delay overflows");
                live_read_latency = delivery.read_latency + detach;
            }
            background.event_deliveries.push_back(graph_jit::EventDeliveryPlan{
                .source = source.source,
                .target = target.target,
                .source_type = source.source_type,
                .conversion = source.conversion,
                .source_port = source_port,
                .target_port = target_port,
                .semantic_source_index = delivery.source_index,
                .mechanism = delivery.mechanism,
                .live_read_latency = live_read_latency,
            });

            if (!relevant) continue;
            auto const source_node = background_node_for_bundle(source.source.bundle);
            auto const target_node = background_node_for_bundle(target.target.bundle);
            if (!source_node || !target_node) continue;
            auto const replay_sequential = target_replay
                && uses_realtime_storage(delivery.mechanism);
            append_background_dependency(BackgroundDependencyPlan{
                .source_node = *source_node,
                .target_node = *target_node,
                .source_port = source_configured_port,
                .target_port = target_configured_port,
                .kind = replay_sequential
                    ? BackgroundDependencyKind::replay_sequential
                    : BackgroundDependencyKind::materialized_delivery,
                .delivery = delivery.mechanism,
                .source_is_stored_boundary =
                    source.production == PlannedSourceProduction::tick
                    && source.realization == OutputRealization::buffered,
            });
        }
        append_connection(std::move(background), normalized_index);
    }

    for (BackgroundPortIndex index = 0; index < plan.ports.size(); ++index) {
        auto const& input = plan.ports[index];
        if (input.direction == PortDirection::input
            && input.input_access != InputAccessKind::sequential
            && input.content_access == InputContentAccess::complete_buffer) {
            auto validate_source = [&](BackgroundPortIndex source) -> bool {
                return source < plan.ports.size()
                    && plan.ports[source].realization == OutputRealization::buffered;
            };
            for (auto const connection_index : input.incoming_connections) {
                auto const& connection = plan.connections[connection_index];
                for (auto const source : connection.source_coverage_ports)
                    if (!validate_source(source)) return std::unexpected(
                        "GraphJit computed output cannot satisfy a complete-buffer input; use explicit materialization");
                for (auto const& delivery : connection.event_deliveries)
                    if (delivery.target_port == index && delivery.source_port
                        && !validate_source(*delivery.source_port)) return std::unexpected(
                        "GraphJit computed event output cannot satisfy a complete-buffer input; use explicit materialization");
            }
        }
        if (input.direction != PortDirection::input
            || input.input_access != InputAccessKind::region) {
            continue;
        }
        if (input.kind == PortKind::event) {
            std::vector<BackgroundPortIndex> sources;
            for (auto const connection_index : input.incoming_connections) {
                auto const& connection = plan.connections[connection_index];
                for (auto const& delivery : connection.event_deliveries) {
                    if (delivery.target_port == index
                        && delivery.source_port) {
                        sources.push_back(*delivery.source_port);
                    }
                }
            }
            if (sources.size() != 1) {
                return std::unexpected(
                    "GraphJit event Region input must have exactly one incoming event contribution");
            }
            auto const source = sources.front();
            if (source >= plan.ports.size()
                || plan.ports[source].kind != PortKind::event
                || plan.ports[source].direction != PortDirection::output) {
                return std::unexpected(
                    "GraphJit event Region input has an invalid source output");
            }
            if (plan.ports[source].output_production
                != OutputProductionKind::region) {
                return std::unexpected(
                    "GraphJit event Region input requires a Region output");
            }
            if (plan.ports[source].persisted_tick_output) {
                return std::unexpected(
                    "GraphJit persisted Tick event output cannot satisfy an event Region input");
            }
            continue;
        }
        if (input.kind != PortKind::sample) continue;
        if (input.incoming_connections.size() != 1) {
            return std::unexpected(
                "GraphJit Region input must have exactly one incoming continuous connection");
        }
        auto const& connection =
            plan.connections[input.incoming_connections.front()];
        if (connection.sample_projections.size() > 1) {
            return std::unexpected(
                "GraphJit Region input cannot receive multiple continuous "
                "projection contributions");
        }
        for (auto const source : connection.source_coverage_ports) {
            if (source >= plan.ports.size()
                || plan.ports[source].kind != PortKind::sample
                || plan.ports[source].direction != PortDirection::output) {
                return std::unexpected(
                    "GraphJit Region input has an invalid continuous source output");
            }
            if (plan.ports[source].output_production
                != OutputProductionKind::region) {
                return std::unexpected(
                    "GraphJit Region input requires a Region output");
            }
            if (plan.ports[source].persisted_tick_output) {
                return std::unexpected(
                    "GraphJit persisted Tick output cannot satisfy a Region input");
            }
        }
    }

    return {};
}

bool executes_in_background(BackgroundNodePlan const& node)
{
    return node.authored_tock_execution || node.replays_tick;
}

std::vector<std::vector<BackgroundNodeIndex>> background_evaluation_adjacency(
    BackgroundEvaluationPlan const& plan)
{
    std::vector<std::vector<BackgroundNodeIndex>> outgoing(plan.nodes.size());
    for (auto const& dependency : plan.background_dependencies) {
        if (dependency.source_is_stored_boundary) continue;
        if (executes_in_background(plan.nodes[dependency.source_node])
            && executes_in_background(plan.nodes[dependency.target_node])) {
            outgoing[dependency.source_node].push_back(dependency.target_node);
        }
    }
    for (auto& targets : outgoing) {
        std::ranges::sort(targets);
        targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    }
    return outgoing;
}

std::expected<void, std::string> schedule_background_topology(
    BackgroundEvaluationPlan& plan,
    std::span<std::size_t const> callback_batch_classes)
{
    std::vector<std::vector<BackgroundNodeIndex>> background_outgoing(
        plan.nodes.size());
    std::vector<BackgroundNodeIndex> parent(plan.nodes.size());
    std::iota(parent.begin(), parent.end(), BackgroundNodeIndex{0});
    auto find_root = [&](auto&& self, BackgroundNodeIndex node)
        -> BackgroundNodeIndex {
        if (parent[node] == node) return node;
        parent[node] = self(self, parent[node]);
        return parent[node];
    };
    auto join = [&](BackgroundNodeIndex lhs, BackgroundNodeIndex rhs) {
        lhs = find_root(find_root, lhs);
        rhs = find_root(find_root, rhs);
        if (lhs == rhs) return;
        if (rhs < lhs) std::swap(lhs, rhs);
        parent[rhs] = lhs;
    };
    for (auto const& connection : plan.connections) {
        std::vector<BackgroundNodeIndex> participating_nodes;
        for (auto const port : connection.source_coverage_ports) {
            auto const node = plan.ports[port].node;
            if (!std::ranges::contains(participating_nodes, node)) {
                participating_nodes.push_back(node);
            }
        }
        for (auto const port : connection.target_coverage_ports) {
            auto const node = plan.ports[port].node;
            if (!std::ranges::contains(participating_nodes, node)) {
                participating_nodes.push_back(node);
            }
        }
        for (std::size_t i = 1; i < participating_nodes.size(); ++i) {
            join(participating_nodes.front(), participating_nodes[i]);
        }
        for (auto const source_port : connection.source_coverage_ports) {
            auto const source = plan.ports[source_port].node;
            for (auto const target_port : connection.target_coverage_ports) {
                auto const target = plan.ports[target_port].node;
                background_outgoing[source].push_back(target);
            }
        }
    }

    std::vector<std::size_t> indegree(plan.nodes.size(), 0);
    for (auto& targets : background_outgoing) {
        std::ranges::sort(targets);
        targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
        for (auto const target : targets) ++indegree[target];
    }
    std::vector<BackgroundNodeIndex> ready_nodes;
    auto insert_ready_node = [&](BackgroundNodeIndex node) {
        ready_nodes.insert(std::ranges::lower_bound(ready_nodes, node), node);
    };
    for (BackgroundNodeIndex node = 0; node < plan.nodes.size(); ++node) {
        if (indegree[node] == 0) insert_ready_node(node);
    }
    std::vector<BackgroundNodeIndex> background_order;
    background_order.reserve(plan.nodes.size());
    auto callback_batch_class = [&](BackgroundNodeIndex node)
        -> std::optional<std::size_t> {
        if (node >= plan.nodes.size()
            || !plan.nodes[node].authored_tock_execution
            || callback_batch_classes.empty()) {
            return std::nullopt;
        }
        auto const bundle = plan.nodes[node].bundle;
        if (bundle >= callback_batch_classes.size()) return std::nullopt;
        auto const selected = callback_batch_classes[bundle];
        if (selected == std::numeric_limits<std::size_t>::max()) {
            return std::nullopt;
        }
        return selected;
    };
    while (!ready_nodes.empty()) {
        auto selected_class = callback_batch_class(ready_nodes.front());
        std::size_t selected_count = selected_class ? 0 : 1;
        for (auto const candidate : ready_nodes) {
            auto const candidate_class = callback_batch_class(candidate);
            if (!candidate_class) continue;
            auto const count = static_cast<std::size_t>(std::ranges::count_if(
                ready_nodes,
                [&](BackgroundNodeIndex node) {
                    return callback_batch_class(node) == candidate_class;
                }));
            if (count > selected_count) {
                selected_class = candidate_class;
                selected_count = count;
            }
        }
        std::vector<BackgroundNodeIndex> selected;
        if (selected_class) {
            for (auto const node : ready_nodes) {
                if (callback_batch_class(node) == selected_class) {
                    selected.push_back(node);
                }
            }
        } else {
            selected.push_back(ready_nodes.front());
        }
        for (std::size_t begin = 0; begin < selected.size();
             begin += initial_callback_batch_lane_limit) {
            auto const end = std::min(
                selected.size(), begin + initial_callback_batch_lane_limit);
            plan.forward_ready_groups.emplace_back(
                selected.begin() + static_cast<std::ptrdiff_t>(begin),
                selected.begin() + static_cast<std::ptrdiff_t>(end));
        }
        for (auto const node : selected) {
            background_order.push_back(node);
            auto const found = std::ranges::find(ready_nodes, node);
            IV_ASSERT(found != ready_nodes.end(),
                "selected background node is absent from the ready frontier");
            ready_nodes.erase(found);
        }
        for (auto const node : selected) {
            for (auto const target : background_outgoing[node]) {
                if (--indegree[target] == 0) insert_ready_node(target);
            }
        }
    }
    if (background_order.size() != plan.nodes.size()) {
        // Coverage queries are suspension-driven. An authored F may not read
        // a structural input at all, or a ready full contribution may resolve
        // a union before other inputs are ready. Validate actual unresolved
        // reads in the planning scheduler, not this whole-node approximation.
        for (BackgroundNodeIndex node = 0; node < plan.nodes.size(); ++node) {
            if (std::ranges::contains(background_order, node)) continue;
            background_order.push_back(node);
            plan.forward_ready_groups.push_back({node});
        }
    }
    plan.requires_live_tick_coverage = plan.requires_live_tick_coverage
        || !plan.tick_sequential_inputs.empty();
    plan.reverse_ready_groups = plan.forward_ready_groups;
    std::ranges::reverse(plan.reverse_ready_groups);

    // The background evaluation DAG is deliberately distinct from semantic SCC
    // membership and from coverage propagation. Stored Tick boundaries terminate
    // traversal, while authored Tock and compiler-provided replay nodes participate in
    // the executable order.
    auto const evaluation_outgoing = background_evaluation_adjacency(plan);
    std::vector<std::size_t> background_indegree(plan.nodes.size(), 0);
    for (auto const& targets : evaluation_outgoing) {
        for (auto const target : targets) ++background_indegree[target];
    }
    std::vector<BackgroundNodeIndex> background_ready;
    auto insert_background_ready = [&](BackgroundNodeIndex node) {
        background_ready.insert(
            std::ranges::lower_bound(background_ready, node), node);
    };
    std::size_t executable_count = 0;
    for (BackgroundNodeIndex node = 0; node < plan.nodes.size(); ++node) {
        if (!executes_in_background(plan.nodes[node])) continue;
        ++executable_count;
        if (background_indegree[node] == 0) insert_background_ready(node);
    }
    while (!background_ready.empty()) {
        auto selected_class = callback_batch_class(background_ready.front());
        std::size_t selected_count = selected_class ? 0 : 1;
        for (auto const candidate : background_ready) {
            auto const candidate_class = callback_batch_class(candidate);
            if (!candidate_class) continue;
            auto const count = static_cast<std::size_t>(std::ranges::count_if(
                background_ready,
                [&](BackgroundNodeIndex node) {
                    return callback_batch_class(node) == candidate_class;
                }));
            if (count > selected_count) {
                selected_class = candidate_class;
                selected_count = count;
            }
        }
        std::vector<BackgroundNodeIndex> selected;
        if (selected_class) {
            for (auto const node : background_ready) {
                if (callback_batch_class(node) == selected_class) {
                    selected.push_back(node);
                }
            }
        } else {
            selected.push_back(background_ready.front());
        }
        for (std::size_t begin = 0; begin < selected.size();
             begin += initial_callback_batch_lane_limit) {
            auto const end = std::min(
                selected.size(), begin + initial_callback_batch_lane_limit);
            plan.evaluation_ready_groups.emplace_back(
                selected.begin() + static_cast<std::ptrdiff_t>(begin),
                selected.begin() + static_cast<std::ptrdiff_t>(end));
        }
        for (auto const node : selected) {
            plan.background_evaluation_order.push_back(node);
            auto const found = std::ranges::find(background_ready, node);
            IV_ASSERT(found != background_ready.end(),
                "selected background evaluation node is absent from the ready frontier");
            background_ready.erase(found);
        }
        for (auto const node : selected) {
            for (auto const target : evaluation_outgoing[node]) {
                if (--background_indegree[target] == 0) {
                    insert_background_ready(target);
                }
            }
        }
    }
    if (plan.background_evaluation_order.size() != executable_count) {
        return std::unexpected(
            "GraphJit background evaluation dependency graph contains an unresolved replay cycle");
    }

    std::map<BackgroundNodeIndex, std::size_t> component_by_root;
    std::vector<std::optional<std::size_t>> component_by_node(plan.nodes.size());
    for (auto const node : background_order) {
        auto const root = find_root(find_root, node);
        auto [entry, inserted] = component_by_root.emplace(
            root, plan.components.size());
        if (inserted) {
            plan.component_order.push_back(entry->second);
            plan.components.emplace_back();
        }
        auto& component = plan.components[entry->second];
        component_by_node[node] = entry->second;
        component.nodes.push_back(node);
        component.forward_order.push_back(node);
        if (plan.nodes[node].authored_tock_execution) {
            component.tock_order.push_back(node);
        }
        if (plan.nodes[node].replays_tick) {
            component.replay_order.push_back(node);
        }
        auto const scc = plan.nodes[node].semantic_scc;
        if (!std::ranges::contains(component.semantic_scc_order, scc)) {
            component.semantic_scc_order.push_back(scc);
        }
    }
    for (auto& component : plan.components) {
        component.reverse_order = component.forward_order;
        std::ranges::reverse(component.reverse_order);
    }
    for (auto const node : plan.background_evaluation_order) {
        if (node < component_by_node.size() && component_by_node[node]) {
            plan.components[*component_by_node[node]]
                .background_evaluation_order.push_back(node);
        }
    }
    for (BackgroundConnectionIndex connection = 0;
         connection < plan.connections.size(); ++connection) {
        auto const& planned = plan.connections[connection];
        std::optional<BackgroundNodeIndex> node;
        if (!planned.source_coverage_ports.empty()) {
            node = plan.ports[planned.source_coverage_ports.front()].node;
        } else if (!planned.target_coverage_ports.empty()) {
            node = plan.ports[planned.target_coverage_ports.front()].node;
        }
        if (!node) continue;
        auto const root = find_root(find_root, *node);
        plan.components[component_by_root.at(root)]
            .connections.push_back(connection);
    }

    return {};
}

std::expected<void, std::string> validate_detached_connections(
    ConfiguredGraph const& graph,
    ConnectionAnalysisPlan const& plan)
{
    auto const semantic_outgoing = dependency_adjacency(graph, plan, false);
    std::vector<std::optional<std::size_t>> bundle_to_node(
        graph.node_bundles.size());
    for (std::size_t i = 0; i < plan.nodes.size(); ++i)
        bundle_to_node[plan.nodes[i].bundle] = i;

    auto validate = [&](std::span<NodeBundleHandle const> sources,
                        std::span<NodeBundleHandle const> targets,
                        std::string_view kind,
                        std::size_t configured_connection_index)
        -> std::expected<void, std::string> {
        if (sources.empty() || targets.empty())
            return std::unexpected(
                "GraphJit " + std::string(kind)
                + " detach has an empty port set");
        for (auto const target : targets) {
            if (target >= bundle_to_node.size() || !bundle_to_node[target])
                return std::unexpected(
                    "GraphJit detached connection must target a concrete graph node");
            bool closes_semantic_cycle = false;
            for (auto const source : sources) {
                if (source >= bundle_to_node.size() || !bundle_to_node[source])
                    continue;
                closes_semantic_cycle = closes_semantic_cycle
                    || has_path(
                        semantic_outgoing,
                        *bundle_to_node[target],
                        *bundle_to_node[source]);
            }
            if (!closes_semantic_cycle) {
                return std::unexpected(
                    "GraphJit " + std::string(kind) + " detach on connection "
                    + std::to_string(configured_connection_index)
                    + " breaks an acyclic dependency");
            }
        }
        return {};
    };

    for (auto const& connection : plan.sample_connections) {
        std::array<NodeBundleHandle, 1> targets{
            connection.target_port.node_bundle_handle};
        if (connection.detach) {
            auto sources = unique_source_bundles(connection.source_channels);
            if (auto result = validate(
                    sources, targets, "sample",
                    connection.configured_connection_index);
                !result) return result;
        }
        for (auto const& contribution : connection.routed_contributions) {
            if (!contribution.detach) continue;
            std::vector<NodeBundleHandle> sources;
            for (std::size_t source_index = 0;
                 source_index < contribution.source_coefficients.size();
                 ++source_index) {
                if (contribution.source_coefficients[source_index] == 0.0f)
                    continue;
                sources.push_back(
                    connection.source_channel_timings[source_index]
                        .source.bundle);
            }
            std::ranges::sort(sources);
            sources.erase(
                std::unique(sources.begin(), sources.end()), sources.end());
            if (auto result = validate(
                    sources, targets, "sample",
                    connection.configured_connection_index);
                !result) return result;
        }
    }
    for (auto const& route : plan.manual_sample_routes) {
        if (route.delay == 0) continue;
        // A manual value has no upstream execution dependency of its own.
        // It may share a temporal cut with a cyclic ordinary contribution,
        // but cannot turn detach into an arbitrary feed-forward delay.
        if (route.detach_sources.empty()) return std::unexpected(
            "GraphJit manual sample detach on connection " + std::to_string(route.configured_connection_index)
            + " breaks an acyclic dependency");
        std::array<NodeBundleHandle, 1> targets{route.target.node_bundle_handle};
        if (auto result = validate(route.detach_sources, targets, "manual sample", route.configured_connection_index); !result)
            return result;
    }
    for (auto const& connection : plan.event_connections) {
        std::vector<NodeBundleHandle> targets;
        targets.reserve(connection.targets.size());
        for (auto const target : connection.targets)
            targets.push_back(target.bundle);
        std::ranges::sort(targets);
        targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
        for (auto const& source : connection.source_plans) {
            if (!source.detach) continue;
            std::array<NodeBundleHandle, 1> sources{source.source.bundle};
            if (auto result = validate(
                    sources, targets, "event",
                    connection.configured_connection_index);
                !result) return result;
        }
    }
    return {};
}

// A Sequential input of either stream kind owns the values its sources have
// finalized that its read delay beyond their latency has not yet let it read:
// compiler-owned compensation plus any detach() delay. Path equalization has
// set every read_latency by now, so this sets each input record's
// in_flight_samples: per target channel for a sample input, the largest over
// its deliveries for an event stream. Every channel of an input fed only through realtime storage
// gets a record, even an empty one, so that a successor whose read delay grows
// from nothing still finds the window it grows from.
// The positions a Sequential input's read delay holds beyond its source's
// latency, whatever its stream kind: compiler-owned compensation plus any
// detach() delay. They are the input's in-flight values.
std::size_t in_flight_extent(
    std::size_t read_latency,
    std::size_t source_latency,
    std::size_t detach_latency) noexcept
{
    auto const compensation = read_latency > source_latency
        ? read_latency - source_latency
        : 0;
    return saturating_add(compensation, detach_latency);
}

// Raises one input record's in-flight count: a sample input's record for one
// channel, or an event input's stream record (channel empty). An input without
// a record gets one when it is fed entirely in realtime; a slot-fed or mixed
// input binds no realtime storage that could hold one.
void record_input_in_flight(
    RealtimePortStateRequirements& requirements,
    NodeBundlePortId target,
    std::optional<std::size_t> channel,
    std::string const& port_name,
    std::optional<std::size_t> port_index,
    std::optional<StableConcreteNodeId> const& stable_node,
    std::size_t in_flight,
    bool realtime_fed)
{
    if (in_flight == 0 && !realtime_fed) return;
    auto const existing = std::ranges::find_if(
        requirements.states,
        [&](RealtimePortStateRequirement const& requirement) {
            return requirement.configured_port == target
                && requirement.direction == PortDirection::input
                && requirement.role
                    == RealtimePortStateRole::sequential_input_history
                && requirement.channel == channel;
        });
    if (existing != requirements.states.end()) {
        existing->in_flight_samples = std::max(
            existing->in_flight_samples, in_flight);
        return;
    }
    if (!realtime_fed) return;
    append_realtime_port_state(
        requirements,
        target,
        PortDirection::input,
        port_name,
        port_index,
        channel,
        RealtimePortStateRole::sequential_input_history,
        0,
        stable_node,
        in_flight,
        realtime_fed);
}

std::expected<void, std::string> inventory_sequential_input_in_flight_values(
    ConfiguredGraph const& graph,
    ConnectionAnalysisPlan& plan)
{
    for (auto const& connection : plan.sample_connections) {
        if (connection.destination_access
            != PlannedDestinationAccess::sequential) {
            continue;
        }
        auto const& timings = connection.source_channel_timings;
        auto const in_flight_of = [&](
            std::size_t source,
            std::optional<ConfiguredSampleConnectionDetach> const& contribution_detach)
            -> std::size_t {
            if (source >= timings.size()) return 0;
            auto const& timing = timings[source];
            if (timing.delivery != PlannedDeliveryMechanism::tick_to_sequential) {
                return 0;
            }
            auto const& detach = timing.detach
                ? timing.detach
                : contribution_detach ? contribution_detach : connection.detach;
            return in_flight_extent(
                timing.read_latency,
                timing.source_latency,
                detach ? detach->loop_extra_latency : 0);
        };

        auto const channels = channel_count(connection.target_layout);
        std::vector<std::size_t> in_flight(channels, 0);
        auto const raise = [&](std::size_t channel, std::size_t value) {
            if (channel < in_flight.size()) {
                in_flight[channel] = std::max(in_flight[channel], value);
            }
        };
        if (!connection.routed_contributions.empty()) {
            for (auto const& contribution : connection.routed_contributions) {
                for (std::size_t target = 0;
                     target < contribution.target_coefficients.size(); ++target) {
                    if (contribution.target_coefficients[target] == 0.0f) continue;
                    for (std::size_t source = 0;
                         source < contribution.source_coefficients.size();
                         ++source) {
                        if (contribution.source_coefficients[source] == 0.0f) {
                            continue;
                        }
                        raise(target, in_flight_of(source, contribution.detach));
                    }
                }
            }
        } else if (!connection.projection_contributions.empty()) {
            for (auto const& contribution : connection.projection_contributions) {
                // A same-type contribution without a transform maps each source
                // channel to the target channel in the same position.
                auto const pairwise = contribution.source_type
                        == contribution.target_type
                    && contribution.transform.empty()
                    && contribution.source_channel_indices.size()
                        == contribution.target_channels.size();
                for (std::size_t position = 0;
                     position < contribution.target_channels.size(); ++position) {
                    auto const target = contribution.target_channels[position];
                    if (pairwise) {
                        raise(target, in_flight_of(
                            contribution.source_channel_indices[position],
                            std::nullopt));
                        continue;
                    }
                    auto const source_count = contribution.source_channel_indices.size();
                    for (std::size_t column = 0; column < source_count; ++column) {
                        auto const coefficient = contribution.transform.empty()
                            ? channel_conversion_coefficient(contribution.source_type,
                                contribution.target_type, column, position)
                            : contribution.transform.coefficients[position * source_count + column];
                        if (coefficient == 0.0f) continue;
                        raise(target, in_flight_of(contribution.source_channel_indices[column], std::nullopt));
                    }
                }
            }
        } else {
            std::size_t whole_port = 0;
            for (std::size_t source = 0; source < timings.size(); ++source) {
                whole_port = std::max(whole_port, in_flight_of(source, std::nullopt));
            }
            for (std::size_t channel = 0; channel < channels; ++channel) {
                raise(channel, whole_port);
            }
        }

        auto const realtime_fed = !timings.empty()
            && std::ranges::all_of(
                timings,
                [](SampleSourceChannelTimingPlan const& timing) {
                    return timing.delivery
                        == PlannedDeliveryMechanism::tick_to_sequential;
                });
        auto const& target = connection.target_port;
        auto const& bundle = graph.node_bundles.bundle(target.node_bundle_handle);
        if (!bundle.is_concrete()) continue;
        auto const inputs = bundle.concrete_ports().sample_inputs();
        if (target.port_index >= inputs.size()
            || !is_sequential(inputs[target.port_index])) {
            continue;
        }
        auto const planned = std::ranges::find(
            plan.nodes, target.node_bundle_handle, &PlannedGraphNode::bundle);
        if (planned == plan.nodes.end()) {
            return std::unexpected(
                "GraphJit in-flight input state lost its planned concrete node");
        }
        auto const& name = inputs[target.port_index].name;
        auto const identity_index = planned->stable_identity && realtime_fed
            ? port_migration_index(graph, target, PortDirection::input, name) : std::nullopt;
        for (std::size_t channel = 0; channel < channels; ++channel) {
            record_input_in_flight(
                plan.realtime_port_states,
                target,
                channel,
                name,
                identity_index,
                planned->stable_identity,
                in_flight[channel],
                realtime_fed);
        }
    }

    // An event input's stream has one record; its in-flight extent is the
    // largest over the deliveries it receives.
    for (auto const& connection : plan.event_connections) {
        for (std::size_t target_index = 0;
             target_index < connection.target_plans.size(); ++target_index) {
            auto const& target_plan = connection.target_plans[target_index];
            if (target_plan.access != PlannedDestinationAccess::sequential) {
                continue;
            }
            std::size_t in_flight = 0;
            bool delivered = false;
            bool realtime_fed = true;
            for (auto const& delivery : connection.deliveries) {
                if (delivery.target_index != target_index
                    || delivery.source_index >= connection.source_plans.size()) {
                    continue;
                }
                delivered = true;
                if (delivery.mechanism
                    != PlannedDeliveryMechanism::tick_to_sequential) {
                    realtime_fed = false;
                    continue;
                }
                auto const& source = connection.source_plans[delivery.source_index];
                auto const& detach = source.detach ? source.detach : connection.detach;
                in_flight = std::max(in_flight, in_flight_extent(
                    delivery.read_latency,
                    source.latency,
                    detach ? detach->loop_extra_latency : 0));
            }
            if (!delivered) continue;
            auto const target = NodeBundlePortId{
                target_plan.target.bundle, PortKind::event, target_plan.target.port};
            auto const& bundle = graph.node_bundles.bundle(target.node_bundle_handle);
            if (!bundle.is_concrete()) continue;
            auto const inputs = bundle.concrete_ports().event_inputs();
            if (target.port_index >= inputs.size()
                || !is_sequential(inputs[target.port_index])) {
                continue;
            }
            auto const planned = std::ranges::find(
                plan.nodes, target.node_bundle_handle, &PlannedGraphNode::bundle);
            if (planned == plan.nodes.end()) {
                return std::unexpected(
                    "GraphJit in-flight input state lost its planned concrete node");
            }
            record_input_in_flight(
                plan.realtime_port_states,
                target,
                std::nullopt,
                inputs[target.port_index].name,
                planned->stable_identity && realtime_fed
                    ? port_migration_index(graph, target, PortDirection::input, inputs[target.port_index].name)
                    : std::nullopt,
                planned->stable_identity,
                in_flight,
                realtime_fed);
        }
    }
    return {};
}

} // namespace

void configure_module_input_event_bounds(ConfiguredGraph& graph)
{
    // Resolve module aliases and tiled targets once. Conversions are required
    // to be non-expanding by the normal event-route inventory.
    std::map<NodeBundleHandle, std::vector<EventOutputPortId>> ingress;
    for (NodeBundleHandle bundle = 0; bundle < graph.node_bundles.size(); ++bundle) {
        auto const origin = graph.node_bundles.bundle(bundle).resolved_module_input();
        if (origin && origin->port_kind == PortKind::event) ingress.try_emplace(bundle);
    }
    if (ingress.empty()) return;
    auto routes = build_structural_event_routes(graph);
    if (!routes) throw std::invalid_argument(routes.error());
    for (auto const& route : *routes) {
        for (auto target : route.connection.targets) {
            auto found = ingress.find(target.bundle);
            if (found == ingress.end()) continue;
            for (auto const& source : route.sources) found->second.push_back(source.source.port);
        }
    }
    struct Bound {
        double rate = 0;
        std::optional<std::size_t> total = 0;
    };
    std::map<NodeBundleHandle, Bound> complete;
    std::vector<NodeBundleHandle> active;
    auto derive = [&](auto&& self, NodeBundleHandle bundle) -> Bound {
        if (auto found = complete.find(bundle); found != complete.end()) return found->second;
        if (std::ranges::contains(active, bundle))
            throw std::invalid_argument("Sequential module event inputs form a routing cycle without an authored event producer bound");
        active.push_back(bundle);
        Bound result;
        for (auto source : ingress.at(bundle)) {
            auto const config = graph.node_bundles.resolve_event_output(
                {source.bundle, PortKind::event, source.port}).config;
            auto const bound = ingress.contains(source.bundle)
                ? self(self, source.bundle)
                : Bound{config.max_events_per_index, config.max_event_count};
            result.rate += bound.rate;
            if (!is_valid_event_buffer_rate(bound.rate) || !is_valid_event_buffer_rate(result.rate))
                throw std::invalid_argument("Sequential module event input aggregate rate is not representable");
            if (result.total && bound.total) {
                if (*bound.total > std::numeric_limits<std::size_t>::max() - *result.total)
                    throw std::overflow_error("Sequential module event input aggregate event count overflows");
                *result.total += *bound.total;
            } else result.total.reset();
        }
        active.pop_back();
        complete.emplace(bundle, result);
        return result;
    };
    for (auto const& [bundle, sources] : ingress) {
        (void)sources;
        auto bound = derive(derive, bundle);
        graph.node_bundles.bundle(bundle).set_input_resolution_event_bounds(bound.rate, bound.total);
    }
}

std::expected<SequentialCoverageSource, std::string> sequential_source_coverage(
    ConnectionAnalysisPlan const& plan,
    SampleSourceChannelTimingPlan const& source)
{
    SequentialCoverageSource result{.read_latency = source.read_latency};
    auto const* node = planned_node_for_bundle(plan, source.source.bundle);
    if (!node) return std::unexpected("Sequential coverage lost its source node");
    if (source.production == PlannedSourceProduction::tick
        && node->full_live_tick_coverage) return result;
    NodeBundlePortId const id{
        source.source.bundle, PortKind::sample, source.source.port};
    auto const port = std::ranges::find_if(plan.background.ports, [&](auto const& port) {
        return port.direction == PortDirection::output && port.configured_port == id;
    });
    if (port == plan.background.ports.end() || !port->accumulators.output_change)
        return std::unexpected("Sequential coverage lost its planned source output");
    result.output_change = port->accumulators.output_change;
    return result;
}

std::expected<std::vector<SequentialCoverageProjection>, std::string>
sequential_connection_coverage(
    ConnectionAnalysisPlan const& plan,
    SampleConnectionPlan const& connection,
    std::optional<bool> realtime_component)
{
    std::vector<SequentialCoverageProjection> result;
    auto append = [&](std::span<std::size_t const> source_indices,
                      std::vector<std::size_t> targets,
                      std::size_t detach_delay) -> std::expected<void, std::string> {
        if (realtime_component && !std::ranges::any_of(source_indices, [&](auto index) {
                return index < connection.source_channel_timings.size()
                    && uses_realtime_storage(connection.source_channel_timings[index].delivery) == *realtime_component;
            })) return {};
        SequentialCoverageProjection projection{.target_channels = std::move(targets)};
        for (auto const index : source_indices) {
            if (index >= connection.source_channel_timings.size())
                return std::unexpected("Sequential coverage has an invalid source channel");
            auto source = sequential_source_coverage(plan, connection.source_channel_timings[index]);
            if (!source) return std::unexpected(std::move(source.error()));
            if (detach_delay > std::numeric_limits<std::size_t>::max() - source->read_latency)
                return std::unexpected("Sequential coverage delay overflows");
            source->read_latency += detach_delay;
            if (!std::ranges::contains(projection.sources, *source))
                projection.sources.push_back(*source);
        }
        result.push_back(std::move(projection));
        return {};
    };
    if (!connection.routed_contributions.empty()) {
        for (auto const& contribution : connection.routed_contributions) {
            std::vector<std::size_t> sources, targets;
            for (std::size_t i = 0; i < contribution.source_coefficients.size(); ++i)
                if (contribution.source_coefficients[i] != 0) sources.push_back(i);
            for (std::size_t i = 0; i < contribution.target_coefficients.size(); ++i)
                if (contribution.target_coefficients[i] != 0)
                    targets.push_back(connection.target_channels[i].channel);
            auto appended = append(sources, std::move(targets),
                contribution.detach ? contribution.detach->loop_extra_latency : 0);
            if (!appended) return std::unexpected(std::move(appended.error()));
        }
    } else if (!connection.projection_contributions.empty()) {
        for (auto const& contribution : connection.projection_contributions) {
            auto appended = append(contribution.source_channel_indices,
                contribution.target_channels,
                connection.detach ? connection.detach->loop_extra_latency : 0);
            if (!appended) return std::unexpected(std::move(appended.error()));
        }
    } else {
        std::vector<std::size_t> sources, targets;
        for (std::size_t i = 0; i < connection.source_channel_timings.size(); ++i)
            sources.push_back(i);
        for (auto const target : connection.target_channels) targets.push_back(target.channel);
        auto appended = append(sources, std::move(targets),
            connection.detach ? connection.detach->loop_extra_latency : 0);
        if (!appended) return std::unexpected(std::move(appended.error()));
    }
    return result;
}

void plan_public_coverage_inputs(ConfiguredGraph const& graph, ResolvedPortRealization const& realization,
    StructuralSampleRouteIndex const& routing, ConnectionAnalysisPlan& connections)
{
    auto& background = connections.background;
    for (NodeBundleHandle bundle = 0; bundle < graph.node_bundles.size(); ++bundle) {
        auto const& node = graph.node_bundles.bundle(bundle);
        if (!node.is_subgraph()) continue;
        for (std::size_t port = 0; port < node.sample_input_count(); ++port) {
            NodeBundlePortId const id{bundle, PortKind::sample, port};
            auto const config = graph.node_bundles.resolve_sample_input(id).config;
            auto const& resolved = realization.input(bundle, port);
            if (input_access(config) == InputAccessKind::coverage && resolved.value_type == ContinuousValueTypeId::sample) {
                background.public_coverage_inputs.push_back({.port = id, .layout = config.channel_layout,
                    .pace_exponent = resolved.absolute_pace.exponent(), .ordering = resolved.ordering});
            }
        }
    }
    if (background.public_coverage_inputs.empty()) return;

    std::vector<ManualSampleRoutePlan> unused_sequential_manual_routes;
    auto routed = build_structural_sample_routes(
        graph, routing, unused_sequential_manual_routes, ConnectionView::effective, true);
    if (!routed) {
        for (auto& input : background.public_coverage_inputs) input.reconstructible = false;
        return;
    }
    std::map<std::pair<NodeBundleHandle, std::size_t>, std::vector<RoutedSampleConnection>> by_input;
    for (auto& entry : *routed) {
        auto const target = entry.connection.target_channels.front();
        by_input[{target.bundle, target.port}].push_back(std::move(entry));
    }
    // Normalize the same routes with the same channel rules as a concrete
    // consumer. This inventory is private to the read and owns no schedule.
    ConnectionAnalysisPlan read_analysis{.boundary_bundle = connections.boundary_bundle, .nodes = connections.nodes};
    std::map<std::pair<NodeBundleHandle, std::size_t>, BackgroundPortIndex> readable_outputs;
    for (auto const output : background.requestable_outputs) {
        auto const id = background.ports[output].configured_port;
        if (id.port_kind == PortKind::sample) readable_outputs.emplace(std::pair{id.node_bundle_handle, id.port_index}, output);
    }
    for (auto& input : background.public_coverage_inputs) {
        auto const routes = by_input.find({input.port.node_bundle_handle, input.port.port_index});
        if (routes == by_input.end()) continue;
        read_analysis.sample_connections.clear();
        read_analysis.dependencies.clear();
        if (!inventory_sample_connections(graph, realization, read_analysis, routes->second)
            || !normalize_sample_target_projections(graph, read_analysis)
            || !validate_detach_delivery_mechanisms(read_analysis)) {
            input.reconstructible = false;
            continue;
        }
        for (auto const& connection : read_analysis.sample_connections) {
            std::vector<PublicCoverageInputView::Source> sources;
            for (auto const& timing : connection.source_channel_timings) {
                auto const found = readable_outputs.find({timing.source.bundle, timing.source.port});
                if (found == readable_outputs.end()) {
                    input.reconstructible = false;
                    break;
                }
                sources.push_back({found->second, timing.source.channel, timing.read_latency});
            }
            if (!input.reconstructible) break;
            if (connection.projection_contributions.empty()) {
                SampleProjectionPlan channels{.source_type = connection.source_type, .target_type = connection.target_type};
                for (auto const channel : connection.target_channels) channels.target_channels.push_back(channel.channel);
                input.projections.push_back({std::move(sources), std::move(channels)});
                continue;
            }
            for (auto const& projection : connection.projection_contributions) {
                auto& result = input.projections.emplace_back();
                for (auto const source : projection.source_channel_indices) result.sources.push_back(sources[source]);
                result.channels = {
                    .source_type = projection.source_type,
                    .target_type = projection.target_type,
                    .target_channels = projection.target_channels,
                    .transform_source_channel_count = projection.transform.source_channel_count,
                    .transform_target_channel_count = projection.transform.target_channel_count,
                    .transform_coefficients = projection.transform.coefficients,
                };
            }
        }
    }
}

void plan_public_event_coverage_inputs(ConfiguredGraph const& graph, BackgroundEvaluationPlan& plan)
{
    for (NodeBundleHandle bundle = 0; bundle < graph.node_bundles.size(); ++bundle) {
        auto const& node = graph.node_bundles.bundle(bundle);
        if (!node.is_subgraph()) continue;
        for (std::size_t index = 0; index < node.event_input_count(); ++index) {
            NodeBundlePortId const port{bundle, PortKind::event, index};
            auto const config = graph.node_bundles.resolve_event_input(port).config;
            if (config.access == InputAccessKind::coverage)
                plan.public_event_coverage_inputs.push_back({.port = port, .type = config.type});
        }
    }
    if (plan.public_event_coverage_inputs.empty()) return;
    auto routes = build_structural_event_routes(graph, true);
    if (!routes) {
        for (auto& input : plan.public_event_coverage_inputs) input.reconstructible = false;
        return;
    }
    for (auto const& route : *routes) {
        for (auto const target : route.connection.targets) {
            auto const input = std::ranges::find(plan.public_event_coverage_inputs,
                NodeBundlePortId{target.bundle, PortKind::event, target.port}, &PublicEventCoverageInputView::port);
            if (input == plan.public_event_coverage_inputs.end()) continue;
            for (auto const& source : route.sources) {
                auto const id = NodeBundlePortId{source.source.port.bundle, PortKind::event, source.source.port.port};
                auto const output = std::ranges::find_if(plan.requestable_outputs,
                    [&](auto port) { return plan.ports[port].configured_port == id; });
                auto const conversion = append_event_conversion(source.conversion, route.conversion);
                if (output == plan.requestable_outputs.end() || !conversion || source.source.detach) {
                    input->reconstructible = false;
                    continue;
                }
                input->sources.push_back({*output, *conversion});
            }
        }
    }
}

namespace {

std::expected<void, std::string> inventory_routed_samples(
    ConfiguredGraph const& graph,
    ResolvedPortRealization const& realization,
    StructuralSampleRouteIndex const& routing,
    ConnectionAnalysisPlan& plan,
    ConnectionView view)
{
    auto routes = [&]() -> std::expected<std::vector<RoutedSampleConnection>, std::string> {
        try {
            return build_structural_sample_routes(graph, routing, plan.manual_sample_routes, view);
        } catch (std::exception const& error) {
            return std::unexpected(
                "GraphJit structural sample routing failed: " + std::string(error.what()));
        }
    }();
    if (!routes) return std::unexpected(std::move(routes.error()));
    if (auto samples = inventory_sample_connections(graph, realization, plan, *routes); !samples)
        return samples;
    return normalize_sample_target_projections(graph, plan);
}

std::expected<void, std::string> inventory_routed_events(
    ConfiguredGraph const& graph,
    ResolvedPortRealization const& realization,
    ConnectionAnalysisPlan& plan)
{
    auto routes = [&]() -> std::expected<std::vector<RoutedEventConnection>, std::string> {
        try {
            return build_structural_event_routes(graph);
        } catch (std::exception const& error) {
            return std::unexpected(
                "GraphJit structural event routing failed: " + std::string(error.what()));
        }
    }();
    if (!routes) return std::unexpected(std::move(routes.error()));
    return inventory_event_connections(graph, realization, plan, *routes);
}

std::expected<void, std::string> plan_connection_semantics(
    ConfiguredGraph const& graph,
    ResolvedPortRealization const& realization,
    ConnectionAnalysisPlan& plan,
    ConnectionView view,
    std::size_t kernel_block_size,
    std::vector<std::size_t>& normalized_connection_indices)
{
    auto background = build_semantic_background_plan(graph, plan);
    if (!background) return std::unexpected(std::move(background.error()));
    plan.background = std::move(*background);
    mark_live_sequential_outputs(graph, plan, view);
    if (auto replay = derive_contextual_replayability(graph, plan); !replay)
        return replay;
    if (auto replay = validate_contextual_replay_realization(graph, realization, plan); !replay)
        return replay;
    // Diagnose background feedback using the whole semantic graph before
    // checking the narrower transport contract for a detach.
    if (auto detaches = validate_detach_delivery_mechanisms(plan); !detaches)
        return detaches;
    return plan_background_connections(graph, realization, plan, plan.background,
        normalized_connection_indices, kernel_block_size, view);
}

std::expected<void, std::string> validate_authored_routes(
    ConfiguredGraph const& graph,
    ResolvedPortRealization const& realization,
    ConnectionAnalysisPlan& plan,
    std::size_t kernel_block_size)
{
    std::vector<std::size_t> normalized_connection_indices;
    if (auto semantics = plan_connection_semantics(graph, realization, plan,
            ConnectionView::authored, kernel_block_size, normalized_connection_indices); !semantics) {
        return semantics;
    }
    if (!is_acyclic(background_evaluation_adjacency(plan.background))) {
        return std::unexpected(
            "GraphJit background evaluation dependency graph contains an unresolved replay cycle");
    }
    if (auto acyclic = validate_explicit_graph_is_acyclic(graph, plan); !acyclic)
        return acyclic;
    return validate_detached_connections(graph, plan);
}

} // namespace

std::expected<ConnectionTopology, std::string> ConnectionTopology::build(
    ConfiguredGraph const& graph,
    ResolvedPortRealization const& port_realization,
    std::size_t kernel_block_size,
    std::span<std::size_t const> callback_batch_classes,
    ConfiguredBundleInventory configured_bundles,
    std::span<std::optional<StableConcreteNodeId> const> identities)
{
    if (kernel_block_size == 0) {
        return std::unexpected(
            "connection analysis requires a non-zero kernel block size");
    }

    for (NodeBundleHandle bundle = 0;
         bundle < graph.node_bundles.size(); ++bundle) {
        auto const& node_bundle = graph.node_bundles.bundle(bundle);
        // Only concrete producers own recorded Tick storage. A structural
        // export can describe the same source with a different channel layout.
        if (!node_bundle.is_concrete()) continue;
        for (std::size_t port = 0;
             port < node_bundle.sample_output_count(); ++port) {
            auto const config = graph.node_bundles.resolve_sample_output(
                {bundle, PortKind::sample, port}).config;
            if (is_sequential_output(config)
                && config.realization == OutputRealization::buffered
                && config.channel_layout.sample_layout
                    != SampleStreamLayout::interleaved) {
                return std::unexpected(
                    "GraphJit persisted Tick continuous output "
                    + std::to_string(bundle) + ":"
                    + std::to_string(port)
                    + " must use interleaved storage");
            }
        }
    }

    ConnectionTopology topology(graph, port_realization, kernel_block_size);
    auto& plan = topology.plan_;
    BundleMigrationIdentities initial_identities;
    if (identities.empty()) {
        initial_identities = assign_bundle_migration_identities(graph, configured_bundles);
        identities = initial_identities;
    }
    if (auto inventory = inventory_nodes(graph, port_realization, plan, configured_bundles, identities); !inventory) {
        return std::unexpected(std::move(inventory.error()));
    }
    auto routing = index_structural_sample_routes(graph);
    if (!routing) return std::unexpected(std::move(routing.error()));

    auto const has_manual_override = std::ranges::any_of(
        graph.manual_controls, &ConfiguredManualControl::override_connections);
    if (has_manual_override) {
        // A live replacement can open a valid feedback loop or hide an
        // invalid authored connection. Validate the original routing without
        // scheduling or allocating storage for it.
        ConnectionAnalysisPlan authored{
            .boundary_bundle = plan.boundary_bundle,
            .nodes = plan.nodes,
        };
        if (auto samples = inventory_routed_samples(
                graph, port_realization, *routing, authored, ConnectionView::authored);
            !samples)
            return std::unexpected(std::move(samples.error()));
        auto const first_event_dependency = authored.dependencies.size();
        if (auto events = inventory_routed_events(graph, port_realization, authored); !events)
            return std::unexpected(std::move(events.error()));

        // Manual controls change Sample routes. Reuse the Event inventory
        // and keep the dependency order as Sample followed by Event.
        plan.event_connections = authored.event_connections;
        auto const event_dependencies = std::span(authored.dependencies).subspan(first_event_dependency);
        if (auto valid = validate_authored_routes(graph, port_realization, authored, kernel_block_size);
            !valid)
            return std::unexpected(std::move(valid.error()));
        if (auto samples = inventory_routed_samples(
                graph, port_realization, *routing, plan, ConnectionView::effective);
            !samples)
            return std::unexpected(std::move(samples.error()));
        plan.dependencies.insert(plan.dependencies.end(), event_dependencies.begin(), event_dependencies.end());
    } else {
        if (auto samples = inventory_routed_samples(
                graph, port_realization, *routing, plan, ConnectionView::effective);
            !samples)
            return std::unexpected(std::move(samples.error()));
        if (auto events = inventory_routed_events(graph, port_realization, plan); !events)
            return std::unexpected(std::move(events.error()));
    }

    if (auto semantics = plan_connection_semantics(graph, port_realization, plan,
            ConnectionView::effective, kernel_block_size, topology.normalized_connection_indices_); !semantics)
        return std::unexpected(std::move(semantics.error()));
    if (auto scheduled = schedule_background_topology(plan.background, callback_batch_classes); !scheduled)
        return std::unexpected(std::move(scheduled.error()));
    plan_public_coverage_inputs(graph, port_realization, *routing, plan);
    plan_public_event_coverage_inputs(graph, plan.background);
    if (auto acyclic = validate_explicit_graph_is_acyclic(graph, plan); !acyclic)
        return std::unexpected(std::move(acyclic.error()));
    if (!has_manual_override) {
        if (auto detaches = validate_detached_connections(graph, plan); !detaches)
            return std::unexpected(std::move(detaches.error()));
    }
    return topology;
}

std::expected<ConnectionAnalysisPlan, std::string> ConnectionTopology::build_analysis_plan(
    std::span<std::size_t const> callback_batch_classes,
    std::span<NodeBundleHandle const> callback_elision_candidates) &&
{
    auto const& graph = graph_;
    auto const& port_realization = port_realization_;
    auto const kernel_block_size = kernel_block_size_;
    auto plan = std::move(plan_);

    if (auto scheduled = plan_realtime_schedule(
            graph, plan, callback_batch_classes, callback_elision_candidates);
        !scheduled) {
        return std::unexpected(std::move(scheduled.error()));
    }
    if (auto in_flight =
            inventory_sequential_input_in_flight_values(graph, plan);
        !in_flight) {
        return std::unexpected(std::move(in_flight.error()));
    }
    if (auto grouped = plan_port_groups(
            graph, port_realization, plan, kernel_block_size); !grouped) {
        return std::unexpected(std::move(grouped.error()));
    }
    auto background_storage = build_background_storage_plan(
        plan, normalized_connection_indices_);
    if (!background_storage) {
        return std::unexpected(std::move(background_storage.error()));
    }
    plan.background.storage = std::move(*background_storage);
    if (auto runtime = finalize_background_runtime_plan(plan.background);
        !runtime) {
        return std::unexpected(std::move(runtime.error()));
    }
    if (auto tick_runtime = finalize_tick_runtime_plan(
            plan.background, plan.schedule, kernel_block_size);
        !tick_runtime) {
        return std::unexpected(std::move(tick_runtime.error()));
    }
    for (auto& requirement : plan.realtime_port_states.states) {
        auto const* node = planned_node_for_bundle(plan, requirement.configured_port.node_bundle_handle);
        if (!node || !node->resolved_module_input) continue;
        auto const& origin = *node->resolved_module_input;
        requirement.resolved_module_input = origin;
        requirement.stable_identity = StableRealtimePortStateId{
            .node = origin.node, .direction = requirement.direction,
            .kind = origin.kind, .port_name = origin.port_name,
            .port_index = origin.port_index, .channel = requirement.channel,
            .role = requirement.role,
        };
    }
    return plan;
}

std::expected<ConnectionAnalysisPlan, std::string> build_connection_analysis_plan(
    ConfiguredGraph const& graph,
    ResolvedPortRealization const& port_realization,
    std::size_t kernel_block_size,
    std::span<std::size_t const> callback_batch_classes,
    std::span<NodeBundleHandle const> callback_elision_candidates)
{
    auto topology = ConnectionTopology::build(
        graph, port_realization, kernel_block_size, callback_batch_classes);
    if (!topology) return std::unexpected(std::move(topology.error()));
    return std::move(*topology).build_analysis_plan(
        callback_batch_classes, callback_elision_candidates);
}

std::expected<std::string, std::string> input_timeline_migration_identity(
    RealtimePortStateRequirements const& requirements,
    NodeBundlePortId configured_port,
    std::string_view prefix)
{
    std::optional<StableRealtimePortStateId> selected;
    bool found_anonymous = false;
    for (auto const& requirement : requirements.states) {
        if (requirement.configured_port != configured_port
            || requirement.direction != PortDirection::input
            || requirement.role
                != RealtimePortStateRole::sequential_input_history) {
            continue;
        }
        if (!requirement.stable_identity) {
            found_anonymous = true;
            continue;
        }
        auto const& identity = *requirement.stable_identity;
        if (identity.direction != PortDirection::input
            || identity.kind != configured_port.port_kind
            || (requirement.resolved_module_input
                ? identity.port_index != requirement.resolved_module_input->port_index
                : identity.port_index && *identity.port_index != configured_port.port_index)
            || identity.role
                != RealtimePortStateRole::sequential_input_history) {
            return std::unexpected(
                "GraphJit input timeline has inconsistent stable port identity");
        }
        if (selected
            && (selected->node != identity.node
                || selected->direction != identity.direction
                || selected->kind != identity.kind
                || selected->port_name != identity.port_name
                || selected->port_index != identity.port_index
                || selected->role != identity.role)) {
            return std::unexpected(
                "GraphJit input timeline combines different stable ports");
        }
        selected = identity;
    }
    if (found_anonymous && selected) {
        return std::unexpected(
            "GraphJit input timeline mixes stable and anonymous state identities");
    }
    if (!selected) return std::string{};

    auto const& identity = *selected;
    std::ostringstream out;
    out << prefix << ':' << migration_identity_fields(identity.node)
        << ":direction=" << static_cast<unsigned>(identity.direction)
        << ":kind=" << static_cast<unsigned>(identity.kind)
        << migration_port_identity_fields(identity.port_name, identity.port_index)
        << ":role=" << static_cast<unsigned>(identity.role);
    return std::move(out).str();
}

std::optional<std::string> raw_region_node_identity(
    ConnectionAnalysisPlan const& connections,
    NodeBundleHandle bundle)
{
    auto const node = std::ranges::find(
        connections.nodes, bundle, &PlannedGraphNode::bundle);
    if (node == connections.nodes.end()) return std::nullopt;
    if (node->resolved_module_input) {
        auto const& origin = *node->resolved_module_input;
        return "{module-input:" + migration_identity_fields(origin.node)
            + ":kind=" + std::to_string(static_cast<unsigned>(origin.kind))
            + migration_port_identity_fields(origin.port_name, origin.port_index) + '}';
    }
    if (!node->stable_identity) return std::nullopt;
    return '{' + migration_identity_fields(*node->stable_identity) + '}';
}

namespace {
// Cannot occur in a stable identity, whose fields are printable text.
constexpr std::string_view anonymous_raw_region_node = "\x01" "anonymous" "\x01";
} // namespace

std::string raw_region_node_identity_text(
    ConnectionAnalysisPlan const& connections,
    NodeBundleHandle bundle)
{
    auto identity = raw_region_node_identity(connections, bundle);
    return identity ? std::move(*identity)
                    : std::string{anonymous_raw_region_node};
}

std::string owned_raw_region_identity(std::string identity)
{
    if (identity.find(anonymous_raw_region_node) != std::string::npos) {
        return {};
    }
    return identity;
}

std::vector<std::string> resolve_detach_identities(
    std::span<DetachIdentityParts const> detaches)
{
    std::vector<std::string> result(detaches.size());
    std::vector<bool> resolved(detaches.size(), false);
    // Resolves every remaining detach whose key at this level no other
    // remaining detach shares. Keys of different levels never compare equal.
    auto const settle = [&](auto key_of) {
        std::map<std::string, std::size_t> counts;
        for (std::size_t index = 0; index < detaches.size(); ++index) {
            if (resolved[index]) continue;
            if (auto key = key_of(detaches[index])) ++counts[*key];
        }
        for (std::size_t index = 0; index < detaches.size(); ++index) {
            if (resolved[index]) continue;
            auto key = key_of(detaches[index]);
            if (!key || counts[*key] != 1) continue;
            result[index] = std::move(*key);
            resolved[index] = true;
        }
    };
    auto const producer = [](DetachIdentityParts const& parts)
        -> std::optional<std::string> {
        if (!parts.producer) return std::nullopt;
        return "producer=" + *parts.producer;
    };
    auto const consumer = [&](DetachIdentityParts const& parts)
        -> std::optional<std::string> {
        if (!parts.consumer) return std::nullopt;
        return producer(parts).value_or(std::string{})
            + ":consumer=" + *parts.consumer;
    };
    auto const settings = [&](DetachIdentityParts const& parts)
        -> std::optional<std::string> {
        auto base = consumer(parts);
        if (!base) base = producer(parts);
        if (!base) return std::nullopt;
        return *base + ":settings=" + parts.settings;
    };
    settle(producer);
    settle(consumer);
    settle(settings);
    return result;
}

std::optional<std::string> detach_identity_port(
    ConnectionAnalysisPlan const& connections,
    NodeBundleHandle bundle,
    std::size_t port_index,
    std::optional<std::size_t> channel)
{
    auto node = raw_region_node_identity(connections, bundle);
    if (!node) return std::nullopt;
    auto result = std::move(*node) + '.' + std::to_string(port_index);
    if (channel) result += '.' + std::to_string(*channel);
    return result;
}

std::string detach_identity_settings(
    std::size_t latency,
    std::optional<std::vector<std::byte>> const& initial_value)
{
    std::ostringstream out;
    out << "latency=" << latency << ":initial=";
    if (initial_value) {
        for (auto value : *initial_value) {
            out << std::hex << static_cast<unsigned>(value) << '.';
        }
    }
    return std::move(out).str();
}

} // namespace iv::graph_jit::detail
