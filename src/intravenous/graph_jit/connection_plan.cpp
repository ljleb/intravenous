#include <intravenous/graph_jit/connection_plan.h>

#include <intravenous/graph_jit/background_storage_plan.h>

#include <intravenous/channel_layout.h>
#include <intravenous/ports.h>
#include <intravenous/sample.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <limits>
#include <iterator>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace iv::graph_jit::detail {
namespace {

std::size_t saturating_add(std::size_t a, std::size_t b) noexcept
{
    auto const max = std::numeric_limits<std::size_t>::max();
    return b > max - a ? max : a + b;
}

std::size_t retained_extent(
    std::size_t source_history,
    std::size_t read_latency,
    std::size_t target_history) noexcept
{
    return saturating_add(
        std::max(source_history, target_history), read_latency);
}

std::expected<std::size_t, std::string> checked_latency_add(
    std::size_t a,
    std::size_t b,
    std::string_view context)
{
    if (b > std::numeric_limits<std::size_t>::max() - a) {
        return std::unexpected(std::string(context) + " overflows size_t");
    }
    return a + b;
}

bool is_internal_bundle(
    NodeBundleHandle bundle,
    NodeBundleHandle boundary) noexcept
{
    return bundle != boundary;
}

PlannedSourceProduction source_production(
    OutputProductionConfig const& production) noexcept
{
    return is_tick(production)
        ? PlannedSourceProduction::tick
        : PlannedSourceProduction::tock;
}

PlannedDestinationAccess destination_access(
    InputAccessConfig const& access) noexcept
{
    return is_sequential(access)
        ? PlannedDestinationAccess::sequential
        : PlannedDestinationAccess::random_access;
}

std::expected<PlannedDeliveryMechanism, std::string> connection_delivery(
    PlannedSourceProduction production,
    OutputRetention retention,
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
    if (retention == OutputRetention::persisted) {
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
    return node && node->intrinsically_replayable;
}

std::expected<void, std::string> inventory_nodes(
    ConfiguredGraph const& graph,
    ConnectionAnalysisPlan& plan)
{
    plan.boundary_bundle = graph.public_ports.boundary_handle();
    // A default-constructed ConfiguredGraph is the shell's semantic empty
    // identity and intentionally has no materialized boundary bundle.
    if (graph.node_bundles.size() == 0) return {};

    std::size_t boundary_count = 0;
    std::size_t bundle = 0;
    std::string error;
    graph.node_bundles.for_each_configured_bundle(
        [&](ConfiguredNodeBundleView const& view) {
            auto const current = bundle++;
            if (!error.empty()) return;

            if (view.kind == ConfiguredNodeBundleKind::boundary) {
                ++boundary_count;
                if (current != plan.boundary_bundle) {
                    error = "connection analysis found a boundary that is not the project boundary";
                }
                return;
            }
            if (view.kind != ConfiguredNodeBundleKind::concrete) {
                error =
                    "connection analysis does not yet support tiled/subgraph bundles";
                return;
            }
            if (!view.ports) {
                error = "concrete bundle has no port metadata during connection analysis";
                return;
            }
            plan.nodes.push_back(PlannedGraphNode{
                .bundle = current,
                .internal_latency_samples = view.internal_latency_samples,
                .maximum_block_size = view.maximum_block_size,
                .sample_input_count = view.ports->sample_input_count(),
                .sample_output_count = view.ports->sample_output_count(),
                .event_input_count = view.ports->event_input_count(),
                .event_output_count = view.ports->event_output_count(),
                .intrinsically_replayable = view.intrinsically_replayable,
            });
        });

    if (!error.empty()) return std::unexpected(std::move(error));
    if (boundary_count != 1 || plan.boundary_bundle >= graph.node_bundles.size()) {
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
    OutputRetention retention,
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
                && edge.source_retention == retention
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
            .source_retention = retention,
            .destination_access = access,
            .delivery = delivery,
            .configured_connection_index = connection_index,
            .sequential_tick_dependency =
                delivery == PlannedDeliveryMechanism::tick_to_sequential,
        });
    }
}

std::expected<void, std::string> inventory_sample_connections(
    ConfiguredGraph const& graph,
    ConnectionAnalysisPlan& plan)
{
    auto const connections = graph.connections.configured_sample_connections();
    plan.sample_connections.reserve(connections.size());
    for (std::size_t i = 0; i < connections.size(); ++i) {
        auto const& connection = connections[i];
        if (connection.source_channels.empty() || connection.target_channels.empty()) {
            return std::unexpected(
                "configured sample connection has an empty port set");
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

        SampleConnectionPlan connection_plan{
            .configured_connection_index = i,
            .source_type = connection.source_type,
            .source_channels = connection.source_channels,
            .target_type = connection.target_type,
            .target_channels = connection.target_channels,
            .target_port = NodeBundlePortId{
                first_target.bundle, PortKind::sample, first_target.port},
        };

        try {
            connection_plan.canonical_source_port =
                graph.node_bundles.sample_output_port_for_channels(
                    connection.source_type, connection.source_channels);
            auto const target = graph.node_bundles.resolve_sample_input(
                connection_plan.target_port).config;
            connection_plan.target_layout = target.channel_layout;
            connection_plan.target_history = port_history_or_zero(target);
            if (connection.detach) {
                if (connection.detach->loop_extra_latency == 0)
                    return std::unexpected(
                        "GraphJit sample detach latency must be at least one sample");
                connection_plan.detach = connection.detach;
                connection_plan.detach_initial_value =
                    connection.detach->initial_value_override.value_or(
                        target.neutral_value);
            }
            auto const target_access = destination_access(target.access);
            connection_plan.destination_access = target_access;

            std::optional<ChannelLayout> canonical_source_layout;
            connection_plan.source_channel_timings.reserve(
                connection.source_channels.size());
            for (auto const source_channel : connection.source_channels) {
                NodeBundlePortId const source_port{
                    source_channel.bundle, PortKind::sample, source_channel.port};
                auto const source =
                    graph.node_bundles.resolve_sample_output(source_port).config;
                auto const source_history = port_history_or_zero(source);
                auto const source_latency = tick_latency_or_zero(source);
                auto const production = source_production(source.production);
                auto delivery = connection_delivery(
                    production,
                    source.retention,
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
                        .production = production,
                        .retention = source.retention,
                        .destination_access = target_access,
                        .delivery = *delivery,
                        .source_history = source_history,
                        .source_latency = source_latency,
                        .read_latency = source_latency,
                    });
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
            connection_plan.read_latency = connection_plan.source_latency;
            if (!connection.detach) {
                for (auto const& source : connection_plan.source_channel_timings) {
                    append_dependency(
                        plan,
                        source.source.bundle,
                        first_target.bundle,
                        PlannedConnectionData::sample,
                        source.production,
                        source.retention,
                        source.destination_access,
                        source.delivery,
                        i);
                }
            }
            connection_plan.external_boundary =
                first_target.bundle == plan.boundary_bundle
                || std::ranges::any_of(
                    connection.source_channels,
                    [&](SampleOutputChannelId source) {
                        return source.bundle == plan.boundary_bundle;
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
                !connection_plan.canonical_source_port
                || connection.source_type != connection.target_type
                || !canonical_source_layout
                || *canonical_source_layout != semantic_target_layout;
            if (connection_plan.requires_conversion) {
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
            && plan.sample_connections[first_index].canonical_source_port) {
            consumed[first_index] = true;
            normalized.push_back(std::move(plan.sample_connections[first_index]));
            continue;
        }

        auto const target_channel_total = canonical_targets.size();
        std::vector<bool> claimed(target_channel_total, false);

        auto const& first = plan.sample_connections[first_index];
        auto target_history = first.target_history;
        auto detach = first.detach;
        auto detach_initial_value = first.detach_initial_value;
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
                || connection.target_history != target_history
                || connection.destination_access != first.destination_access
                || connection.detach != detach) {
                return std::unexpected(
                    "GraphJit sample target-channel projections disagree on target semantics");
            }
            if (connection.source_channels.size()
                    != channel_count(connection.source_type)
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
            };
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
                if (claimed[target_index]) {
                    return std::unexpected(
                        "GraphJit sample target channel has more than one source");
                }
                claimed[target_index] = true;
                contribution.target_channels.push_back(target_index);
            }

            // inventory_sample_connections() already validates the configured
            // conversion. Re-check the normalized semantic boundary here so a
            // future producer-layout change cannot silently alter composition.
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

        merged.read_latency = merged.source_latency;
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

std::expected<void, std::string> inventory_event_connections(
    ConfiguredGraph const& graph,
    ConnectionAnalysisPlan& plan)
{
    auto const connections = graph.connections.configured_event_connections();
    plan.event_connections.reserve(connections.size());
    for (std::size_t i = 0; i < connections.size(); ++i) {
        auto const& connection = connections[i];
        if (connection.sources.empty() || connection.targets.empty()) {
            return std::unexpected(
                "configured event connection has an empty port set");
        }
        EventConnectionPlan connection_plan{
            .configured_connection_index = i,
            .source_type = connection.source_type,
            .sources = connection.sources,
            .target_type = connection.target_type,
            .targets = connection.targets,
        };

        try {
            connection_plan.source_plans.reserve(connection.sources.size());
            for (auto const source_id : connection.sources) {
                NodeBundlePortId const source_port{
                    source_id.bundle, PortKind::event, source_id.port};
                auto const source =
                    graph.node_bundles.resolve_event_output(source_port).config;
                auto const history = port_history_or_zero(source);
                auto const latency = tick_latency_or_zero(source);
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
                    .production = source_production(source.production),
                    .retention = source.retention,
                    .history = history,
                    .latency = latency,
                    .max_events_per_index = source.max_events_per_index,
                });
            }

            connection_plan.target_plans.reserve(connection.targets.size());
            for (auto const target_id : connection.targets) {
                NodeBundlePortId const target_port{
                    target_id.bundle, PortKind::event, target_id.port};
                auto const target =
                    graph.node_bundles.resolve_event_input(target_port).config;
                auto const history = port_history_or_zero(target);
                connection_plan.target_history = std::max(
                    connection_plan.target_history, history);
                connection_plan.target_plans.push_back(EventTargetPlan{
                    .target = target_id,
                    .access = destination_access(target.access),
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
                        source.retention,
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

            if (connection.detach) {
                if (connection.detach->loop_extra_latency == 0) {
                    return std::unexpected(
                        "GraphJit event detach latency must be at least one sample");
                }
                connection_plan.detach = connection.detach;
            } else {
                for (auto const& delivery : connection_plan.deliveries) {
                    auto const& source =
                        connection_plan.source_plans[delivery.source_index];
                    auto const& target =
                        connection_plan.target_plans[delivery.target_index];
                    append_dependency(
                        plan,
                        source.source.bundle,
                        target.target.bundle,
                        PlannedConnectionData::event,
                        source.production,
                        source.retention,
                        target.access,
                        delivery.mechanism,
                        i);
                }
            }
            connection_plan.external_boundary =
                std::ranges::any_of(
                    connection.sources,
                    [&](EventOutputPortId source) {
                        return source.bundle == plan.boundary_bundle;
                    })
                || std::ranges::any_of(
                    connection.targets,
                    [&](EventInputPortId target) {
                        return target.bundle == plan.boundary_bundle;
                    });
            connection_plan.conversion = EventConversionRegistry::instance().plan(
                connection.source_type, connection.target_type);
            if (!EventConversionRegistry::is_nonexpanding(
                    connection_plan.conversion)) {
                return std::unexpected(
                    "implicit event conversion must be non-expanding");
            }
            connection_plan.requires_conversion =
                connection_plan.conversion.size() != 0
                || connection.sources.size() != 1;
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
    OutputRetention retention = OutputRetention::ephemeral;
    std::size_t configured_connection_index = 0;
};

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
                .retention = source.retention,
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
                .retention = source.retention,
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
        if (!node->intrinsically_replayable) {
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
                || dependency.retention == OutputRetention::persisted) {
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
            if (!source->intrinsically_replayable) {
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

std::expected<void, std::string> validate_explicit_graph_is_acyclic(
    ConfiguredGraph const& graph,
    ConnectionAnalysisPlan const& plan)
{
    auto const outgoing = dependency_adjacency(graph, plan, false);
    std::vector<std::size_t> indegree(outgoing.size(), 0);
    for (auto const& targets : outgoing) {
        for (auto const target : targets) ++indegree[target];
    }
    std::vector<std::size_t> ready;
    for (std::size_t node = 0; node < indegree.size(); ++node) {
        if (indegree[node] == 0) ready.push_back(node);
    }
    std::size_t visited = 0;
    for (std::size_t i = 0; i < ready.size(); ++i) {
        ++visited;
        for (auto const target : outgoing[ready[i]]) {
            if (--indegree[target] == 0) ready.push_back(target);
        }
    }
    if (visited != outgoing.size()) {
        return std::unexpected(
            "GraphJit graph contains an implicit cycle; use detach() to break feedback explicitly");
    }
    return {};
}

std::expected<void, std::string> validate_detach_delivery_mechanisms(
    ConnectionAnalysisPlan const& plan)
{
    for (auto const& connection : plan.sample_connections) {
        if (connection.detach && !is_entirely_realtime_delivery(connection)) {
            return std::unexpected(
                "GraphJit sample detach requires Tick -> Sequential transport");
        }
    }
    for (auto const& connection : plan.event_connections) {
        if (connection.detach && !is_entirely_realtime_delivery(connection)) {
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

std::vector<NodeBundleHandle> unique_source_bundles(
    std::span<EventOutputPortId const> sources)
{
    std::vector<NodeBundleHandle> bundles;
    bundles.reserve(sources.size());
    for (auto const source : sources) bundles.push_back(source.bundle);
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
        if (!connection.detach) continue;
        for (auto const source : unique_source_bundles(
                 connection.source_channels)) {
            append_edge(source, connection.target_port.node_bundle_handle);
        }
    }
    for (auto const& connection : plan.event_connections) {
        if (!connection.detach) continue;
        for (auto const source : unique_source_bundles(connection.sources)) {
            for (auto const target : connection.targets) {
                append_edge(source, target.bundle);
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

std::expected<void, std::string> populate_background_topology(
    ConfiguredGraph const& graph,
    ConnectionAnalysisPlan const& connections,
    BackgroundEvaluationPlan& plan)
{
    auto stable_node_identity = [&](NodeBundleHandle bundle)
        -> std::optional<StableConcreteNodeId> {
        auto const& configured_bundle = graph.node_bundles.bundle(bundle);
        std::optional<StableConcreteNodeId> selected;
        for (auto const handle : configured_bundle.virtual_node_handles()) {
            auto const& record = graph.virtual_nodes.record(handle);
            if (record.type_identity != configured_bundle.type_identity()) continue;
            auto const member = std::ranges::find(
                record.node_bundle_handles, bundle);
            if (member == record.node_bundle_handles.end()) continue;
            StableConcreteNodeId candidate{
                .graph = graph.identity.value,
                .virtual_node = record.id,
                .direct_member = static_cast<std::size_t>(
                    member - record.node_bundle_handles.begin()),
            };
            if (!selected
                || std::tie(candidate.virtual_node, candidate.direct_member)
                    < std::tie(
                        selected->virtual_node, selected->direct_member)) {
                selected = std::move(candidate);
            }
        }
        return selected;
    };

    plan.intrinsic_replay_candidates.clear();
    for (auto const& node : connections.nodes) {
        if (node.intrinsically_replayable
            && is_internal_bundle(node.bundle, connections.boundary_bundle)) {
            plan.intrinsic_replay_candidates.push_back(node.bundle);
        }
    }

    std::vector<bool> needs_background_node(graph.node_bundles.size(), false);
    auto require_bundle = [&](NodeBundleHandle bundle) {
        if (bundle < needs_background_node.size()
            && is_internal_bundle(bundle, connections.boundary_bundle)) {
            needs_background_node[bundle] = true;
        }
    };

    for (auto const& node : connections.nodes) {
        auto const bundle = node.bundle;
        bool relevant = node.contextually_replayable;
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
            relevant = relevant || is_tock(config)
                || config.retention == OutputRetention::persisted;
        }
        for (std::size_t port = 0; port < node.event_output_count; ++port) {
            auto const config = graph.node_bundles.resolve_event_output(
                {bundle, PortKind::event, port}).config;
            relevant = relevant || is_tock(config)
                || config.retention == OutputRetention::persisted;
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

        bool authored_tock = false;
        for (std::size_t port = 0; port < node.sample_output_count; ++port) {
            authored_tock = authored_tock || is_tock(
                graph.node_bundles.resolve_sample_output(
                    {node.bundle, PortKind::sample, port}).config);
        }
        for (std::size_t port = 0; port < node.event_output_count; ++port) {
            authored_tock = authored_tock || is_tock(
                graph.node_bundles.resolve_event_output(
                    {node.bundle, PortKind::event, port}).config);
        }

        plan.nodes.push_back(BackgroundNodePlan{
            .bundle = node.bundle,
            .semantic_node = semantic_node,
            .semantic_scc = plan.semantic_nodes[semantic_node].scc,
            .stable_identity = stable_node_identity(node.bundle),
            .authored_tock_execution = authored_tock,
            .replays_tick = node.contextually_replayable,
            .uses_replay_forward_coverage = node.contextually_replayable,
            .uses_replay_reverse_coverage = node.contextually_replayable,
            .uses_imported_tick_block_for_replay = node.contextually_replayable,
        });
    }

    struct PortRoles {
        bool random_access_input = false;
        bool tick_sequential_input = false;
        bool replay_sequential_input = false;
        bool authored_tock_output = false;
        bool persisted_tick_output = false;
        bool replayed_tick_output = false;
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
        std::optional<OutputRetention> retention;
        ChannelLayout sample_layout{};
        EventTypeId event_type = EventTypeId::empty;
        double max_events_per_index = 0.0;
        Sample neutral{};
        if (direction == PortDirection::input) {
            if (port.port_kind == PortKind::sample) {
                auto const config = graph.node_bundles.resolve_sample_input(port).config;
                name = config.name;
                sample_layout = config.channel_layout;
                neutral = config.neutral_value;
            } else {
                auto const config = graph.node_bundles.resolve_event_input(port).config;
                name = config.name;
                event_type = config.type;
            }
        } else {
            if (port.port_kind == PortKind::sample) {
                auto const config = graph.node_bundles.resolve_sample_output(port).config;
                name = config.name;
                retention = config.retention;
                sample_layout = config.channel_layout;
            } else {
                auto const config = graph.node_bundles.resolve_event_output(port).config;
                name = config.name;
                retention = config.retention;
                event_type = config.type;
                max_events_per_index = config.max_events_per_index;
            }
        }

        PortAccumulatorIndices indices;
        if (direction == PortDirection::input
            && (roles.random_access_input || roles.replay_sequential_input)) {
            indices = allocate_input_accumulators(node_plan);
        } else if (direction == PortDirection::output) {
            indices = allocate_output_accumulators(node_plan);
        }

        auto const port_index = plan.ports.size();
        std::optional<StableOutputPortId> stable_output;
        if (direction == PortDirection::output
            && node_plan.stable_identity) {
            stable_output = StableOutputPortId{
                .node = *node_plan.stable_identity,
                .kind = port.port_kind,
                .port_name = name,
                .port_index = port.port_index,
            };
        }
        plan.ports.push_back(BackgroundPortPlan{
            .node = background_node,
            .configured_port = port,
            .kind = port.port_kind,
            .direction = direction,
            .name = std::move(name),
            .random_access_input = roles.random_access_input,
            .tick_sequential_input = roles.tick_sequential_input,
            .replay_sequential_input = roles.replay_sequential_input,
            .authored_tock_output = roles.authored_tock_output,
            .persisted_tick_output = roles.persisted_tick_output,
            .replayed_tick_output = roles.replayed_tick_output,
            .sample_neutral_value = neutral,
            .retention = retention,
            .stable_identity = std::move(stable_output),
            .sample_layout = sample_layout,
            .event_type = event_type,
            .max_events_per_index = max_events_per_index,
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
            plan.requestable_outputs.push_back(port_index);
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
            };
            if (!roles.random_access_input && !roles.replay_sequential_input) continue;
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
            };
            if (!roles.random_access_input && !roles.replay_sequential_input) continue;
            auto port_index = ensure_port(
                id, PortDirection::input, roles);
            if (!port_index) return std::unexpected(std::move(port_index.error()));
        }
        for (std::size_t port = 0; port < node.sample_output_count; ++port) {
            NodeBundlePortId const id{bundle, PortKind::sample, port};
            auto const config = graph.node_bundles.resolve_sample_output(id).config;
            PortRoles roles{
                .authored_tock_output = is_tock(config),
                .persisted_tick_output = is_tick(config.production)
                    && config.retention == OutputRetention::persisted,
                .replayed_tick_output = is_tick(config.production)
                    && config.retention == OutputRetention::ephemeral
                    && node.contextually_replayable,
            };
            if (!roles.authored_tock_output && !roles.persisted_tick_output
                && !roles.replayed_tick_output) continue;
            auto port_index = ensure_port(
                id, PortDirection::output, roles);
            if (!port_index) return std::unexpected(std::move(port_index.error()));
        }
        for (std::size_t port = 0; port < node.event_output_count; ++port) {
            NodeBundlePortId const id{bundle, PortKind::event, port};
            auto const config = graph.node_bundles.resolve_event_output(id).config;
            PortRoles roles{
                .authored_tock_output = is_tock(config),
                .persisted_tick_output = is_tick(config.production)
                    && config.retention == OutputRetention::persisted,
                .replayed_tick_output = is_tick(config.production)
                    && config.retention == OutputRetention::ephemeral
                    && node.contextually_replayable,
            };
            if (!roles.authored_tock_output && !roles.persisted_tick_output
                && !roles.replayed_tick_output) continue;
            auto port_index = ensure_port(
                id, PortDirection::output, roles);
            if (!port_index) return std::unexpected(std::move(port_index.error()));
        }
    }

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
    auto append_connection = [&](BackgroundConnectionPlan connection) {
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
    auto background_node_for_bundle = [&](NodeBundleHandle bundle)
        -> std::optional<BackgroundNodeIndex> {
        if (bundle >= plan.bundle_to_background_node.size()) return std::nullopt;
        return plan.bundle_to_background_node[bundle];
    };

    for (auto const& connection : connections.sample_connections) {
        auto const target_bundle = connection.target_port.node_bundle_handle;
        auto const target_replay = node_is_contextually_replayable(target_bundle);
        auto const has_background_delivery = std::ranges::any_of(
            connection.source_channel_timings,
            [](SampleSourceChannelTimingPlan const& source) {
                return !uses_realtime_storage(source.delivery);
            });
        if (!has_background_delivery && !target_replay) continue;

        bool tick_sequential_target = std::ranges::any_of(
            connection.source_channel_timings,
            [](SampleSourceChannelTimingPlan const& source) {
                return source.delivery
                    == PlannedDeliveryMechanism::tock_to_sequential;
            });
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
        background.sample_deliveries.reserve(
            connection.source_channel_timings.size());

        for (auto const& source : connection.source_channel_timings) {
            background.sample_deliveries.push_back(source.delivery);
            auto const background_source = !uses_realtime_storage(source.delivery)
                || target_replay;
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
            });
        }
        append_connection(std::move(background));

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
                    && source.retention == OutputRetention::persisted,
            });
        }
    }

    for (auto const& connection : connections.event_connections) {
        bool any_relevant = false;
        for (auto const& delivery : connection.deliveries) {
            auto const& target = connection.target_plans[delivery.target_index];
            any_relevant = any_relevant
                || !uses_realtime_storage(delivery.mechanism)
                || node_is_contextually_replayable(target.target.bundle);
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
            auto const relevant = !uses_realtime_storage(delivery.mechanism)
                || target_replay;

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
            if (relevant && is_internal_bundle(
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
            background.event_deliveries.push_back(graph_jit::EventDeliveryPlan{
                .source = source.source,
                .target = target.target,
                .source_port = source_port,
                .target_port = target_port,
                .mechanism = delivery.mechanism,
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
                    && source.retention == OutputRetention::persisted,
            });
        }
        append_connection(std::move(background));
    }

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
    while (!ready_nodes.empty()) {
        auto const node = ready_nodes.front();
        ready_nodes.erase(ready_nodes.begin());
        background_order.push_back(node);
        for (auto const target : background_outgoing[node]) {
            if (--indegree[target] == 0) insert_ready_node(target);
        }
    }
    if (background_order.size() != plan.nodes.size()) {
        return std::unexpected(
            "GraphJit background coverage dependency graph is cyclic after semantic SCC validation");
    }

    // The background evaluation DAG is deliberately distinct from semantic SCC
    // membership and from coverage propagation. Stored Tick boundaries terminate
    // traversal, while authored Tock and compiler-provided replay nodes participate in
    // the executable order.
    std::vector<std::vector<BackgroundNodeIndex>> evaluation_outgoing(
        plan.nodes.size());
    std::vector<std::size_t> background_indegree(plan.nodes.size(), 0);
    auto executes_in_background = [&](BackgroundNodeIndex node) {
        return plan.nodes[node].authored_tock_execution
            || plan.nodes[node].replays_tick;
    };
    for (auto const& dependency : plan.background_dependencies) {
        if (dependency.source_is_stored_boundary) continue;
        if (!executes_in_background(dependency.source_node)
            || !executes_in_background(dependency.target_node)) {
            continue;
        }
        evaluation_outgoing[dependency.source_node].push_back(
            dependency.target_node);
    }
    for (auto& targets : evaluation_outgoing) {
        std::ranges::sort(targets);
        targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
        for (auto const target : targets) ++background_indegree[target];
    }
    std::vector<BackgroundNodeIndex> background_ready;
    auto insert_background_ready = [&](BackgroundNodeIndex node) {
        background_ready.insert(
            std::ranges::lower_bound(background_ready, node), node);
    };
    std::size_t executable_count = 0;
    for (BackgroundNodeIndex node = 0; node < plan.nodes.size(); ++node) {
        if (!executes_in_background(node)) continue;
        ++executable_count;
        if (background_indegree[node] == 0) insert_background_ready(node);
    }
    while (!background_ready.empty()) {
        auto const node = background_ready.front();
        background_ready.erase(background_ready.begin());
        plan.background_evaluation_order.push_back(node);
        for (auto const target : evaluation_outgoing[node]) {
            if (--background_indegree[target] == 0) {
                insert_background_ready(target);
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
        if (!connection.detach) continue;
        auto sources = unique_source_bundles(connection.source_channels);
        std::array<NodeBundleHandle, 1> targets{
            connection.target_port.node_bundle_handle};
        if (auto result = validate(
                sources, targets, "sample",
                connection.configured_connection_index);
            !result) return result;
    }
    for (auto const& connection : plan.event_connections) {
        if (!connection.detach) continue;
        auto sources = unique_source_bundles(connection.sources);
        std::vector<NodeBundleHandle> targets;
        targets.reserve(connection.targets.size());
        for (auto const target : connection.targets)
            targets.push_back(target.bundle);
        std::ranges::sort(targets);
        targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
        if (auto result = validate(
                sources, targets, "event",
                connection.configured_connection_index);
            !result) return result;
    }
    return {};
}

std::size_t floor_power_of_two(std::size_t value) noexcept
{
    std::size_t result = 1;
    while (result <= value / 2) result *= 2;
    return result;
}

std::expected<SchedulePlan, std::string> build_schedule(
    ConfiguredGraph const& graph,
    ConnectionAnalysisPlan const& plan)
{
    SchedulePlan schedule;
    schedule.bundle_to_region.resize(graph.node_bundles.size());
    schedule.bundle_execution_position.resize(graph.node_bundles.size());
    if (plan.nodes.empty()) return schedule;

    // Ordinary tick dependencies remain acyclic. Semantic SCC membership and
    // condensation order were already computed with every authored detach
    // restored while building BackgroundEvaluationPlan; schedule formation reuses those
    // facts and only derives the within-SCC same-slice order here.
    auto const explicit_outgoing = dependency_adjacency(graph, plan, true);
    if (plan.background.semantic_nodes.size() != plan.nodes.size()
        || plan.background.semantic_sccs.empty()) {
        return std::unexpected(
            "GraphJit schedule is missing retained semantic SCC facts");
    }
    schedule.regions.reserve(plan.background.semantic_sccs.size());
    std::vector<std::size_t> node_to_region(plan.nodes.size(), 0);
    for (auto const& semantic_scc : plan.background.semantic_sccs) {
        auto const& component = semantic_scc.nodes;
        auto const region_index = schedule.regions.size();
        SccRegionPlan region;
        region.maximum_block_size = std::numeric_limits<std::size_t>::max();
        for (auto const node : component) {
            node_to_region[node] = region_index;
            region.nodes.push_back(plan.nodes[node].bundle);
            region.maximum_block_size = std::min(
                region.maximum_block_size,
                plan.nodes[node].maximum_block_size);
        }
        region.cyclic = semantic_scc.cyclic;

        // A detached dependency is a storage/temporal dependency, not a
        // same-slice tick dependency. Topologically order each region using
        // only ordinary sequential dependencies.
        std::vector<std::size_t> local_indegree(plan.nodes.size(), 0);
        for (auto const node : component) {
            for (auto const target : explicit_outgoing[node]) {
                if (std::ranges::find(component, target) != component.end()) {
                    ++local_indegree[target];
                }
            }
        }
        std::vector<std::size_t> ready;
        for (auto const node : component) {
            if (local_indegree[node] == 0) ready.push_back(node);
        }
        auto by_bundle = [&](std::size_t a, std::size_t b) {
            return plan.nodes[a].bundle < plan.nodes[b].bundle;
        };
        std::ranges::sort(ready, by_bundle);
        while (!ready.empty()) {
            auto const node = ready.front();
            ready.erase(ready.begin());
            region.execution_order.push_back(plan.nodes[node].bundle);
            for (auto const target : explicit_outgoing[node]) {
                if (std::ranges::find(component, target) == component.end()) continue;
                if (--local_indegree[target] == 0) {
                    auto const insertion = std::lower_bound(
                        ready.begin(), ready.end(), target, by_bundle);
                    ready.insert(insertion, target);
                }
            }
        }
        if (region.execution_order.size() != component.size()) {
            return std::unexpected(
                "GraphJit SCC remains cyclic after detach dependencies are removed");
        }
        schedule.regions.push_back(std::move(region));
    }

    for (std::size_t node = 0; node < plan.nodes.size(); ++node) {
        schedule.bundle_to_region[plan.nodes[node].bundle] = node_to_region[node];
    }

    // Schedule condensation is a semantically distinct adjacency: background
    // dependencies must not pretend that tick_block produces their values.
    // Reuse the retained SCC partition, then order those SCCs using only
    // ordinary sequential tick dependencies.
    std::vector<std::vector<std::size_t>> region_outgoing(
        schedule.regions.size());
    std::vector<std::size_t> region_indegree(schedule.regions.size(), 0);
    for (std::size_t source = 0; source < explicit_outgoing.size(); ++source) {
        for (auto const target : explicit_outgoing[source]) {
            auto const source_region = node_to_region[source];
            auto const target_region = node_to_region[target];
            if (source_region == target_region) continue;
            region_outgoing[source_region].push_back(target_region);
        }
    }
    for (auto& targets : region_outgoing) {
        std::ranges::sort(targets);
        targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
        for (auto const target : targets) ++region_indegree[target];
    }
    auto region_key = [&](std::size_t region) {
        return schedule.regions[region].nodes.front();
    };
    std::vector<std::size_t> ready_regions;
    for (std::size_t region = 0; region < schedule.regions.size(); ++region) {
        if (region_indegree[region] == 0) ready_regions.push_back(region);
    }
    std::ranges::sort(ready_regions, {}, region_key);
    while (!ready_regions.empty()) {
        auto const region = ready_regions.front();
        ready_regions.erase(ready_regions.begin());
        schedule.region_order.push_back(region);
        for (auto const target : region_outgoing[region]) {
            if (--region_indegree[target] == 0) {
                auto const insertion = std::lower_bound(
                    ready_regions.begin(), ready_regions.end(), target,
                    [&](std::size_t lhs, std::size_t rhs) {
                        return region_key(lhs) < region_key(rhs);
                    });
                ready_regions.insert(insertion, target);
            }
        }
    }
    if (schedule.region_order.size() != schedule.regions.size()) {
        return std::unexpected(
            "GraphJit sequential SCC condensation graph is cyclic");
    }

    std::size_t execution_position = 0;
    for (auto const region_index : schedule.region_order) {
        for (auto const bundle : schedule.regions[region_index].execution_order)
            schedule.bundle_execution_position[bundle] = execution_position++;
    }
    return schedule;
}

std::expected<void, std::string> bind_detach_regions(
    ConnectionAnalysisPlan& plan)
{
    auto bind = [&](auto& connection, NodeBundleHandle target_bundle)
        -> std::expected<void, std::string> {
        if (!connection.detach) return {};
        if (target_bundle >= plan.schedule.bundle_to_region.size()
            || !plan.schedule.bundle_to_region[target_bundle]) {
            return std::unexpected(
                "GraphJit detach does not map to a concrete execution region");
        }
        auto const region_index = *plan.schedule.bundle_to_region[target_bundle];
        if (region_index >= plan.schedule.regions.size()
            || !plan.schedule.regions[region_index].cyclic) {
            return std::unexpected(
                "GraphJit validated detach did not form an execution SCC");
        }
        connection.detach_region = region_index;
        auto& region = plan.schedule.regions[region_index];
        region.maximum_block_size = std::min(
            region.maximum_block_size,
            floor_power_of_two(connection.detach->loop_extra_latency));
        return {};
    };

    for (auto& connection : plan.sample_connections) {
        if (auto bound = bind(
                connection, connection.target_port.node_bundle_handle);
            !bound) return std::unexpected(std::move(bound.error()));
    }
    for (auto& connection : plan.event_connections) {
        if (!connection.detach) continue;
        if (connection.targets.empty())
            return std::unexpected("GraphJit detached event connection has no target");
        if (auto bound = bind(connection, connection.targets.front().bundle);
            !bound) return std::unexpected(std::move(bound.error()));
    }
    for (auto& region : plan.schedule.regions) {
        region.scc_feedback_latency = region.cyclic
            ? region.maximum_block_size
            : 0;
    }
    return {};
}

std::size_t effective_block_size(
    ConnectionAnalysisPlan const& plan,
    NodeBundleHandle bundle,
    std::size_t kernel_block_size)
{
    if (bundle == plan.boundary_bundle) return kernel_block_size;
    if (bundle >= plan.schedule.bundle_to_region.size()
        || !plan.schedule.bundle_to_region[bundle]) {
        return kernel_block_size;
    }
    auto const region = *plan.schedule.bundle_to_region[bundle];
    if (region >= plan.schedule.regions.size()) return kernel_block_size;
    return std::min(
        kernel_block_size, plan.schedule.regions[region].maximum_block_size);
}

std::expected<void, std::string> plan_sample_latency_compensation(
    ConnectionAnalysisPlan& plan)
{
    // The feed-forward latency carried by a concrete node's output values,
    // before the authored latency of any particular output port. Once all
    // inputs of a node are aligned, every output shares the same upstream
    // path latency plus the node's intrinsic internal latency.
    std::vector<std::optional<std::size_t>> node_output_latency(
        plan.schedule.bundle_to_region.size());

    auto planned_node = [&](NodeBundleHandle bundle) -> PlannedGraphNode const* {
        auto const found = std::ranges::find_if(
            plan.nodes,
            [&](PlannedGraphNode const& node) { return node.bundle == bundle; });
        return found == plan.nodes.end() ? nullptr : &*found;
    };

    // Keep authored output latency as the default for connections not covered
    // by this feed-forward Tick pass (background access, boundaries, and
    // cyclic regions whose feedback latency belongs to point 12).
    for (auto& connection : plan.sample_connections) {
        connection.read_latency = connection.source_latency;
        for (auto& channel : connection.source_channel_timings) {
            channel.read_latency = channel.source_latency;
        }
    }

    for (auto const region_index : plan.schedule.region_order) {
        if (region_index >= plan.schedule.regions.size()) {
            return std::unexpected(
                "sample latency compensation references an invalid schedule region");
        }
        auto const& region = plan.schedule.regions[region_index];

        if (region.cyclic) {
            // Feedback-aware SCC latency is deliberately deferred. Mark a
            // conservative provisional base so analysis of downstream regions
            // remains total; GraphJit's current lowering capability gate still
            // rejects feedback sample storage before execution.
            for (auto const bundle : region.execution_order) {
                auto const* node = planned_node(bundle);
                if (!node || bundle >= node_output_latency.size()) continue;
                node_output_latency[bundle] = node->internal_latency_samples;
            }
            continue;
        }

        for (auto const bundle : region.execution_order) {
            auto const* node = planned_node(bundle);
            if (!node || bundle >= node_output_latency.size()) {
                return std::unexpected(
                    "sample latency compensation lost a scheduled concrete node");
            }

            struct IncomingPath {
                std::size_t connection_index = 0;
                std::size_t source_channel_index = 0;
                std::size_t arrival_latency = 0;
            };
            std::vector<IncomingPath> incoming;
            std::size_t aligned_input_latency = 0;

            for (std::size_t connection_index = 0;
                 connection_index < plan.sample_connections.size();
                 ++connection_index) {
                auto const& connection = plan.sample_connections[connection_index];
                if (!is_entirely_realtime_delivery(connection)
                    || connection.detach
                    || connection.target_port.node_bundle_handle != bundle) {
                    continue;
                }
                if (connection.source_channel_timings.size()
                    != connection.source_channels.size()) {
                    return std::unexpected(
                        "sample latency compensation lost source-channel timing metadata");
                }

                for (std::size_t source_channel_index = 0;
                     source_channel_index < connection.source_channel_timings.size();
                     ++source_channel_index) {
                    auto const& channel =
                        connection.source_channel_timings[source_channel_index];
                    std::size_t upstream_latency = 0;
                    if (channel.source.bundle != plan.boundary_bundle) {
                        if (channel.source.bundle >= node_output_latency.size()
                            || !node_output_latency[channel.source.bundle]) {
                            return std::unexpected(
                                "sample latency compensation encountered a source before its producer");
                        }
                        upstream_latency = *node_output_latency[channel.source.bundle];
                    }

                    auto arrival = checked_latency_add(
                        upstream_latency,
                        channel.source_latency,
                        "sample channel path latency");
                    if (!arrival) {
                        return std::unexpected(std::move(arrival.error()));
                    }
                    aligned_input_latency = std::max(
                        aligned_input_latency, *arrival);
                    incoming.push_back(IncomingPath{
                        .connection_index = connection_index,
                        .source_channel_index = source_channel_index,
                        .arrival_latency = *arrival,
                    });
                }
            }

            for (auto const& path : incoming) {
                auto& connection = plan.sample_connections[path.connection_index];
                auto& channel =
                    connection.source_channel_timings[path.source_channel_index];
                auto const compensation =
                    aligned_input_latency - path.arrival_latency;
                auto read_latency = checked_latency_add(
                    channel.source_latency,
                    compensation,
                    "sample compensated channel read latency");
                if (!read_latency) {
                    return std::unexpected(std::move(read_latency.error()));
                }
                channel.read_latency = *read_latency;
            }

            // Whole-port sources necessarily share one producer path and one
            // authored port latency, so every channel has the same effective
            // read latency. For composed sources retain a conservative scalar
            // maximum for the existing storage planner; explicit channel
            // composition will consume source_channel_timings directly.
            for (auto& connection : plan.sample_connections) {
                if (connection.target_port.node_bundle_handle != bundle
                    || connection.source_channel_timings.empty()) {
                    continue;
                }
                connection.read_latency = 0;
                for (auto const& channel : connection.source_channel_timings) {
                    connection.read_latency = std::max(
                        connection.read_latency, channel.read_latency);
                }
            }

            auto output_latency = checked_latency_add(
                aligned_input_latency,
                node->internal_latency_samples,
                "sample node output path latency");
            if (!output_latency) {
                return std::unexpected(std::move(output_latency.error()));
            }
            node_output_latency[bundle] = *output_latency;
        }
    }

    return {};
}

std::size_t scheduled_execution_count(ConnectionAnalysisPlan const& plan)
{
    std::size_t result = 0;
    for (auto const& position : plan.schedule.bundle_execution_position) {
        if (position) result = std::max(result, *position + 1);
    }
    return result;
}

ConnectionLiveIntervalPlan live_interval_for_sample_group(
    ConnectionAnalysisPlan const& plan,
    SampleProducerGroupPlan const& group)
{
    auto const execution_count = scheduled_execution_count(plan);
    ConnectionLiveIntervalPlan live{
        .begin = execution_count,
        .end = 0,
    };
    bool saw_port = false;

    if (group.source_port) {
        saw_port = true;
        auto const source_bundle = group.source_port->node_bundle_handle;
        if (source_bundle == plan.boundary_bundle) {
            live.begin = 0;
        } else if (source_bundle < plan.schedule.bundle_execution_position.size()
            && plan.schedule.bundle_execution_position[source_bundle]) {
            live.begin = *plan.schedule.bundle_execution_position[source_bundle];
        }
    }

    for (auto const connection_index : group.connection_indices) {
        auto const& connection = plan.sample_connections[connection_index];
        auto const has_group_realtime = std::ranges::any_of(
            connection.source_channel_timings,
            [&](SampleSourceChannelTimingPlan const& source) {
                if (!uses_realtime_storage(source.delivery)) return false;
                return !group.source_port
                    || (source.source.bundle
                            == group.source_port->node_bundle_handle
                        && source.source.port == group.source_port->port_index);
            });
        if (!has_group_realtime) continue;
        auto const target = connection.target_port.node_bundle_handle;
        saw_port = true;
        if (target == plan.boundary_bundle) {
            live.end = execution_count;
        } else if (target < plan.schedule.bundle_execution_position.size()
            && plan.schedule.bundle_execution_position[target]) {
            live.end = std::max(
                live.end,
                *plan.schedule.bundle_execution_position[target]);
        }
        live.crosses_kernel_invocations =
            live.crosses_kernel_invocations || connection.detach;
    }
    if (!saw_port) live.begin = 0;
    if (live.begin == execution_count && execution_count != 0) live.begin = 0;
    if (live.end < live.begin) live.end = live.begin;
    return live;
}

ConnectionLiveIntervalPlan live_interval_for_event_group(
    ConnectionAnalysisPlan const& plan,
    EventProducerGroupPlan const& group)
{
    auto const execution_count = scheduled_execution_count(plan);
    ConnectionLiveIntervalPlan live{
        .begin = execution_count,
        .end = 0,
    };
    bool saw_port = false;
    for (auto const connection_index : group.connection_indices) {
        auto const& connection = plan.event_connections[connection_index];
        for (auto const& delivery : connection.deliveries) {
            if (!uses_realtime_storage(delivery.mechanism)) continue;
            auto const source =
                connection.source_plans[delivery.source_index].source;
            auto const target =
                connection.target_plans[delivery.target_index].target;
            saw_port = true;
            if (source.bundle == plan.boundary_bundle) {
                live.begin = 0;
            } else if (source.bundle
                    < plan.schedule.bundle_execution_position.size()
                && plan.schedule.bundle_execution_position[source.bundle]) {
                live.begin = std::min(
                    live.begin,
                    *plan.schedule.bundle_execution_position[source.bundle]);
            }
            if (target.bundle == plan.boundary_bundle) {
                live.end = execution_count;
            } else if (target.bundle
                    < plan.schedule.bundle_execution_position.size()
                && plan.schedule.bundle_execution_position[target.bundle]) {
                live.end = std::max(
                    live.end,
                    *plan.schedule.bundle_execution_position[target.bundle]);
            }
        }
        live.crosses_kernel_invocations =
            live.crosses_kernel_invocations || connection.detach;
    }
    if (!saw_port) live.begin = 0;
    if (live.begin == execution_count && execution_count != 0) live.begin = 0;
    if (live.end < live.begin) live.end = live.begin;
    return live;
}

void join_capabilities(
    PortStorageRequirements& target,
    PortStorageRequirements const& source) noexcept
{
    target.current_tick = target.current_tick
        || source.current_tick;
    target.capture = target.capture
        || source.capture;
    target.persisted_pages = target.persisted_pages
        || source.persisted_pages;
    target.tick_sequential = target.tick_sequential
        || source.tick_sequential;
    target.tick_random_access = target.tick_random_access
        || source.tick_random_access;
    target.background_random_access =
        target.background_random_access
        || source.background_random_access;
}

void remove_subsumed_capabilities(
    PortStorageRequirements& capabilities) noexcept
{
    // One materialized immutable addressable window supplies ordinary sequential
    // slices for the same atom/range. Keep the incidence facts separately, but
    // do not ask storage planning for a redundant sequential-only data.
    if (capabilities.tick_random_access) {
        capabilities.tick_sequential = false;
    }
}

PortStorageRequirements storage_capabilities_for(
    PlannedSourceProduction production,
    OutputRetention retention,
    PlannedDeliveryMechanism delivery) noexcept
{
    PortStorageRequirements result;
    if (retention == OutputRetention::persisted) {
        result.persisted_pages = true;
        result.capture =
            production == PlannedSourceProduction::tick;
    }

    switch (delivery) {
    case PlannedDeliveryMechanism::tick_to_sequential:
        result.current_tick = true;
        break;
    case PlannedDeliveryMechanism::tock_to_sequential:
        if (retention == OutputRetention::ephemeral) {
            result.tick_sequential = true;
        }
        break;
    case PlannedDeliveryMechanism::tock_to_random_access:
    case PlannedDeliveryMechanism::replayed_tick_to_random_access:
        if (retention == OutputRetention::ephemeral) {
            // RandomAccessInputConfig does not yet say which legal callback
            // reads it. Until callback-use facts are reflected, join both the
            // Tick-time materialized and background-transaction ports.
            result.tick_random_access = true;
            result.background_random_access = true;
        }
        break;
    case PlannedDeliveryMechanism::persisted_tick_to_random_access:
        break;
    }
    return result;
}

PortStorageRequirements target_capabilities_for(
    PlannedSourceProduction production,
    OutputRetention retention,
    PlannedDeliveryMechanism delivery) noexcept
{
    auto result = storage_capabilities_for(production, retention, delivery);
    // Capture is an intrinsic source-side transfer obligation. A target may
    // consume the resulting published pages, but it never owns another copy of
    // the producer's capture staging.
    result.capture = false;
    return result;
}

template<class T>
void append_unique(std::vector<T>& values, T value)
{
    if (!std::ranges::contains(values, value)) values.push_back(std::move(value));
}

struct SampleSourceAtomUse {
    std::size_t connection_index = 0;
    std::size_t contribution_index = 0;
    // Position within the contribution's semantic conversion input. Channels
    // participating in the same contribution are not interchangeable merely
    // because their connection/contribution indices match.
    std::size_t projection_source_position = 0;
    PlannedDeliveryMechanism delivery =
        PlannedDeliveryMechanism::tick_to_sequential;
    PlannedDestinationAccess destination_access =
        PlannedDestinationAccess::sequential;
    std::size_t source_history = 0;
    std::size_t source_latency = 0;
    std::size_t read_latency = 0;
    std::size_t target_history = 0;
    bool detached = false;

    bool operator==(SampleSourceAtomUse const&) const = default;
};

struct SampleTargetAtomUse {
    std::size_t connection_index = 0;
    std::size_t contribution_index = 0;
    // Position within the contribution's semantic conversion result.
    std::size_t projection_target_position = 0;
    std::vector<std::size_t> source_channel_indices{};
    PlannedDestinationAccess destination_access =
        PlannedDestinationAccess::sequential;
    std::size_t target_history = 0;
    bool detached = false;

    bool operator==(SampleTargetAtomUse const&) const = default;
};

struct SampleSourceAtomCandidate {
    SampleOutputChannelId channel{};
    ChannelLayout layout{};
    PlannedSourceProduction production = PlannedSourceProduction::tick;
    OutputRetention retention = OutputRetention::ephemeral;
    std::vector<SampleSourceAtomUse> uses{};
    PortStorageRequirements capabilities{};
};

struct SampleTargetAtomCandidate {
    SampleInputChannelId channel{};
    ChannelLayout layout{};
    PlannedDestinationAccess access = PlannedDestinationAccess::sequential;
    std::vector<SampleTargetAtomUse> uses{};
    PortStorageRequirements capabilities{};
};

void plan_port_atoms(
    ConfiguredGraph const& graph,
    ConnectionAnalysisPlan& plan)
{
    auto& background = plan.background;
    background.sample_source_subsets.clear();
    background.sample_target_subsets.clear();
    background.event_source_subsets.clear();
    background.event_target_subsets.clear();

    constexpr auto whole_contribution =
        std::numeric_limits<std::size_t>::max();
    std::vector<SampleSourceAtomCandidate> sample_sources;
    std::vector<SampleTargetAtomCandidate> sample_targets;

    auto source_candidate = [&](SampleOutputChannelId channel,
                                ChannelLayout layout,
                                PlannedSourceProduction production,
                                OutputRetention retention)
        -> SampleSourceAtomCandidate& {
        auto const found = std::ranges::find_if(
            sample_sources,
            [&](SampleSourceAtomCandidate const& candidate) {
                return candidate.channel.bundle == channel.bundle
                    && candidate.channel.port == channel.port
                    && candidate.channel.channel == channel.channel;
            });
        if (found != sample_sources.end()) return *found;
        sample_sources.push_back(SampleSourceAtomCandidate{
            .channel = channel,
            .layout = layout,
            .production = production,
            .retention = retention,
        });
        return sample_sources.back();
    };
    auto target_candidate = [&](SampleInputChannelId channel,
                                ChannelLayout layout,
                                PlannedDestinationAccess access)
        -> SampleTargetAtomCandidate& {
        auto const found = std::ranges::find_if(
            sample_targets,
            [&](SampleTargetAtomCandidate const& candidate) {
                return candidate.channel.bundle == channel.bundle
                    && candidate.channel.port == channel.port
                    && candidate.channel.channel == channel.channel;
            });
        if (found != sample_targets.end()) return *found;
        sample_targets.push_back(SampleTargetAtomCandidate{
            .channel = channel,
            .layout = layout,
            .access = access,
        });
        return sample_targets.back();
    };

    // Begin with every authored port element, including disconnected
    // values. Connections refine incidence below; intrinsic persistence and
    // requestability remain storage roots even without a current consumer.
    for (auto const& node : plan.nodes) {
        for (std::size_t port = 0; port < node.sample_input_count; ++port) {
            NodeBundlePortId const id{node.bundle, PortKind::sample, port};
            auto const config = graph.node_bundles.resolve_sample_input(id).config;
            auto const count = channel_count(config.channel_layout);
            for (std::size_t channel = 0; channel < count; ++channel) {
                target_candidate(
                    SampleInputChannelId{
                        .bundle = node.bundle,
                        .port = port,
                        .channel = channel,
                    },
                    config.channel_layout,
                    destination_access(config.access));
            }
        }
        for (std::size_t port = 0; port < node.sample_output_count; ++port) {
            NodeBundlePortId const id{node.bundle, PortKind::sample, port};
            auto const config = graph.node_bundles.resolve_sample_output(id).config;
            auto const production = source_production(config.production);
            PortStorageRequirements intrinsic;
            if (config.retention == OutputRetention::persisted) {
                intrinsic.persisted_pages = true;
                intrinsic.capture =
                    production == PlannedSourceProduction::tick;
            } else if (production == PlannedSourceProduction::tock
                       || (production == PlannedSourceProduction::tick
                           && node.contextually_replayable)) {
                intrinsic.background_random_access = true;
            }
            auto const count = channel_count(config.channel_layout);
            for (std::size_t channel = 0; channel < count; ++channel) {
                auto& candidate = source_candidate(
                    SampleOutputChannelId{
                        .bundle = node.bundle,
                        .port = port,
                        .channel = channel,
                    },
                    config.channel_layout,
                    production,
                    config.retention);
                join_capabilities(candidate.capabilities, intrinsic);
            }
        }
    }

    for (std::size_t connection_index = 0;
         connection_index < plan.sample_connections.size();
         ++connection_index) {
        auto const& connection = plan.sample_connections[connection_index];
        for (std::size_t source_index = 0;
             source_index < connection.source_channel_timings.size();
             ++source_index) {
            auto const& timing = connection.source_channel_timings[source_index];
            auto& candidate = source_candidate(
                timing.source,
                timing.source_layout,
                timing.production,
                timing.retention);
            std::vector<std::size_t> contributions;
            for (std::size_t contribution = 0;
                 contribution < connection.projection_contributions.size();
                 ++contribution) {
                if (std::ranges::contains(
                        connection.projection_contributions[contribution]
                            .source_channel_indices,
                        source_index)) {
                    contributions.push_back(contribution);
                }
            }
            if (contributions.empty()) contributions.push_back(whole_contribution);
            for (auto const contribution : contributions) {
                auto source_position = source_index;
                if (contribution != whole_contribution) {
                    auto const& source_indices =
                        connection.projection_contributions[contribution]
                            .source_channel_indices;
                    source_position = static_cast<std::size_t>(std::distance(
                        source_indices.begin(),
                        std::ranges::find(source_indices, source_index)));
                }
                append_unique(candidate.uses, SampleSourceAtomUse{
                    .connection_index = connection_index,
                    .contribution_index = contribution,
                    .projection_source_position = source_position,
                    .delivery = timing.delivery,
                    .destination_access = timing.destination_access,
                    .source_history = timing.source_history,
                    .source_latency = timing.source_latency,
                    .read_latency = timing.read_latency,
                    .target_history = connection.target_history,
                    .detached = connection.detach.has_value(),
                });
            }
            join_capabilities(
                candidate.capabilities,
                storage_capabilities_for(
                    timing.production, timing.retention, timing.delivery));
        }

        for (std::size_t target_channel = 0;
             target_channel < connection.target_channels.size();
             ++target_channel) {
            auto& candidate = target_candidate(
                connection.target_channels[target_channel],
                connection.target_layout,
                connection.destination_access);
            bool saw_contribution = false;
            for (std::size_t contribution = 0;
                 contribution < connection.projection_contributions.size();
                 ++contribution) {
                auto const& projection =
                    connection.projection_contributions[contribution];
                if (!std::ranges::contains(
                        projection.target_channels, target_channel)) {
                    continue;
                }
                saw_contribution = true;
                auto const target_position =
                    static_cast<std::size_t>(std::distance(
                        projection.target_channels.begin(),
                        std::ranges::find(
                            projection.target_channels, target_channel)));
                append_unique(candidate.uses, SampleTargetAtomUse{
                    .connection_index = connection_index,
                    .contribution_index = contribution,
                    .projection_target_position = target_position,
                    .source_channel_indices =
                        projection.source_channel_indices,
                    .destination_access = connection.destination_access,
                    .target_history = connection.target_history,
                    .detached = connection.detach.has_value(),
                });
                for (auto const source_index :
                     projection.source_channel_indices) {
                    if (source_index >= connection.source_channel_timings.size()) {
                        continue;
                    }
                    auto const& timing =
                        connection.source_channel_timings[source_index];
                    join_capabilities(
                        candidate.capabilities,
                        target_capabilities_for(
                            timing.production,
                            timing.retention,
                            timing.delivery));
                }
            }
            if (!saw_contribution) {
                std::vector<std::size_t> source_indices(
                    connection.source_channel_timings.size());
                std::iota(source_indices.begin(), source_indices.end(), 0);
                append_unique(candidate.uses, SampleTargetAtomUse{
                    .connection_index = connection_index,
                    .contribution_index = whole_contribution,
                    .projection_target_position = target_channel,
                    .source_channel_indices = std::move(source_indices),
                    .destination_access = connection.destination_access,
                    .target_history = connection.target_history,
                    .detached = connection.detach.has_value(),
                });
                for (auto const& timing : connection.source_channel_timings) {
                    join_capabilities(
                        candidate.capabilities,
                        target_capabilities_for(
                            timing.production,
                            timing.retention,
                            timing.delivery));
                }
            }
        }
    }

    for (auto& candidate : sample_sources) {
        remove_subsumed_capabilities(candidate.capabilities);
    }
    for (auto& candidate : sample_targets) {
        remove_subsumed_capabilities(candidate.capabilities);
    }

    auto source_port = [](SampleOutputChannelId channel) {
        return NodeBundlePortId{
            channel.bundle, PortKind::sample, channel.port};
    };
    for (auto const& candidate : sample_sources) {
        auto atom = std::ranges::find_if(
            background.sample_source_subsets,
            [&](SampleSourcePortSubsetPlan const& existing) {
                if (existing.port != source_port(candidate.channel)
                    || existing.source_layout != candidate.layout
                    || existing.production != candidate.production
                    || existing.retention != candidate.retention
                    || existing.storage != candidate.capabilities
                    || existing.channels.empty()) {
                    return false;
                }
                auto const representative = std::ranges::find_if(
                    sample_sources,
                    [&](SampleSourceAtomCandidate const& value) {
                        auto const& first = existing.channels.front();
                        return value.channel.bundle == first.bundle
                            && value.channel.port == first.port
                            && value.channel.channel == first.channel;
                    });
                return representative != sample_sources.end()
                    && representative->uses == candidate.uses;
            });
        if (atom == background.sample_source_subsets.end()) {
            background.sample_source_subsets.push_back(
                SampleSourcePortSubsetPlan{
                    .port = source_port(candidate.channel),
                    .source_layout = candidate.layout,
                    .production = candidate.production,
                    .retention = candidate.retention,
                    .channels = {candidate.channel},
                    .storage = candidate.capabilities,
                });
            atom = std::prev(background.sample_source_subsets.end());
        } else {
            atom->channels.push_back(candidate.channel);
        }
        for (auto const& use : candidate.uses) {
            append_unique(atom->connection_indices, use.connection_index);
            append_unique(
                atom->configured_connection_indices,
                plan.sample_connections[use.connection_index]
                    .configured_connection_index);
        }
    }

    auto sample_source_atom_for = [&](SampleOutputChannelId channel)
        -> std::optional<PortSubsetIndex> {
        for (PortSubsetIndex atom = 0;
             atom < background.sample_source_subsets.size(); ++atom) {
            if (std::ranges::contains(
                    background.sample_source_subsets[atom].channels, channel)) {
                return atom;
            }
        }
        return std::nullopt;
    };
    auto target_port = [](SampleInputChannelId channel) {
        return NodeBundlePortId{
            channel.bundle, PortKind::sample, channel.port};
    };
    for (auto const& candidate : sample_targets) {
        std::vector<PortSubsetIndex> source_subsets;
        for (auto const& use : candidate.uses) {
            auto const& connection =
                plan.sample_connections[use.connection_index];
            for (auto const source_index : use.source_channel_indices) {
                if (source_index >= connection.source_channel_timings.size()) {
                    continue;
                }
                if (auto const atom = sample_source_atom_for(
                        connection.source_channel_timings[source_index].source)) {
                    append_unique(source_subsets, *atom);
                }
            }
        }
        auto atom = std::ranges::find_if(
            background.sample_target_subsets,
            [&](SampleTargetPortSubsetPlan const& existing) {
                if (existing.port != target_port(candidate.channel)
                    || existing.target_layout != candidate.layout
                    || existing.access != candidate.access
                    || existing.storage != candidate.capabilities
                    || existing.source_subsets != source_subsets
                    || existing.channels.empty()) {
                    return false;
                }
                auto const representative = std::ranges::find_if(
                    sample_targets,
                    [&](SampleTargetAtomCandidate const& value) {
                        auto const& first = existing.channels.front();
                        return value.channel.bundle == first.bundle
                            && value.channel.port == first.port
                            && value.channel.channel == first.channel;
                    });
                return representative != sample_targets.end()
                    && representative->uses == candidate.uses;
            });
        if (atom == background.sample_target_subsets.end()) {
            background.sample_target_subsets.push_back(
                SampleTargetPortSubsetPlan{
                    .port = target_port(candidate.channel),
                    .target_layout = candidate.layout,
                    .access = candidate.access,
                    .channels = {candidate.channel},
                    .source_subsets = std::move(source_subsets),
                    .storage = candidate.capabilities,
                });
            atom = std::prev(background.sample_target_subsets.end());
        } else {
            atom->channels.push_back(candidate.channel);
        }
        for (auto const& use : candidate.uses) {
            append_unique(atom->connection_indices, use.connection_index);
            append_unique(
                atom->configured_connection_indices,
                plan.sample_connections[use.connection_index]
                    .configured_connection_index);
        }
    }

    for (auto const& node : plan.nodes) {
        for (std::size_t port = 0; port < node.event_input_count; ++port) {
            NodeBundlePortId const id{node.bundle, PortKind::event, port};
            auto const config = graph.node_bundles.resolve_event_input(id).config;
            background.event_target_subsets.push_back(EventTargetPortSubsetPlan{
                .port = EventInputPortId{node.bundle, port},
                .type = config.type,
                .access = destination_access(config.access),
            });
        }
        for (std::size_t port = 0; port < node.event_output_count; ++port) {
            NodeBundlePortId const id{node.bundle, PortKind::event, port};
            auto const config = graph.node_bundles.resolve_event_output(id).config;
            auto const production = source_production(config.production);
            PortStorageRequirements intrinsic;
            if (config.retention == OutputRetention::persisted) {
                intrinsic.persisted_pages = true;
                intrinsic.capture =
                    production == PlannedSourceProduction::tick;
            } else if (production == PlannedSourceProduction::tock
                       || (production == PlannedSourceProduction::tick
                           && node.contextually_replayable)) {
                intrinsic.background_random_access = true;
            }
            background.event_source_subsets.push_back(EventSourcePortSubsetPlan{
                .port = EventOutputPortId{node.bundle, port},
                .type = config.type,
                .production = production,
                .retention = config.retention,
                .max_events_per_index = config.max_events_per_index,
                .storage = intrinsic,
            });
        }
    }

    for (std::size_t connection_index = 0;
         connection_index < plan.event_connections.size(); ++connection_index) {
        auto const& connection = plan.event_connections[connection_index];
        for (std::size_t source_index = 0;
             source_index < connection.source_plans.size(); ++source_index) {
            auto const& source = connection.source_plans[source_index];
            auto found = std::ranges::find_if(
                background.event_source_subsets,
                [&](EventSourcePortSubsetPlan const& atom) {
                    return atom.port == source.source;
                });
            if (found == background.event_source_subsets.end()) {
                background.event_source_subsets.push_back(
                    EventSourcePortSubsetPlan{
                        .port = source.source,
                        .type = connection.source_type,
                        .production = source.production,
                        .retention = source.retention,
                        .max_events_per_index = source.max_events_per_index,
                    });
                found = std::prev(background.event_source_subsets.end());
            }
            append_unique(found->connection_indices, connection_index);
            append_unique(
                found->configured_connection_indices,
                connection.configured_connection_index);
            for (auto const& delivery : connection.deliveries) {
                if (delivery.source_index != source_index) continue;
                join_capabilities(
                    found->storage,
                    storage_capabilities_for(
                        source.production,
                        source.retention,
                        delivery.mechanism));
            }
        }

        for (std::size_t target_index = 0;
             target_index < connection.target_plans.size(); ++target_index) {
            auto const& target = connection.target_plans[target_index];
            auto found = std::ranges::find_if(
                background.event_target_subsets,
                [&](EventTargetPortSubsetPlan const& atom) {
                    return atom.port == target.target;
                });
            if (found == background.event_target_subsets.end()) {
                background.event_target_subsets.push_back(
                    EventTargetPortSubsetPlan{
                        .port = target.target,
                        .type = connection.target_type,
                        .access = target.access,
                    });
                found = std::prev(background.event_target_subsets.end());
            }
            append_unique(found->connection_indices, connection_index);
            append_unique(
                found->configured_connection_indices,
                connection.configured_connection_index);
            for (auto const& delivery : connection.deliveries) {
                if (delivery.target_index != target_index
                    || delivery.source_index >= connection.source_plans.size()) {
                    continue;
                }
                auto const& source =
                    connection.source_plans[delivery.source_index];
                auto const source_subset = std::ranges::find_if(
                    background.event_source_subsets,
                    [&](EventSourcePortSubsetPlan const& atom) {
                        return atom.port == source.source;
                    });
                if (source_subset != background.event_source_subsets.end()) {
                    append_unique(
                        found->source_subsets,
                        static_cast<PortSubsetIndex>(std::distance(
                            background.event_source_subsets.begin(), source_subset)));
                }
                join_capabilities(
                    found->storage,
                    target_capabilities_for(
                        source.production,
                        source.retention,
                        delivery.mechanism));
            }
        }
    }

    for (auto& connection : background.connections) {
        connection.source_subsets.clear();
        connection.target_subsets.clear();
        if (connection.kind == PortKind::sample) {
            for (PortSubsetIndex atom = 0;
                 atom < background.sample_source_subsets.size(); ++atom) {
                auto const& candidate = background.sample_source_subsets[atom];
                if (!std::ranges::contains(
                        candidate.configured_connection_indices,
                        connection.configured_connection_index)) {
                    continue;
                }
                auto const contributes = std::ranges::any_of(
                    candidate.channels,
                    [&](SampleOutputChannelId const& channel) {
                        return std::ranges::contains(
                            connection.sample_source_channels, channel);
                    });
                if (contributes) append_unique(connection.source_subsets, atom);
            }
            for (PortSubsetIndex atom = 0;
                 atom < background.sample_target_subsets.size(); ++atom) {
                auto const& candidate = background.sample_target_subsets[atom];
                if (!std::ranges::contains(
                        candidate.configured_connection_indices,
                        connection.configured_connection_index)) {
                    continue;
                }
                auto const contributes = std::ranges::any_of(
                    candidate.channels,
                    [&](SampleInputChannelId const& channel) {
                        return std::ranges::contains(
                            connection.sample_target_channels, channel);
                    });
                if (contributes) append_unique(connection.target_subsets, atom);
            }
            continue;
        }

        for (PortSubsetIndex atom = 0;
             atom < background.event_source_subsets.size(); ++atom) {
            auto const& candidate = background.event_source_subsets[atom];
            if (!std::ranges::contains(
                    candidate.configured_connection_indices,
                    connection.configured_connection_index)) {
                continue;
            }
            auto const contributes = std::ranges::any_of(
                connection.event_deliveries,
                [&](graph_jit::EventDeliveryPlan const& delivery) {
                    return delivery.source == candidate.port;
                });
            if (contributes) append_unique(connection.source_subsets, atom);
        }
        for (PortSubsetIndex atom = 0;
             atom < background.event_target_subsets.size(); ++atom) {
            auto const& candidate = background.event_target_subsets[atom];
            if (!std::ranges::contains(
                    candidate.configured_connection_indices,
                    connection.configured_connection_index)) {
                continue;
            }
            auto const contributes = std::ranges::any_of(
                connection.event_deliveries,
                [&](graph_jit::EventDeliveryPlan const& delivery) {
                    return delivery.target == candidate.port;
                });
            if (contributes) append_unique(connection.target_subsets, atom);
        }
    }
    for (auto& atom : background.event_source_subsets) {
        remove_subsumed_capabilities(atom.storage);
    }
    for (auto& atom : background.event_target_subsets) {
        remove_subsumed_capabilities(atom.storage);
    }
}

void plan_sample_groups(
    ConnectionAnalysisPlan& plan,
    std::size_t kernel_block_size,
    RealtimeStorageCostModel const& cost_model)
{
    auto source_port_for = [](SampleOutputChannelId channel) {
        return NodeBundlePortId{
            channel.bundle, PortKind::sample, channel.port};
    };

    for (std::size_t i = 0; i < plan.sample_connections.size(); ++i) {
        auto& connection = plan.sample_connections[i];
        if (connection.source_channel_timings.size()
            != connection.source_channels.size()) {
            continue;
        }

        if (has_realtime_delivery(connection)) {
            auto const target_block = effective_block_size(
                plan,
                connection.target_port.node_bundle_handle,
                kernel_block_size);
            for (auto const& channel : connection.source_channel_timings) {
                if (!uses_realtime_storage(channel.delivery)) continue;
                if (effective_block_size(
                        plan, channel.source.bundle, kernel_block_size)
                    != target_block) {
                    connection.requires_block_materialization = true;
                    break;
                }
            }
        }

    }

    // Port atoms are the correctness partition. The current node-facing
    // Tick storage may still coalesce the atoms of one authored output
    // port because OutputPort writes that port as one operation; all incidence
    // and capability distinctions remain retained in BackgroundEvaluationPlan for derived
    // materialization/page ports.
    std::vector<NodeBundlePortId> connected_source_ports;
    for (auto const& atom : plan.background.sample_source_subsets) {
        if (!atom.connection_indices.empty()) {
            append_unique(connected_source_ports, atom.port);
        }
    }
    for (PortSubsetIndex atom_index = 0;
         atom_index < plan.background.sample_source_subsets.size(); ++atom_index) {
        auto const& atom = plan.background.sample_source_subsets[atom_index];
        if (!std::ranges::contains(connected_source_ports, atom.port)) continue;
        auto group = std::ranges::find_if(
            plan.sample_producer_groups,
            [&](SampleProducerGroupPlan const& candidate) {
                return candidate.source_port
                    && *candidate.source_port == atom.port;
            });
        if (group == plan.sample_producer_groups.end()) {
            plan.sample_producer_groups.push_back(SampleProducerGroupPlan{
                .source_port = atom.port,
                .source_type = atom.source_layout.channel_type,
                .canonical_source_layout = atom.source_layout,
            });
            group = std::prev(plan.sample_producer_groups.end());
        } else if (!group->canonical_source_layout
            || *group->canonical_source_layout != atom.source_layout) {
            continue;
        }
        append_unique(group->source_atom_indices, atom_index);
        for (auto const channel : atom.channels) {
            append_unique(group->source_channels, channel);
        }
        for (auto const connection_index : atom.connection_indices) {
            append_unique(group->connection_indices, connection_index);
        }
    }
    for (auto& group : plan.sample_producer_groups) {
        std::ranges::sort(
            group.source_channels,
            {},
            [](SampleOutputChannelId const& channel) {
                return std::tuple{
                    channel.bundle, channel.port, channel.channel};
            });
        std::ranges::sort(group.connection_indices);
    }

    for (std::size_t group_index = 0;
         group_index < plan.sample_producer_groups.size(); ++group_index) {
        auto& group = plan.sample_producer_groups[group_index];
        bool external = false;
        std::size_t retained = 0;
        for (auto const connection_index : group.connection_indices) {
            auto const& connection = plan.sample_connections[connection_index];
            std::size_t connection_retained = 0;
            bool saw_group_realtime_channel = false;
            bool saw_group_background_channel = false;
            for (auto const& channel : connection.source_channel_timings) {
                if (!group.source_port
                    || source_port_for(channel.source) != *group.source_port) {
                    continue;
                }
                if (!uses_realtime_storage(channel.delivery)) {
                    saw_group_background_channel = true;
                    continue;
                }
                saw_group_realtime_channel = true;
                // A detached consumer owns its delay/history retention in
                // the branch-local retained feedback timeline. The canonical producer still
                // retains its own declared output history, which its callback
                // may address independently of any consumer.
                connection_retained = std::max(
                    connection_retained,
                    connection.detach
                        ? saturating_add(
                            channel.source_history, channel.source_latency)
                        : retained_extent(
                            channel.source_history,
                            channel.read_latency,
                            connection.target_history));
            }
            group.has_background_connections = group.has_background_connections
                || saw_group_background_channel;
            if (!saw_group_realtime_channel) continue;
            group.has_realtime_connections = true;
            // Only Tick -> Sequential contributions enter realtime storage
            // planning. A mixed composition may also have background-materialized
            // source channels, but those are not represented by this buffer.
            external = external || connection.external_boundary;
            retained = std::max(retained, connection_retained);
        }
        group.storage_requirements = SampleConnectionStorageRequirements{
            .current_block_frames = kernel_block_size,
            .retained_frames = retained,
            .channel_count = group.canonical_source_layout
                ? channel_count(*group.canonical_source_layout)
                : channel_count(group.source_type),
            .value_size_bytes = sizeof(Sample),
        };
        if (!group.has_realtime_connections) continue;
        if (!external) {
            group.storage_plan = choose_sample_connection_storage_plan(
                group.storage_requirements, cost_model);
        }

        auto live = live_interval_for_sample_group(plan, group);
        live.crosses_kernel_invocations = live.crosses_kernel_invocations
            || group.storage_requirements.retained_frames != 0;
        group.live_interval = live;
        auto append_storage = [&](ConnectionStorageLifetime lifetime,
                                  std::size_t current_block_frames,
                                  std::size_t retained_extent_value) {
            plan.storage.regions.push_back(ConnectionStorageRegionRequirement{
                .data = PlannedConnectionData::sample,
                .producer_group_index = group_index,
                .lifetime = lifetime,
                .live_interval = live,
                .current_block_frames = current_block_frames,
                .retained_extent = retained_extent_value,
                .channel_count = group.storage_requirements.channel_count,
                .value_size_bytes = sizeof(Sample),
            });
        };
        if (external) {
            append_storage(
                ConnectionStorageLifetime::external,
                kernel_block_size,
                group.storage_requirements.retained_frames);
            continue;
        }
        switch (group.storage_plan->kind) {
        case RealtimeBufferStorageKind::transient_stack:
            append_storage(
                ConnectionStorageLifetime::transient, kernel_block_size, 0);
            break;
        case RealtimeBufferStorageKind::stack_with_persistent_carry:
            append_storage(
                ConnectionStorageLifetime::persistent,
                0,
                group.storage_requirements.retained_frames);
            append_storage(
                ConnectionStorageLifetime::transient, kernel_block_size, 0);
            break;
        case RealtimeBufferStorageKind::full_node_storage:
            append_storage(
                ConnectionStorageLifetime::persistent,
                kernel_block_size,
                group.storage_requirements.retained_frames);
            break;
        }
    }
}

std::expected<void, std::string> plan_event_groups(
    ConnectionAnalysisPlan& plan,
    std::size_t kernel_block_size,
    RealtimeStorageCostModel const& cost_model)
{
    for (std::size_t i = 0; i < plan.event_connections.size(); ++i) {
        auto& connection = plan.event_connections[i];
        if (has_realtime_delivery(connection)) {
            std::optional<std::size_t> block_size;
            std::optional<std::size_t> common_region;
            bool same_cyclic_region = true;
            auto observe = [&](NodeBundleHandle bundle) {
                auto const current =
                    effective_block_size(plan, bundle, kernel_block_size);
                if (block_size && *block_size != current) {
                    connection.requires_block_materialization = true;
                } else {
                    block_size = current;
                }
                if (bundle >= plan.schedule.bundle_to_region.size()
                    || !plan.schedule.bundle_to_region[bundle]) {
                    same_cyclic_region = false;
                    return;
                }
                auto const region = *plan.schedule.bundle_to_region[bundle];
                if (region >= plan.schedule.regions.size()
                    || !plan.schedule.regions[region].cyclic) {
                    same_cyclic_region = false;
                    return;
                }
                if (common_region && *common_region != region) {
                    same_cyclic_region = false;
                } else {
                    common_region = region;
                }
            };
            for (auto const& delivery : connection.deliveries) {
                if (!uses_realtime_storage(delivery.mechanism)) continue;
                observe(connection.source_plans[delivery.source_index].source.bundle);
                observe(connection.target_plans[delivery.target_index].target.bundle);
            }
            // Slice-major SCC execution keeps exact-type event transport direct
            // inside one region. Outside an SCC, invocation-oriented sequences
            // still need root-block materialization whenever either realtime
            // port is sliced.
            if (!same_cyclic_region
                && block_size && *block_size != kernel_block_size) {
                connection.requires_block_materialization = true;
            }
        }

        auto group = std::ranges::find_if(
            plan.event_producer_groups,
            [&](EventProducerGroupPlan const& candidate) {
                return candidate.source_type == connection.source_type
                    && candidate.sources == connection.sources;
            });
        if (group == plan.event_producer_groups.end()) {
            plan.event_producer_groups.push_back(EventProducerGroupPlan{
                .source_type = connection.source_type,
                .sources = connection.sources,
            });
            group = std::prev(plan.event_producer_groups.end());
        }
        group->connection_indices.push_back(i);

        for (auto const source : connection.sources) {
            auto const atom = std::ranges::find_if(
                plan.background.event_source_subsets,
                [&](EventSourcePortSubsetPlan const& candidate) {
                    return candidate.port == source;
                });
            if (atom != plan.background.event_source_subsets.end()) {
                append_unique(
                    group->source_atom_indices,
                    static_cast<PortSubsetIndex>(std::distance(
                        plan.background.event_source_subsets.begin(), atom)));
            }
        }
        for (auto const target : connection.targets) {
            auto const atom = std::ranges::find_if(
                plan.background.event_target_subsets,
                [&](EventTargetPortSubsetPlan const& candidate) {
                    return candidate.port == target;
                });
            if (atom != plan.background.event_target_subsets.end()) {
                append_unique(
                    group->target_atom_indices,
                    static_cast<PortSubsetIndex>(std::distance(
                        plan.background.event_target_subsets.begin(), atom)));
            }
        }

        for (std::size_t source_index = 0;
             source_index < connection.source_plans.size(); ++source_index) {
            auto const source = connection.source_plans[source_index].source;
            auto const source_realtime = std::ranges::any_of(
                connection.deliveries,
                [&](EventDeliveryPlan const& delivery) {
                    return delivery.source_index == source_index
                        && uses_realtime_storage(delivery.mechanism);
                });
            auto const source_background = std::ranges::any_of(
                connection.deliveries,
                [&](EventDeliveryPlan const& delivery) {
                    return delivery.source_index == source_index
                        && !uses_realtime_storage(delivery.mechanism);
                });
            if (source_realtime
                && !std::ranges::contains(group->realtime_sources, source)) {
                group->realtime_sources.push_back(source);
                auto const rate =
                    connection.source_plans[source_index].max_events_per_index;
                if (!is_valid_event_buffer_rate(rate)
                    || !is_valid_event_buffer_rate(
                        group->max_events_per_index + rate)) {
                    return std::unexpected(
                        "GraphJit realtime event producer aggregate rate is not representable");
                }
                group->max_events_per_index += rate;
            }
            if (source_background
                && !std::ranges::contains(group->background_sources, source)) {
                group->background_sources.push_back(source);
            }
            group->has_realtime_connections = group->has_realtime_connections
                || source_realtime;
            group->has_background_connections = group->has_background_connections
                || source_background;
        }
    }

    for (std::size_t group_index = 0;
         group_index < plan.event_producer_groups.size(); ++group_index) {
        auto& group = plan.event_producer_groups[group_index];
        bool external = false;
        bool requires_invocation_aggregate = group.realtime_sources.size() > 1;
        std::size_t retained = 0;
        for (auto const source : group.realtime_sources) {
            if (source.bundle >= plan.schedule.bundle_to_region.size()
                || !plan.schedule.bundle_to_region[source.bundle]) {
                continue;
            }
            auto const region = *plan.schedule.bundle_to_region[source.bundle];
            requires_invocation_aggregate = requires_invocation_aggregate
                || (region < plan.schedule.regions.size()
                    && plan.schedule.regions[region].cyclic);
        }
        for (auto const connection_index : group.connection_indices) {
            auto const& connection = plan.event_connections[connection_index];
            if (!has_realtime_delivery(connection)) continue;
            // Feedback storage is branch-local: the canonical producer group
            // remains an ordinary aggregate sequence and detached branches
            // acquire their own persistent rings during storage lowering.
            external = external || connection.external_boundary;
            requires_invocation_aggregate = requires_invocation_aggregate
                || connection.requires_conversion
                || connection.requires_block_materialization
                || connection.detach.has_value();
            for (auto const& delivery : connection.deliveries) {
                if (!uses_realtime_storage(delivery.mechanism)) continue;
                auto const& source =
                    connection.source_plans[delivery.source_index];
                auto const& target =
                    connection.target_plans[delivery.target_index];
                retained = std::max(
                    retained,
                    retained_extent(
                        source.history, source.latency, target.history));
            }
        }
        auto const current_event_capacity = event_count_for_sample_span(
            group.max_events_per_index, kernel_block_size);
        auto const retained_event_capacity = event_count_for_sample_span(
            group.max_events_per_index, retained);
        if (!current_event_capacity || !retained_event_capacity) {
            return std::unexpected(
                "GraphJit event producer sizing rate/sample span exceeds representable static capacity");
        }
        auto base_requirements = EventConnectionStorageRequirements{
            .current_window_samples = kernel_block_size,
            .retained_window_samples = retained,
            .current_event_capacity = *current_event_capacity,
            .retained_event_capacity = *retained_event_capacity,
            .value_size_bytes = sizeof(TimedEvent),
        };
        group.storage_requirements = base_requirements;
        group.requires_invocation_aggregate = requires_invocation_aggregate;
        if (!group.has_realtime_connections) continue;
        if (!external) {
            // Connection analysis owns only the event-rate and retained-window
            // requirements. Fan-in implementation alternatives depend on the
            // concrete per-slice producer buffers and merge operation selected
            // during lowering, so producer-home selection is deliberately
            // deferred to plan_event_ports().
            group.storage_plan = choose_event_connection_storage_plan(
                group.storage_requirements, cost_model);
        }

        auto live = live_interval_for_event_group(plan, group);
        live.crosses_kernel_invocations = live.crosses_kernel_invocations
            || retained != 0;
        group.live_interval = live;
        auto append_storage = [&](ConnectionStorageLifetime lifetime,
                                  std::size_t current_block_frames,
                                  std::size_t retained_extent_value) {
            plan.storage.regions.push_back(ConnectionStorageRegionRequirement{
                .data = PlannedConnectionData::event,
                .producer_group_index = group_index,
                .lifetime = lifetime,
                .live_interval = live,
                .current_block_frames = current_block_frames,
                .retained_extent = retained_extent_value,
                .channel_count = 1,
                .value_size_bytes = 0,
            });
        };
        if (external) {
            append_storage(
                ConnectionStorageLifetime::external,
                kernel_block_size,
                retained);
            continue;
        }
        switch (group.storage_plan->kind) {
        case RealtimeBufferStorageKind::transient_stack:
            append_storage(
                ConnectionStorageLifetime::transient, kernel_block_size, 0);
            break;
        case RealtimeBufferStorageKind::stack_with_persistent_carry:
            append_storage(
                ConnectionStorageLifetime::persistent,
                0,
                retained);
            append_storage(
                ConnectionStorageLifetime::transient, kernel_block_size, 0);
            break;
        case RealtimeBufferStorageKind::full_node_storage:
            append_storage(
                ConnectionStorageLifetime::persistent,
                kernel_block_size,
                retained);
            break;
        }
    }
    return {};
}

} // namespace

std::expected<ConnectionAnalysisPlan, std::string> build_connection_analysis_plan(
    ConfiguredGraph const& graph,
    std::size_t kernel_block_size,
    RealtimeStorageCostModel const& cost_model,
    BackgroundEvaluationPlan const* retained_background_plan)
{
    if (kernel_block_size == 0) {
        return std::unexpected(
            "connection analysis requires a non-zero kernel block size");
    }

    ConnectionAnalysisPlan plan;
    if (auto inventory = inventory_nodes(graph, plan); !inventory) {
        return std::unexpected(std::move(inventory.error()));
    }
    if (auto samples = inventory_sample_connections(graph, plan); !samples) {
        return std::unexpected(std::move(samples.error()));
    }
    if (auto projections = normalize_sample_target_projections(graph, plan); !projections) {
        return std::unexpected(std::move(projections.error()));
    }
    if (auto events = inventory_event_connections(graph, plan); !events) {
        return std::unexpected(std::move(events.error()));
    }
    if (auto replay = derive_contextual_replayability(graph, plan); !replay) {
        return std::unexpected(std::move(replay.error()));
    }
    if (retained_background_plan) {
        if (retained_background_plan->semantic_nodes.size()
                != plan.nodes.size()
            || retained_background_plan->bundle_to_semantic_node.size()
                != graph.node_bundles.size()) {
            return std::unexpected(
                "GraphJit retained background plan does not match this graph specialization");
        }
        for (std::size_t node = 0; node < plan.nodes.size(); ++node) {
            if (retained_background_plan->semantic_nodes[node].bundle
                != plan.nodes[node].bundle) {
                return std::unexpected(
                    "GraphJit retained background plan does not match configured node identity");
            }
        }
        plan.background = *retained_background_plan;
    } else {
        auto background = build_semantic_background_plan(graph, plan);
        if (!background) return std::unexpected(std::move(background.error()));
        plan.background = std::move(*background);
    }
    // Semantic SCC validation intentionally precedes the narrower storage
    // restriction on detach transport so background feedback is diagnosed in
    // terms of the whole semantic graph.
    if (auto detaches = validate_detach_delivery_mechanisms(plan); !detaches) {
        return std::unexpected(std::move(detaches.error()));
    }
    if (!retained_background_plan) {
        if (auto populated = populate_background_topology(graph, plan, plan.background);
            !populated) {
            return std::unexpected(std::move(populated.error()));
        }
    }
    if (auto acyclic = validate_explicit_graph_is_acyclic(graph, plan); !acyclic) {
        return std::unexpected(std::move(acyclic.error()));
    }
    if (auto detaches = validate_detached_connections(graph, plan); !detaches) {
        return std::unexpected(std::move(detaches.error()));
    }
    auto schedule = build_schedule(graph, plan);
    if (!schedule) return std::unexpected(std::move(schedule.error()));
    plan.schedule = std::move(*schedule);
    if (auto detach_regions = bind_detach_regions(plan); !detach_regions) {
        return std::unexpected(std::move(detach_regions.error()));
    }
    if (auto latency = plan_sample_latency_compensation(plan); !latency) {
        return std::unexpected(std::move(latency.error()));
    }
    plan_port_atoms(graph, plan);
    plan_sample_groups(plan, kernel_block_size, cost_model);
    if (auto events = plan_event_groups(
            plan, kernel_block_size, cost_model); !events) {
        return std::unexpected(std::move(events.error()));
    }
    auto background_storage = build_background_storage_plan(plan);
    if (!background_storage) {
        return std::unexpected(std::move(background_storage.error()));
    }
    plan.background.storage = std::move(*background_storage);
    return plan;
}

} // namespace iv::graph_jit::detail
