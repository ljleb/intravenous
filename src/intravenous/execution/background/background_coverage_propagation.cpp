#include <intravenous/execution/background/background_coverage_propagation.h>
#include <atomic>
#include <intravenous/execution/background/background_planning_scheduler.h>
#include <intravenous/execution/background/background_region_routing.h>
#include <intravenous/graph_jit/sequential_coverage.h>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <queue>
#include <span>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace iv {
namespace {

[[nodiscard]] IndexCoverage shifted_forward(
    IndexCoverage const& coverage, std::size_t latency)
{
    if (latency == 0) return coverage;
    IndexCoverage result;
    for (auto const range : coverage) {
        result.include({
            .begin = range.begin.advanced_by(latency),
            .extent = range.extent,
        });
    }
    return result;
}

[[nodiscard]] IndexCoverage shifted_back(
    IndexCoverage const& coverage, std::size_t latency)
{
    if (latency == 0) return coverage;
    IndexCoverage result;
    for (auto const range : coverage) {
        result.include({
            .begin = range.begin.retreated_by(latency),
            .extent = range.extent,
        });
    }
    return result;
}

IndexCoverage coverage_difference(IndexCoverage const& left, IndexCoverage const& right)
{
    return (left - right) | (right - left);
}

IndexCoverage region_coverage(std::span<GlobalIndexRange const> regions)
{
    IndexCoverage result;
    for (auto const region : regions) result.include(region);
    return result;
}

std::vector<GlobalIndexRange> coverage_regions(IndexCoverage const& coverage)
{
    return {coverage.begin(), coverage.end()};
}

[[nodiscard]] bool canonical_region_less(
    GlobalIndexRange left, GlobalIndexRange right) noexcept
{
    if (left.begin == right.begin) return left.extent > right.extent;
    return SignedGlobalIndexOrder{}(left.begin, right.begin);
}

[[nodiscard]] std::vector<BackgroundInputRegion> merge_sorted_region_runs(
    std::span<std::vector<BackgroundInputRegion>> runs)
{
    struct Cursor {
        std::size_t run = 0;
        std::size_t offset = 0;
    };
    auto later = [&](Cursor left, Cursor right) {
        auto const left_region = runs[left.run][left.offset].range;
        auto const right_region = runs[right.run][right.offset].range;
        if (left_region != right_region) {
            return canonical_region_less(right_region, left_region);
        }
        return left.run > right.run;
    };
    std::priority_queue<Cursor, std::vector<Cursor>, decltype(later)> pending{
        later};
    std::size_t count = 0;
    for (std::size_t run = 0; run < runs.size(); ++run) {
        count += runs[run].size();
        if (!runs[run].empty()) pending.push({.run = run});
    }
    std::vector<BackgroundInputRegion> result;
    result.reserve(count);
    while (!pending.empty()) {
        auto cursor = pending.top();
        pending.pop();
        result.push_back(std::move(runs[cursor.run][cursor.offset]));
        ++cursor.offset;
        if (cursor.offset < runs[cursor.run].size()) pending.push(cursor);
    }
    return result;
}

std::vector<GlobalIndexRange> input_region_ranges(std::span<BackgroundInputRegion const> regions)
{
    std::vector<GlobalIndexRange> result;
    result.reserve(regions.size());
    for (auto const& region : regions) result.push_back(region.range);
    return result;
}

bool shares_configuration_identity(
    graph_jit::NodeConfigurationIdentity const& left,
    graph_jit::NodeConfigurationIdentity const& right) noexcept
{
    if (auto const* sample = std::get_if<graph_jit::CompilerSampleConfiguration>(&left)) {
        auto const* other = std::get_if<graph_jit::CompilerSampleConfiguration>(&right);
        return other && *sample == *other;
    }
    auto const& owner = std::get<std::shared_ptr<void const>>(left);
    auto const* other = std::get_if<std::shared_ptr<void const>>(&right);
    return owner && other && *other && owner.get() == other->get()
        && !owner.owner_before(*other) && !other->owner_before(owner);
}

// Authored outputs and compiler-owned public-input resolution outputs occupy
// separate identity domains. Never manufacture a concrete node/port index for
// the latter just to make it fit StableOutputPortId.
using BackgroundOutputIdentity = std::variant<graph_jit::StableOutputPortId, graph_jit::StableInputPortId>;
std::optional<BackgroundOutputIdentity> background_output_identity(graph_jit::BackgroundPortPlan const& port)
{
    if (port.resolved_module_input) return BackgroundOutputIdentity{*port.resolved_module_input};
    if (port.stable_identity) return BackgroundOutputIdentity{*port.stable_identity};
    return std::nullopt;
}
using BackgroundNodeIdentity = std::variant<graph_jit::StableConcreteNodeId, graph_jit::StableInputPortId>;
std::optional<BackgroundNodeIdentity> background_node_identity(graph_jit::BackgroundNodePlan const& node)
{
    if (node.resolved_module_input) return BackgroundNodeIdentity{*node.resolved_module_input};
    if (node.stable_identity) return BackgroundNodeIdentity{*node.stable_identity};
    return std::nullopt;
}

bool local_port_semantics_equal(
    graph_jit::BackgroundPortPlan const& current,
    graph_jit::BackgroundPortPlan const& previous)
{
    if (current.direction != previous.direction
        || current.kind != previous.kind
        || current.input_access != previous.input_access
        || current.content_access != previous.content_access
        || current.deferred_tick_lead != previous.deferred_tick_lead
        || current.output_production != previous.output_production
        || current.random_access_input != previous.random_access_input
        || current.tick_sequential_input != previous.tick_sequential_input
        || current.replay_sequential_input != previous.replay_sequential_input
        || current.authored_tock_output != previous.authored_tock_output
        || current.persisted_tick_output != previous.persisted_tick_output
        || current.replayed_tick_output != previous.replayed_tick_output
        || current.full_live_tick_coverage != previous.full_live_tick_coverage
        || current.tick_coverage != previous.tick_coverage
        || current.tick_coverage_has_manual_source != previous.tick_coverage_has_manual_source
        || current.sample_neutral_value != previous.sample_neutral_value
        || current.sample_fallback_value != previous.sample_fallback_value
        || current.coordinate_fallback_value != previous.coordinate_fallback_value
        || current.sample_manual_contribution != previous.sample_manual_contribution
        || current.sequential_history != previous.sequential_history
        || current.output_history != previous.output_history
        || current.output_latency != previous.output_latency
        || current.realization != previous.realization
        || current.sample_layout != previous.sample_layout
        || current.continuous_geometry != previous.continuous_geometry
        || current.absolute_pace_exponent != previous.absolute_pace_exponent
        || current.ordering != previous.ordering
        || current.event_type != previous.event_type
        || current.max_events_per_index != previous.max_events_per_index
        || current.max_event_count != previous.max_event_count
        || current.accumulators.input_change.has_value()
            != previous.accumulators.input_change.has_value()
        || current.accumulators.output_change.has_value()
            != previous.accumulators.output_change.has_value()
        || current.accumulators.output_requirement.has_value()
            != previous.accumulators.output_requirement.has_value()
        || current.accumulators.input_requirement.has_value()
            != previous.accumulators.input_requirement.has_value()) {
        return false;
    }
    return current.direction == graph_jit::PortDirection::input
        ? current.stable_input_identity == previous.stable_input_identity
            && current.stable_input_identity.has_value()
        : background_output_identity(current) == background_output_identity(previous)
            && background_output_identity(current).has_value();
}

bool node_ports_equal(
    graph_jit::BackgroundEvaluationPlan const& current_plan,
    graph_jit::BackgroundNodePlan const& current,
    graph_jit::BackgroundEvaluationPlan const& previous_plan,
    graph_jit::BackgroundNodePlan const& previous)
{
    auto equal_set = [&](std::span<graph_jit::BackgroundPortIndex const> current_ports,
                         std::span<graph_jit::BackgroundPortIndex const> previous_ports) {
        if (current_ports.size() != previous_ports.size()) return false;
        std::vector<bool> matched(previous_ports.size(), false);
        for (auto const current_index : current_ports) {
            if (current_index >= current_plan.ports.size()) return false;
            auto found = previous_ports.size();
            for (std::size_t index = 0; index < previous_ports.size(); ++index) {
                auto const previous_index = previous_ports[index];
                if (matched[index]
                    || previous_index >= previous_plan.ports.size()
                    || !local_port_semantics_equal(
                        current_plan.ports[current_index],
                        previous_plan.ports[previous_index])) {
                    continue;
                }
                found = index;
                break;
            }
            if (found == previous_ports.size()) return false;
            matched[found] = true;
        }
        return true;
    };
    return equal_set(current.inputs, previous.inputs)
        && equal_set(current.outputs, previous.outputs);
}

bool node_local_semantics_equal(
    graph_jit::BackgroundEvaluationPlan const& current_plan,
    graph_jit::BackgroundNodePlan const& current,
    graph_jit::BackgroundEvaluationPlan const& previous_plan,
    graph_jit::BackgroundNodePlan const& previous,
    bool local_semantic_environment_changed)
{
    return !local_semantic_environment_changed
        && ((current.resolved_module_input && current.resolved_module_input == previous.resolved_module_input)
            || (!current.resolved_module_input && !previous.resolved_module_input
                && current.implementation_identity == previous.implementation_identity
                && current.provider_version == previous.provider_version
                && (current.provider_version != 0
                    || std::holds_alternative<graph_jit::CompilerSampleConfiguration>(current.configuration_identity))
                && shares_configuration_identity(current.configuration_identity, previous.configuration_identity)))
        && current.authored_tock_execution == previous.authored_tock_execution
        && current.replays_tick == previous.replays_tick
        && current.full_live_tick_coverage == previous.full_live_tick_coverage
        && current.tracks_live_tick_coverage == previous.tracks_live_tick_coverage
        && current.authored_forward_coverage == previous.authored_forward_coverage
        && current.authored_reverse_coverage == previous.authored_reverse_coverage
        && current.uses_tick_forward_coverage
            == previous.uses_tick_forward_coverage
        && current.uses_replay_reverse_coverage
            == previous.uses_replay_reverse_coverage
        && current.replay_mode == previous.replay_mode
        && current.uses_imported_tick_block_for_replay
            == previous.uses_imported_tick_block_for_replay
        && node_ports_equal(
            current_plan, current, previous_plan, previous);
}

IndexCoverage derive_input_coverage(
    graph_jit::BackgroundEvaluationPlan const& plan,
    std::span<IndexCoverage const> output_coverages,
    graph_jit::BackgroundPortIndex input)
{
    IndexCoverage result;
    auto const output_coverage = [&](graph_jit::BackgroundPortIndex output)
        -> IndexCoverage const& {
        return output_coverages[
            *plan.ports[output].accumulators.output_change];
    };
    for (auto const connection_index : plan.ports[input].incoming_connections) {
        auto const& connection = plan.connections[connection_index];
        bool used_exact_routes = false;
        if (connection.kind == PortKind::sample
            && connection.sample_target_port == input
            && connection.sample_source_port_by_channel.size()
                == connection.sample_read_latencies.size()) {
            used_exact_routes = true;
            auto include_contribution = [&] (
                std::span<std::size_t const> source_indices) {
                std::vector<graph_jit::SequentialCoverageSource> sources;
                sources.reserve(source_indices.size());
                for (auto const source_index : source_indices) {
                    if (source_index >= connection.sample_source_port_by_channel.size())
                        continue;
                    auto const source = connection.sample_source_port_by_channel[source_index];
                    sources.push_back({
                        .output_change = source
                            ? plan.ports[*source].accumulators.output_change : std::nullopt,
                        .read_latency = connection.sample_read_latencies[source_index],
                    });
                }
                result.include(graph_jit::projection_coverage(sources, output_coverages));
            };
            if (connection.sample_projections.empty()) {
                std::vector<std::size_t> source_indices(
                    connection.sample_source_port_by_channel.size());
                std::iota(source_indices.begin(), source_indices.end(), 0);
                include_contribution(source_indices);
            } else {
                for (auto const& projection : connection.sample_projections) {
                    include_contribution(projection.source_channel_indices);
                }
            }
        } else if (connection.kind == PortKind::event) {
            for (auto const& delivery : connection.event_deliveries) {
                if (delivery.target_port != input) continue;
                used_exact_routes = true;
                if (delivery.source_port) {
                    auto const delay = plan.nodes[plan.ports[input].node].replays_tick
                        ? 0 : delivery.live_read_latency;
                    result.include(shifted_forward(output_coverage(*delivery.source_port), delay));
                } else if (delivery.mechanism == graph_jit::PlannedDeliveryMechanism::tick_to_sequential) {
                    result.include(graph_jit::full_index_coverage());
                }
            }
        }
        if (!used_exact_routes) {
            for (auto const source : connection.source_coverage_ports) {
                result.include(output_coverage(source));
            }
        }
    }
    return result;
}

void append_event_regions(
    std::vector<BackgroundInputRegion>& regions, GlobalIndexRange source_range,
    IndexCoverage const& complete, graph_jit::BackgroundConnectionIndex connection,
    BackgroundInputRegionSource source)
{
    auto append = [&](GlobalIndexRange range) {
        regions.push_back({.range = range, .connection = connection, .sources = {source}});
    };
    // IndexCoverage canonicalizes at the signed seam. A fully available
    // logical Region keeps the range that its storage reader exposes.
    if (complete.contains(source_range)) {
        append(source_range);
        return;
    }
    auto const first = regions.size();
    for (auto const available : complete)
        for_each_range_intersection(source_range, available, append);
    // A clipped Region can also straddle the seam. The first and last
    // pieces are adjacent in circular order when they meet at that seam.
    if (regions.size() > first + 1 && regions.back().range.end() == regions[first].range.begin) {
        regions.back().range.extent += regions[first].range.extent;
        regions.erase(regions.begin() + static_cast<std::ptrdiff_t>(first));
    }
}

std::vector<BackgroundInputRegion> derive_input_regions(
    graph_jit::BackgroundEvaluationPlan const& plan,
    std::span<std::vector<GlobalIndexRange> const> output_regions,
    graph_jit::BackgroundPortIndex input,
    IndexCoverage const* requested = nullptr,
    BackgroundSampleDemand const* sample_positions = nullptr,
    std::size_t sparse_sample_budget = 0)
{
    std::vector<BackgroundInputRegion> result;
    std::vector<std::vector<BackgroundInputRegion>> sorted_runs;
    auto const& planned_input = plan.ports[input];
    auto const source_regions = [&](graph_jit::BackgroundPortIndex output,
                                    std::size_t latency = 0) {
        auto const& source = plan.ports[output];
        auto const& regions = output_regions[*source.accumulators.output_change];
        // Tick replay windows are determined by the demand. Authored
        // Regions use their index in the complete published sequence.
        if (!requested || !source.replayed_tick_output)
            return regions;
        auto selected = region_coverage(regions) & shifted_back(*requested, latency);
        if (sample_positions && !sample_positions->empty())
            selected.include(region_coverage(regions)
                & sample_positions->retreated_by(latency).positional_ranges(sparse_sample_budget));
        return std::vector<GlobalIndexRange>{selected.begin(), selected.end()};
    };
    auto const source_ref = [&](graph_jit::BackgroundPortIndex output,
                                std::size_t index, std::optional<std::size_t> route) {
        BackgroundInputRegionSource source{.output = output, .route_index = route};
        auto const& port = plan.ports[output];
        if (!port.replayed_tick_output && !port.persisted_tick_output)
            source.region_index = index;
        return source;
    };
    auto const ordered = [](BackgroundInputRegion const& left, BackgroundInputRegion const& right) {
        return canonical_region_less(left.range, right.range);
    };

    for (auto const connection_index : planned_input.incoming_connections) {
        auto const& connection = plan.connections[connection_index];
        std::vector<BackgroundInputRegion> connection_regions;

        if (connection.kind == PortKind::event) {
            struct EventSource {
                graph_jit::BackgroundPortIndex output;
                std::size_t delivery;
                std::vector<GlobalIndexRange> regions;
            };
            std::optional<IndexCoverage> complete;
            std::vector<EventSource> sources;
            for (std::size_t index = 0; index < connection.event_deliveries.size(); ++index) {
                auto const& delivery = connection.event_deliveries[index];
                if (delivery.target_port != input || !delivery.source_port) continue;
                auto const& source = sources.emplace_back(
                    *delivery.source_port, index, source_regions(*delivery.source_port));
                auto available = region_coverage(source.regions);
                complete = complete ? *complete & available : std::move(available);
            }
            for (auto const& source : sources) {
                for (std::size_t index = 0; index < source.regions.size(); ++index) {
                    append_event_regions(connection_regions, source.regions[index], *complete,
                        connection_index, source_ref(source.output, index, source.delivery));
                }
            }
        } else if (connection.kind == PortKind::sample) {
            auto const append_projection = [&](std::span<std::size_t const> source_indices,
                                               std::optional<std::size_t> projection) {
                struct SampleRoute {
                    graph_jit::BackgroundPortIndex output;
                    std::size_t latency;
                    std::size_t channel;
                };
                std::vector<SampleRoute> routes;
                for (auto const channel : source_indices) {
                    if (channel >= connection.sample_source_port_by_channel.size()
                        || !connection.sample_source_port_by_channel[channel]) continue;
                    auto const output = *connection.sample_source_port_by_channel[channel];
                    auto const latency = connection.sample_read_latencies[channel];
                    if (std::ranges::none_of(routes, [&](auto const& route) {
                            return route.output == output && route.latency == latency;
                        })) {
                        routes.push_back({output, latency, channel});
                    }
                }
                std::vector<std::vector<GlobalIndexRange>> source_ranges;
                std::vector<SampleRegionProjectionSource<GlobalIndexRange>> sources;
                source_ranges.reserve(routes.size());
                sources.reserve(routes.size());
                for (auto const& route : routes) {
                    source_ranges.push_back(source_regions(route.output, route.latency));
                    sources.push_back({source_ranges.back(), route.latency});
                }
                visit_sample_region_projections<GlobalIndexRange>(
                    sources, [&](auto range, std::span<std::size_t const> indices) {
                        auto& region = connection_regions.emplace_back(
                            range, connection_index, projection);
                        region.sources.reserve(indices.size());
                        for (std::size_t index = 0; index < indices.size(); ++index) {
                            auto const& route = routes[index];
                            region.sources.push_back(source_ref(route.output, indices[index], route.channel));
                        }
                        return true;
                    });
            };
            if (connection.sample_target_port == input
                && connection.sample_source_port_by_channel.size()
                    == connection.sample_read_latencies.size()) {
                if (connection.sample_projections.empty()) {
                    std::vector<std::size_t> source_indices(
                        connection.sample_source_port_by_channel.size());
                    std::iota(source_indices.begin(), source_indices.end(), 0);
                    append_projection(source_indices, std::nullopt);
                } else {
                    for (std::size_t index = 0; index < connection.sample_projections.size(); ++index) {
                        append_projection(connection.sample_projections[index].source_channel_indices, index);
                    }
                }
            } else {
                for (auto const source : connection.source_coverage_ports) {
                    auto const regions = source_regions(source);
                    for (std::size_t index = 0; index < regions.size(); ++index) {
                        connection_regions.push_back({
                            .range = regions[index],
                            .connection = connection_index,
                            .sources = {source_ref(source, index, std::nullopt)},
                        });
                    }
                }
            }
        }

        if (planned_input.ordering == CoverageOrdering::sorted) {
            if (!std::ranges::is_sorted(connection_regions, ordered))
                std::stable_sort(connection_regions.begin(), connection_regions.end(), ordered);
            sorted_runs.push_back(std::move(connection_regions));
        } else {
            result.insert(result.end(),
                std::make_move_iterator(connection_regions.begin()),
                std::make_move_iterator(connection_regions.end()));
        }
    }
    if (planned_input.ordering == CoverageOrdering::sorted)
        result = merge_sorted_region_runs(sorted_runs);
    for (std::size_t index = 0; index < result.size(); ++index)
        result[index].logical_index = index;
    return result;
}

bool preserves_event_indices(graph_jit::BackgroundEvaluationPlan const& plan,
    BackgroundInputRegion const& region, graph_jit::BackgroundPortIndex input)
{
    if (region.sources.size() != 1) return false;
    auto const& source = region.sources.front();
    auto const& deliveries = plan.connections[region.connection].event_deliveries;
    if (!source.route_index || *source.route_index >= deliveries.size()) return false;
    auto const& delivery = deliveries[*source.route_index];
    return delivery.conversion.step_count == 0
        && plan.ports[source.output].event_type == plan.ports[input].event_type;
}

struct StableSampleConnectionSource {
    BackgroundOutputIdentity port{};
    std::size_t channel = 0;
    std::size_t read_latency = 0;
    graph_jit::PlannedDeliveryMechanism delivery =
        graph_jit::PlannedDeliveryMechanism::tick_to_sequential;

    bool operator==(StableSampleConnectionSource const&) const = default;
};

struct StableEventConnectionSource {
    BackgroundOutputIdentity port{};
    EventTypeId source_type = EventTypeId::empty;
    EventConversionPlan conversion{};
    graph_jit::PlannedDeliveryMechanism delivery =
        graph_jit::PlannedDeliveryMechanism::tick_to_sequential;

    bool operator==(StableEventConnectionSource const&) const = default;
};

struct StableInputConnectionDescription {
    PortKind kind = PortKind::sample;
    ChannelTypeId sample_source_type = ChannelTypeId::mono;
    ChannelTypeId sample_target_type = ChannelTypeId::mono;
    std::vector<StableSampleConnectionSource> sample_sources{};
    std::vector<std::size_t> sample_target_channels{};
    std::vector<graph_jit::SampleProjectionPlan> sample_projections{};
    EventTypeId event_source_type = EventTypeId::empty;
    EventTypeId event_target_type = EventTypeId::empty;
    EventConversionPlan event_conversion{};
    std::vector<StableEventConnectionSource> event_sources{};
    bool requires_conversion = false;

    bool operator==(StableInputConnectionDescription const&) const = default;
};

std::optional<StableInputConnectionDescription> describe_connection(
    graph_jit::BackgroundEvaluationPlan const& plan,
    graph_jit::BackgroundConnectionIndex connection_index,
    graph_jit::BackgroundPortIndex input)
{
    if (connection_index >= plan.connections.size()) return std::nullopt;
    auto const& connection = plan.connections[connection_index];
    StableInputConnectionDescription result{
        .kind = connection.kind,
        .sample_source_type = connection.sample_source_type,
        .sample_target_type = connection.sample_target_type,
        .sample_projections = connection.sample_projections,
        .event_source_type = connection.event_source_type,
        .event_target_type = connection.event_target_type,
        .event_conversion = connection.event_conversion,
        .requires_conversion = connection.requires_conversion,
    };
    if (connection.kind == PortKind::sample) {
        if (!connection.sample_target_port
            || *connection.sample_target_port != input
            || connection.sample_source_channels.size()
                != connection.sample_source_port_by_channel.size()
            || connection.sample_source_channels.size()
                != connection.sample_read_latencies.size()
            || connection.sample_source_channels.size()
                != connection.sample_deliveries.size()) {
            return std::nullopt;
        }
        result.sample_sources.reserve(connection.sample_source_channels.size());
        for (std::size_t index = 0;
             index < connection.sample_source_channels.size(); ++index) {
            auto const source = connection.sample_source_port_by_channel[index];
            if (!source || *source >= plan.ports.size()
                || !background_output_identity(plan.ports[*source])) {
                return std::nullopt;
            }
            auto const& configured_source = plan.ports[*source].configured_port;
            if (configured_source.node_bundle_handle
                    != connection.sample_source_channels[index].bundle
                || configured_source.port_index
                    != connection.sample_source_channels[index].port) {
                return std::nullopt;
            }
            result.sample_sources.push_back({
                .port = *background_output_identity(plan.ports[*source]),
                .channel = connection.sample_source_channels[index].channel,
                .read_latency = connection.sample_read_latencies[index],
                .delivery = connection.sample_deliveries[index],
            });
        }
        result.sample_target_channels.reserve(
            connection.sample_target_channels.size());
        for (auto const& target : connection.sample_target_channels) {
            auto const& configured_input = plan.ports[input].configured_port;
            if (target.bundle != configured_input.node_bundle_handle
                || target.port != configured_input.port_index) {
                return std::nullopt;
            }
            result.sample_target_channels.push_back(target.channel);
        }
        return result;
    }

    for (auto const& delivery : connection.event_deliveries) {
        if (!delivery.target_port || *delivery.target_port != input) continue;
        if (!delivery.source_port || *delivery.source_port >= plan.ports.size()
            || !background_output_identity(plan.ports[*delivery.source_port])) {
            return std::nullopt;
        }
        auto const& configured_source =
            plan.ports[*delivery.source_port].configured_port;
        if (configured_source.node_bundle_handle != delivery.source.bundle
            || configured_source.port_index != delivery.source.port) {
            return std::nullopt;
        }
        result.event_sources.push_back({
            .port = *background_output_identity(plan.ports[*delivery.source_port]),
            .source_type = delivery.source_type,
            .conversion = delivery.conversion,
            .delivery = delivery.mechanism,
        });
    }
    return result;
}

bool input_connections_equal(
    graph_jit::BackgroundEvaluationPlan const& current_plan,
    graph_jit::BackgroundPortIndex current_input,
    graph_jit::BackgroundEvaluationPlan const& previous_plan,
    graph_jit::BackgroundPortIndex previous_input)
{
    auto const& current = current_plan.ports[current_input];
    auto const& previous = previous_plan.ports[previous_input];
    if (current.kind != previous.kind
        || current.input_access != previous.input_access
        || current.content_access != previous.content_access
        || current.sample_layout != previous.sample_layout
        || current.continuous_geometry != previous.continuous_geometry
        || current.event_type != previous.event_type
        || current.incoming_connections.size()
            != previous.incoming_connections.size()) {
        return false;
    }

    for (std::size_t index = 0;
         index < current.incoming_connections.size(); ++index) {
        auto current_description = describe_connection(
            current_plan, current.incoming_connections[index], current_input);
        auto previous_description = describe_connection(
            previous_plan,
            previous.incoming_connections[index],
            previous_input);
        if (!current_description || !previous_description
            || *current_description != *previous_description) {
            return false;
        }
    }
    return true;
}

} // namespace

// All discovery scopes share this selected candidate's scheduler. A scope has
// its own reverse accumulators; exploratory requirements never enter the final
// value-production selection. Request owners outlive every suspended consumer.
struct BackgroundPropagationWorkspace::PlanningState {
    using Operation = BackgroundPlanningScheduler::Operation;
    struct Group {
        BackgroundPropagationWorkspace* workspace = nullptr;
        BackgroundCoverageDemandExpansion expansion{};
        std::vector<Operation*> forward{};
        std::vector<Operation*> reverse{};
    };
    struct InformationRequest {
        Operation* operation = nullptr;
        graph_jit::BackgroundPortIndex input = 0;
        CoverageInformation information = CoverageInformation::coverage;
        IndexCoverage positions{};
        BackgroundSampleDemand sample_positions{};
        RegionRef selected_region{};
        std::vector<IndexRun> selected_runs{};
        CoverageInputInfo metadata{};
        GlobalIndex neighbor_position{};
        EventNeighborMode neighbor_mode = EventNeighborMode::bounded;
        EventNeighbors neighbors{};
        BackgroundDiscoveredInput contents{};
        CoverageRequestId id = 0;
    };
    BackgroundPlanningScheduler scheduler{};
    ~PlanningState() { scheduler.cancel(); }
    BackgroundPropagationWorkspace* root = nullptr;
    std::uint64_t snapshot = 0;
    CompiledGraphBackgroundOperations const* operations = nullptr;
    BackgroundCoverageDemandExpansion expansion{};
    BackgroundCoverageDiscovery discovery{};
    std::vector<std::unique_ptr<Group>> groups{};
    std::map<BackgroundPropagationWorkspace*, Group*> group_by_workspace{};
    std::vector<std::unique_ptr<InformationRequest>> requests{};
    std::map<Operation*, std::map<std::pair<graph_jit::BackgroundPortIndex, CoverageInformation>,
        std::vector<InformationRequest*>>> requests_by_input{};

    Group& group(BackgroundPropagationWorkspace* workspace) {
        auto found = group_by_workspace.find(workspace);
        if (found == group_by_workspace.end()) throw std::logic_error("missing coverage planning scope");
        return *found->second;
    }
};

void BackgroundPropagationWorkspace::initialize_planning()
{
    using Operation = PlanningState::Operation;
    auto& state = *planning_state_;
    auto group = std::make_unique<PlanningState::Group>();
    group->workspace = this;
    auto dispatch = [this](std::span<Operation* const> ready) {
        auto const reverse = ready.front()->reverse;
        auto const bit = reverse ? graph_jit::BackgroundNodeActivity::reverse
                                 : graph_jit::BackgroundNodeActivity::forward;
        std::vector<graph_jit::BackgroundNodeActivity> saved;
        saved.reserve(node_calls_.size());
        for (auto& frame : node_calls_) {
            saved.push_back(frame.activity);
            frame.activity = graph_jit::BackgroundNodeActivity::none;
        }
        for (auto const* operation : ready) node_calls_[operation->node].activity = bit;
        auto restore = [&] {
            for (std::size_t node = 0; node < node_calls_.size(); ++node) {
                node_calls_[node].activity = node_calls_[node].activity | saved[node];
            }
        };
        try {
            if (reverse) planning_state_->operations->propagate_reverse(&call_);
            else planning_state_->operations->propagate_forward(&call_);
            // A generated traversal may activate and visit further upstream
            // nodes in this same reverse call. Native test roots use the same
            // activity contract without going through the authored adapters.
            auto& group = planning_state_->group(this);
            auto const& dispatched = reverse ? group.reverse : group.forward;
            for (auto* operation : dispatched) {
                if (graph_jit::has_activity(node_calls_[operation->node].activity, bit)
                    && operation->queued && operation->waiting.empty()) {
                    planning_state_->scheduler.finish(*operation);
                }
            }
        } catch (...) { restore(); throw; }
        restore();
    };
    for (std::size_t node = 0; node < node_calls_.size(); ++node) {
        for (bool reverse : {false, true}) {
            auto& operation = state.scheduler.add_operation(this, node, reverse, dispatch);
            operation.yielded = [this, node, &operation] {
                if (operation.reverse) return;
                for (auto const output : plan_->nodes[node].outputs) {
                    if (published_outputs_[output] || operation.complete) {
                        output_ready_[output] = true;
                        finalized_outputs_[output] = true;
                        published_outputs_[output] = false;
                        if (auto const index = port(output).accumulators.output_change)
                            candidate_output_coverage_known_[*index] = true;
                    }
                }
            };
            (reverse ? group->reverse : group->forward).push_back(&operation);
            CoveragePlanningContext context{
                .owner = &operation,
                .access = state.scheduler.access(),
                .request = +[](void* opaque, bool event, std::size_t input,
                    CoverageInformation information, CoverageInputDemand positions) {
                    auto& operation = *static_cast<Operation*>(opaque);
                    auto& workspace = *static_cast<BackgroundPropagationWorkspace*>(operation.group);
                    return workspace.request_information(opaque, event, input, information, positions);
                },
                .start = +[](void* opaque, CoverageTaskHandle task) {
                    auto& operation = *static_cast<Operation*>(opaque);
                    operation.scheduler->submit(operation, task);
                },
                .completed = +[](void* opaque) {
                    auto& operation = *static_cast<Operation*>(opaque);
                    operation.scheduler->finish(operation);
                },
            };
            if (reverse) node_calls_[node].reverse.planning = context;
            else node_calls_[node].forward.planning = context;
        }
    }
    state.group_by_workspace[this] = group.get();
    state.groups.push_back(std::move(group));
}

void BackgroundPropagationWorkspace::refresh_input(graph_jit::BackgroundPortIndex input)
{
    if (!planning_state_ || planning_state_->root == this) return;
    auto& change = input_changes_[input_index(input)];
    change.coverage = input_coverage(input);
    change.regions = input_regions(input);
    change.region_view = change.regions;
    if (port(input).accumulators.input_requirement) {
        input_requirements_[*port(input).accumulators.input_requirement].coverage = change.coverage;
    }
}

bool BackgroundPropagationWorkspace::input_information_ready(
    graph_jit::BackgroundPortIndex input) const
{
    auto const& root = *planning_state_->root;
    auto const& planned = port(input);
    for (auto const connection_index : planned.incoming_connections) {
        auto const& connection = plan_->connections[connection_index];
        bool exact = false;
        if (connection.kind == PortKind::sample && connection.sample_target_port == input) {
            for (auto const source : connection.sample_source_port_by_channel) {
                if (source && !root.output_ready_[*source]) return false;
            }
            exact = !connection.sample_source_port_by_channel.empty();
        } else if (connection.kind == PortKind::event) {
            for (auto const& delivery : connection.event_deliveries) {
                if (delivery.target_port != input || !delivery.source_port) continue;
                if (!root.output_ready_[*delivery.source_port]) return false;
                exact = true;
            }
        }
        if (!exact) for (auto const source : connection.source_coverage_ports) {
            if (!root.output_ready_[source]) return false;
        }
    }
    return true;
}

CoverageRequestId BackgroundPropagationWorkspace::request_information(
    void* opaque, bool event, std::size_t local_input,
    CoverageInformation information, CoverageInputDemand demand)
{
    auto& operation = *static_cast<PlanningState::Operation*>(opaque);
    auto& state = *planning_state_;
    auto const& calls = node_call_data_[operation.node];
    auto const& inputs = event ? calls.event_inputs : calls.sample_inputs;
    if (local_input >= inputs.size()) throw std::out_of_range("coverage discovery input is out of range");
    auto const input = inputs[local_input];
    auto const requested = demand.positional ? *demand.positional : IndexCoverage{};
    BackgroundSampleDemand samples;
    samples.include(demand.sample_positions);
    if (demand.selection.region
        && (details::RegionRefAccess::snapshot(demand.selection.region) != state.snapshot
            || details::RegionRefAccess::port(demand.selection.region) != input))
        throw std::invalid_argument("discovery Region reference does not belong to this input and snapshot");
    auto& matching = state.requests_by_input[&operation][{input, information}];
    for (auto const* request : matching) {
        if (request->positions == requested && request->sample_positions == samples
            && request->selected_region == demand.selection.region
            && request->neighbor_position == demand.neighbor_position
            && request->neighbor_mode == demand.neighbor_mode
            && std::ranges::equal(request->selected_runs, demand.selection.runs)) return request->id;
    }
    auto owned = std::make_unique<PlanningState::InformationRequest>();
    auto* request = owned.get();
    request->operation = &operation;
    request->input = input;
    request->information = information;
    request->positions = requested;
    request->sample_positions = std::move(samples);
    request->selected_region = demand.selection.region;
    request->neighbor_position = demand.neighbor_position;
    request->neighbor_mode = demand.neighbor_mode;
    request->selected_runs.assign(demand.selection.runs.begin(), demand.selection.runs.end());
    auto description = std::string{"input port "} + std::to_string(input)
        + " (" + port(input).name + ") "
        + (information == CoverageInformation::coverage ? "coverage" : "contents");
    for (auto region : requested) {
        description += " {begin=" + std::to_string(region.begin.signed_value())
            + ", extent=" + std::to_string(region.extent) + "}";
    }
    request->id = state.scheduler.request(operation,
        [&state, request] {
            auto& root = *state.root;
            if (!root.input_information_ready(request->input)) return false;
            auto const index = root.input_index(request->input);
            request->metadata.coverage = root.input_changes_[index].coverage;
            request->metadata.regions = root.input_changes_[index].regions;
            auto const inputs = derive_input_regions(*root.plan_,
                root.candidate_output_region_ranges_, request->input);
            request->metadata.resolved_regions.reserve(inputs.size());
            for (std::size_t i = 0; i < inputs.size(); ++i) {
                auto const& region = inputs[i];
                auto& metadata = request->metadata.resolved_regions.emplace_back(
                    details::RegionRefAccess::make(state.snapshot, request->input, i), region.range);
                if (root.port(request->input).kind != PortKind::event
                    || !preserves_event_indices(*root.plan_, region, request->input)) continue;
                auto const& source = region.sources.front();
                if (!source.region_index) continue;
                auto const& publication = root.candidate_output_event_regions_[root.output_index(source.output)];
                if (*source.region_index < publication.size()
                    && publication[*source.region_index].range == region.range)
                    metadata.event_index = publication[*source.region_index].index;
            }
            if (request->information == CoverageInformation::event_neighbors) {
                auto const region_index = details::RegionRefAccess::index(request->selected_region);
                if (!request->selected_region || region_index >= request->metadata.resolved_regions.size())
                    throw std::invalid_argument("event neighbor request requires a resolved Region");
                auto const& region = request->metadata.resolved_regions[region_index];
                if (!region.event_index)
                    throw std::invalid_argument("event Region does not support indexed neighbors");
                if (!state.discovery.materialize)
                    throw std::logic_error("event neighbor discovery requires a background transaction");
                if (request->neighbor_mode == EventNeighborMode::bounded
                    && !region.range.contains(request->neighbor_position))
                    throw std::invalid_argument("bounded event neighbor lookup is outside its Region");
                auto const count = region.event_index->event_count;
                if (count == 0) return true;
                std::vector<std::pair<std::uint64_t, TimedEvent>> probes;
                auto const read = [&](std::uint64_t index) {
                    auto const found = std::ranges::find_if(probes, [&](auto const& probe) { return probe.first == index; });
                    if (found != probes.end()) return found->second;
                    IndexRun const run{index, 1};
                    auto prepared = root.prepare_discovery(request->input, {}, {}, {region.region, std::span{&run, 1}});
                    auto value = state.discovery.materialize(state.discovery.data, std::move(prepared), request->input);
                    auto const events = value.events.event_runs;
                    if (events.size() != 1 || events.front().slots.size() != 1)
                        throw std::runtime_error("indexed event neighbor is unavailable");
                    return probes.emplace_back(index, events.front().slots.front()).second;
                };
                auto const offset = region.range.begin.forward_distance_to(request->neighbor_position);
                std::uint64_t first = 0;
                auto last = count;
                while (first < last) {
                    auto const middle = first + (last - first) / 2;
                    if (region.range.begin.forward_distance_to(read(middle).time) <= offset)
                        first = middle + 1;
                    else last = middle;
                }
                if (first > 0 || request->neighbor_mode == EventNeighborMode::cyclic)
                    request->neighbors.at_or_before = read(first ? first - 1 : count - 1);
                if (first < count || request->neighbor_mode == EventNeighborMode::cyclic)
                    request->neighbors.after = read(first == count ? 0 : first);
            }
            if (request->information == CoverageInformation::contents) {
                if (!state.discovery.materialize) {
                    throw std::logic_error("input-content discovery requires a background transaction");
                }
                auto prepared = root.prepare_discovery(request->input, request->positions, request->sample_positions,
                    {request->selected_region, request->selected_runs});
                request->contents = state.discovery.materialize(state.discovery.data,
                    std::move(prepared), request->input);
                if (!request->contents.owner) {
                    throw std::logic_error("discovery input does not retain its selected storage");
                }
                request->metadata.regions.clear();
                if (root.port(request->input).kind == PortKind::sample) {
                    auto const& input = request->contents.continuous;
                    request->metadata.coverage = input.coverage();
                    for (auto const& region : input.region_views()) {
                        request->metadata.regions.push_back(region.range);
                    }
                } else {
                    auto const& input = request->contents.events;
                    request->metadata.coverage = input.coverage();
                    auto const regions = input.regions();
                    request->metadata.regions.assign(regions.begin(), regions.end());
                }
            }
            return true;
        },
        [&state, request]() -> void const* {
            if (request->information == CoverageInformation::coverage) return &request->metadata;
            if (request->information == CoverageInformation::event_neighbors) return &request->neighbors;
            return state.root->port(request->input).kind == PortKind::sample
                ? static_cast<void const*>(&request->contents.continuous)
                : static_cast<void const*>(&request->contents.events);
        }, std::move(description));
    matching.push_back(request);
    state.requests.push_back(std::move(owned));
    if (information == CoverageInformation::coverage) state.scheduler.resolve_metadata(request->id);
    return request->id;
}

void BackgroundPropagationWorkspace::before_metadata_read(void* opaque)
{
    auto const& read = *static_cast<MetadataRead*>(opaque);
    auto& workspace = *read.owner;
    if (!workspace.planning_state_) return;
    auto& operation = *(read.reverse
        ? workspace.planning_state_->group(&workspace).reverse
        : workspace.planning_state_->group(&workspace).forward)[workspace.port(read.input).node];
    auto const& calls = workspace.node_call_data_[operation.node];
    auto const event = workspace.port(read.input).kind == PortKind::event;
    auto const& ports = event ? calls.event_inputs : calls.sample_inputs;
    auto const found = std::ranges::find(ports, read.input);
    auto const id = workspace.request_information(&operation, event,
        static_cast<std::size_t>(found - ports.begin()), CoverageInformation::coverage, {});
    (void)workspace.planning_state_->scheduler.read(id);
    workspace.refresh_input(read.input);
}

void BackgroundPropagationWorkspace::mark_forward_pending(graph_jit::BackgroundNodeIndex node)
{
    if (!planning_state_ || planning_state_->root != this) return;
    auto const& planned = plan_->nodes[node];
    if (planned.tracks_live_tick_coverage && !planned.uses_tick_forward_coverage) return;
    auto& operation = *planning_state_->group(this).forward[node];
    planning_state_->scheduler.schedule(operation);
    for (auto const output : plan_->nodes[node].outputs) {
        if (!finalized_outputs_[output] && !port(output).persisted_tick_output)
            output_ready_[output] = false;
    }
}

void BackgroundPropagationWorkspace::run_planning(bool reverse)
{
    auto& group = planning_state_->group(this);
    auto const& operations = reverse ? group.reverse : group.forward;
    planning_state_->scheduler.run_until([&] {
        return std::ranges::all_of(operations, [](auto const* operation) {
            return !operation->queued || operation->complete;
        });
    });
}

PreparedCoveragePropagation BackgroundPropagationWorkspace::prepare_discovery(
    graph_jit::BackgroundPortIndex input, IndexCoverage const& requested, BackgroundSampleDemand const& sample_positions,
    RegionSelection selection)
{
    CoveragePropagationRequest roots;
    roots.input_demands.push_back({.port = input, .required = requested, .sample_positions = sample_positions});
    if (selection.region)
        roots.indexed_input_demands.push_back({input, details::RegionRefAccess::index(selection.region),
            {selection.runs.begin(), selection.runs.end()}});
    return prepare_discovery(roots);
}

PreparedCoveragePropagation BackgroundPropagationWorkspace::prepare_discovery(CoveragePropagationRequest const& roots)
{
    BackgroundCoverageState snapshot;
    snapshot.output_coverages_ = candidate_output_coverages_;
    snapshot.output_region_ranges_ = candidate_output_region_ranges_;
    snapshot.output_event_regions_ = candidate_output_event_regions_;
    snapshot.output_coverage_known_ = candidate_output_coverage_known_;
    BackgroundPropagationWorkspace scope{*plan_, sample_rate_};
    scope.planning_state_ = planning_state_;
    // Discovery completes only its explicit demand. Unrelated stale persisted
    // output completion still belongs to the surrounding final transaction.
    auto expansion = planning_state_->expansion;
    struct Expansion { BackgroundCoverageDemandExpansion original; } selected{expansion};
    expansion.data = &selected;
    expansion.restrict_input = nullptr;
    expansion.select_demands = nullptr;
    expansion.cached_output = +[](void* opaque, graph_jit::BackgroundPortIndex port) {
        auto const& original = static_cast<Expansion*>(opaque)->original;
        return original.cached_output && original.cached_output(original.data, port);
    };
    expansion.expand_output = +[](void* opaque, graph_jit::BackgroundPortIndex output,
        IndexCoverage const& available, std::span<GlobalIndexRange const> regions,
        IndexCoverage const& changed, IndexCoverage const& required, IndexCoverage& additional) {
        auto const& original = static_cast<Expansion*>(opaque)->original;
        if (original.expand_output && !required.empty()) original.expand_output(original.data,
            output, available, regions, changed & required, required, additional);
    };
    return scope.prepare(*planning_state_->operations, snapshot, roots, expansion,
        planning_state_->discovery);
}

[[nodiscard]] graph_jit::BackgroundPortPlan const&
BackgroundPropagationWorkspace::port(graph_jit::BackgroundPortIndex index) const
{
    return plan_->ports[index];
}

[[nodiscard]] std::size_t BackgroundPropagationWorkspace::output_index(
    graph_jit::BackgroundPortIndex index) const
{
    auto const& planned = port(index);
    return *planned.accumulators.output_change;
}

[[nodiscard]] std::size_t BackgroundPropagationWorkspace::input_index(
    graph_jit::BackgroundPortIndex index) const
{
    auto const& planned = port(index);
    return *planned.accumulators.input_change;
}

[[nodiscard]] IndexCoverage BackgroundPropagationWorkspace::input_coverage(
    graph_jit::BackgroundPortIndex input) const
{
    auto const& source = planning_state_ ? *planning_state_->root : *this;
    return derive_input_coverage(*plan_, source.candidate_output_coverages_, input);
}

[[nodiscard]] std::vector<GlobalIndexRange>
BackgroundPropagationWorkspace::input_regions(
    graph_jit::BackgroundPortIndex input) const
{
    auto const& source = planning_state_ ? *planning_state_->root : *this;
    return input_region_ranges(derive_input_regions(*plan_, source.candidate_output_region_ranges_, input));
}

void BackgroundPropagationWorkspace::activate_node(
    graph_jit::BackgroundNodeIndex node,
    graph_jit::BackgroundNodeActivity activity)
{
    auto const& planned = plan_->nodes[node];
    if (activity == graph_jit::BackgroundNodeActivity::forward
        && planned.tracks_live_tick_coverage && !planned.uses_tick_forward_coverage) return;
    node_calls_[node].activity = node_calls_[node].activity | activity;
    if (!planning_state_) return;
    if (graph_jit::has_activity(activity, graph_jit::BackgroundNodeActivity::forward)) {
        mark_forward_pending(node);
    }
    if (graph_jit::has_activity(activity, graph_jit::BackgroundNodeActivity::reverse)) {
        planning_state_->scheduler.schedule(*planning_state_->group(this).reverse[node]);
    }
}

void BackgroundPropagationWorkspace::add_input_change(
    graph_jit::BackgroundPortIndex input, IndexCoverage const& changed,
    bool recompute_coverage)
{
    auto const& planned = port(input);
    if (!planned.accumulators.input_change)
        return;
    auto const index = input_index(input);
    auto& accumulator = input_changes_[index];
    auto const previous_changed = accumulator.changed;
    if (recompute_coverage) {
        auto const next = input_coverage(input);
        accumulator.changed.include(
            coverage_difference(accumulator.coverage, next));
        accumulator.coverage = next;
        accumulator.regions = input_regions(input);
        accumulator.region_view = accumulator.regions;
        auto const requirement_index = *planned.accumulators.input_requirement;
        input_requirements_[requirement_index].coverage = next;
    }
    accumulator.changed.include(changed);
    if (previous_changed != accumulator.changed) {
        activate_node(planned.node, graph_jit::BackgroundNodeActivity::forward);
    }
}

void BackgroundPropagationWorkspace::route_output_change(
    graph_jit::BackgroundPortIndex output, IndexCoverage const& changed,
    bool coverage_changed)
{
    auto const& planned_output = port(output);
    for (auto const connection_index : planned_output.outgoing_connections) {
        auto const& connection = plan_->connections[connection_index];
        bool used_exact_routes = false;
        if (connection.kind == PortKind::sample
            && connection.sample_target_port
            && connection.sample_source_port_by_channel.size()
                == connection.sample_read_latencies.size()) {
            for (std::size_t channel = 0;
                 channel < connection.sample_source_port_by_channel.size();
                 ++channel) {
                if (connection.sample_source_port_by_channel[channel]
                    != output) {
                    continue;
                }
                add_input_change(
                    *connection.sample_target_port,
                    shifted_forward(
                        changed, connection.sample_read_latencies[channel]),
                    coverage_changed);
                used_exact_routes = true;
            }
        } else if (connection.kind == PortKind::event) {
            for (auto const& delivery : connection.event_deliveries) {
                if (delivery.source_port != output || !delivery.target_port)
                    continue;
                auto const target = *delivery.target_port;
                auto const delay = plan_->nodes[port(target).node].replays_tick ? 0 : delivery.live_read_latency;
                add_input_change(target, shifted_forward(changed, delay), coverage_changed);
                used_exact_routes = true;
            }
        }
        if (!used_exact_routes) {
            for (auto const target : connection.target_coverage_ports) {
                add_input_change(target, changed, coverage_changed);
            }
        }
    }
}

void BackgroundPropagationWorkspace::set_input_change(
    graph_jit::BackgroundPortIndex input,
    std::span<GlobalIndexRange const> regions,
    IndexCoverage const& coverage,
    IndexCoverage const& changed)
{
    auto const& planned = port(input);
    auto canonical_regions = std::vector<GlobalIndexRange>{};
    if (regions.empty() && !coverage.empty()) {
        canonical_regions = coverage_regions(coverage);
        regions = canonical_regions;
    }
    if (region_coverage(regions) != coverage) {
        throw std::invalid_argument(
            "input change Region ranges do not match coverage");
    }
    auto const index = input_index(input);
    auto& accumulator = input_changes_[index];
    accumulator.changed.include(
        coverage_difference(accumulator.coverage, coverage));
    accumulator.changed.include(changed);
    accumulator.coverage = coverage;
    accumulator.regions.assign(regions.begin(), regions.end());
    accumulator.region_view = accumulator.regions;
    auto const requirement_index = *planned.accumulators.input_requirement;
    input_requirements_[requirement_index].coverage = coverage;
    if (!accumulator.changed.empty()) {
        activate_node(planned.node, graph_jit::BackgroundNodeActivity::forward);
    }
}

void BackgroundPropagationWorkspace::publish_output_coverage(
    graph_jit::BackgroundPortIndex output, IndexCoverage const& published_coverage)
{
    auto const& planned = port(output);
    // Generated full/union callbacks choose their coverage explicitly. An
    // authored callback retains its own contract even on a replayed Tick.
    auto const& coverage = published_coverage;
    if (planned.output_production != OutputProductionKind::sequential) {
        throw std::invalid_argument(
            "Region or Coverage output must publish Region ranges");
    }
    auto const index = output_index(output);
    if (planning_state_ && planning_state_->root == this && finalized_outputs_[output]
        && planning_state_->group(this).forward[planned.node]->queued
        && candidate_output_coverages_[index] != coverage) {
        throw std::logic_error("forward planner changed output coverage after publishing it across a suspension");
    }
    candidate_output_region_ranges_[index] = coverage_regions(coverage);
    candidate_output_event_regions_[index].clear();
    candidate_output_coverage_known_[index] = true;
    published_outputs_[output] = true;
    output_ready_[output] = true;
    auto& accumulator = output_changes_[index];
    auto const changed =
        coverage_difference(candidate_output_coverages_[index], coverage);
    candidate_output_coverages_[index] = coverage;
    accumulator.touched = accumulator.touched || !changed.empty();
    accumulator.changed.include(changed);
    route_output_change(output, changed, true);
}

void BackgroundPropagationWorkspace::publish_output_regions(
    graph_jit::BackgroundPortIndex output,
    std::span<GlobalIndexRange const> regions,
    std::span<EventRegionInfo const> event_regions)
{
    auto const& planned = port(output);
    if (planned.output_production == OutputProductionKind::sequential) {
        throw std::invalid_argument(
            "Tick output must publish positional coverage");
    }
    if (planned.output_production == OutputProductionKind::region
        && regions.size() != 1) {
        throw std::invalid_argument(
            "Region output must publish exactly one Region range");
    }
    if (std::ranges::any_of(
            regions, [](GlobalIndexRange region) { return region.empty(); })) {
        throw std::invalid_argument(
            "output cannot publish an empty Region range");
    }
    if (planned.ordering == CoverageOrdering::sorted) {
        auto const canonical_less = [](
            GlobalIndexRange left, GlobalIndexRange right) {
            if (left.begin == right.begin) return left.extent > right.extent;
            return SignedGlobalIndexOrder{}(left.begin, right.begin);
        };
        if (!std::ranges::is_sorted(regions, canonical_less)) {
            throw std::invalid_argument(
                "sorted Coverage output published Regions out of canonical order");
        }
    }

    auto const index = output_index(output);
    auto next_regions = std::vector<GlobalIndexRange>{
        regions.begin(), regions.end()};
    auto next_coverage = region_coverage(next_regions);
    std::vector<EventRegionInfo> next_event_regions(event_regions.begin(), event_regions.end());
    for (auto const& region : event_regions) {
        if (region.index && region.index->constant_value
            && !event_matches_type(*region.index->constant_value, planned.event_type))
            throw std::invalid_argument("indexed event Region payload has the wrong type");
    }
    auto const shape_changed = candidate_output_region_ranges_[index] != next_regions
        || candidate_output_event_regions_[index] != next_event_regions;
    if (planning_state_ && planning_state_->root == this && finalized_outputs_[output]
        && planning_state_->group(this).forward[planned.node]->queued
        && shape_changed) {
        throw std::logic_error("forward planner changed output Regions after publishing them across a suspension");
    }
    auto& accumulator = output_changes_[index];
    auto changed = coverage_difference(
        candidate_output_coverages_[index], next_coverage);
    if (shape_changed) {
        changed.include(candidate_output_coverages_[index]);
        changed.include(next_coverage);
    }
    candidate_output_region_ranges_[index] = std::move(next_regions);
    candidate_output_event_regions_[index] = std::move(next_event_regions);
    candidate_output_coverage_known_[index] = true;
    published_outputs_[output] = true;
    output_ready_[output] = true;
    candidate_output_coverages_[index] = std::move(next_coverage);
    accumulator.touched = accumulator.touched || shape_changed
        || !changed.empty();
    accumulator.changed.include(changed);
    route_output_change(output, changed, true);
}

void BackgroundPropagationWorkspace::publish_output_change(
    graph_jit::BackgroundPortIndex output, IndexCoverage const& changed)
{
    if (changed.empty())
        return;
    auto const index = output_index(output);
    if (planning_state_ && planning_state_->root == this && finalized_outputs_[output]
        && !(changed - output_changes_[index].changed).empty()) {
        throw std::logic_error("forward planner changed output invalidation after publishing it across a suspension");
    }

    auto& accumulator = output_changes_[index];
    accumulator.touched = true;
    accumulator.changed.include(changed);
    route_output_change(output, changed, false);
}

void BackgroundPropagationWorkspace::publish_output_coverage_callback(
    void* opaque, IndexCoverage const& coverage)
{
    auto& accumulator = *static_cast<OutputChangeAccumulator*>(opaque);
    accumulator.owner->publish_output_coverage(accumulator.port, coverage);
}

void BackgroundPropagationWorkspace::publish_output_regions_callback(
    void* opaque, GlobalIndexRange const* regions, std::size_t count)
{
    auto& accumulator = *static_cast<OutputChangeAccumulator*>(opaque);
    accumulator.owner->publish_output_regions(
        accumulator.port, {regions, count});
}

void BackgroundPropagationWorkspace::publish_output_event_regions(
    void* opaque, std::span<EventRegionInfo const> regions)
{
    auto& accumulator = *static_cast<OutputChangeAccumulator*>(opaque);
    std::vector<GlobalIndexRange> ranges;
    ranges.reserve(regions.size());
    for (auto const& region : regions) ranges.push_back(region.range);
    accumulator.owner->publish_output_regions(accumulator.port, ranges, regions);
}

void BackgroundPropagationWorkspace::publish_output_change_callback(
    void* opaque, IndexCoverage const& changed)
{
    auto& accumulator = *static_cast<OutputChangeAccumulator*>(opaque);
    accumulator.owner->publish_output_change(accumulator.port, changed);
}

void BackgroundPropagationWorkspace::require_output(
    graph_jit::BackgroundPortIndex output, IndexCoverage const& requested,
    bool activate_producer)
{
    auto const& planned = port(output);
    auto const change_index = output_index(output);
    auto const requirement_index = *planned.accumulators.output_requirement;
    if (planning_state_ && planning_state_->root != this) {
        auto const& root = *planning_state_->root;
        candidate_output_coverages_[change_index] = root.candidate_output_coverages_[change_index];
        candidate_output_region_ranges_[change_index] = root.candidate_output_region_ranges_[change_index];
        candidate_output_coverage_known_[change_index] = root.candidate_output_coverage_known_[change_index];
    }
    auto required = requested & candidate_output_coverages_[change_index];
    if (required.empty())
        return;
    auto const previous = output_requirements_[requirement_index].required;
    auto& requirement = output_requirements_[requirement_index];
    requirement.required.include(required);
    // Reverse callbacks can discover buffered dependencies after the initial
    // demand pass. Resolve missing/stale contents when the requirement arrives,
    // so their producers enter the same reverse-planning schedule.
    if (!activate_producer && planning_state_) {
        auto const& expansion = planning_state_->group(this).expansion;
        if (expansion.expand_output) {
            IndexCoverage missing;
            expansion.expand_output(expansion.data, output,
                candidate_output_coverages_[change_index],
                candidate_output_region_ranges_[change_index],
                output_changes_[change_index].changed,
                output_requirements_[requirement_index].required, missing);
            if (!missing.empty()) {
                output_requirements_[requirement_index].required.include(missing);
                activate_producer = true;
            }
        }
    }
    if (planned.kind == PortKind::sample && previous != requirement.required) {
        requirement.sample_positions.include(requirement.required);
        requirement.positions_view = requirement.sample_positions.view();
        requirement.sample_selections_dirty = true;
    }
    if (!activate_producer) return;
    // A repeated requirement, for example from a resumed consumer, does not
    // reschedule a producer that already plans for it. A persisted boundary
    // first records its demand without activating the producer; demand
    // expansion then activates it for that same range when stored data are
    // missing, so an unscheduled producer is activated even without growth.
    auto const grew = previous != output_requirements_[requirement_index].required;
    auto const scheduled = planning_state_
        ? planning_state_->group(this).reverse[planned.node]->queued
        : graph_jit::has_activity(node_calls_[planned.node].activity,
            graph_jit::BackgroundNodeActivity::reverse);
    if (grew || !scheduled) {
        activate_node(planned.node, graph_jit::BackgroundNodeActivity::reverse);
    }
}

void BackgroundPropagationWorkspace::require_input(
    graph_jit::BackgroundPortIndex input, IndexCoverage const& requested)
{
    auto const& planned_input = port(input);
    IndexCoverage coverage;
    InputRequirementAccumulator* accumulator = nullptr;
    if (planned_input.accumulators.input_requirement) {
        auto const index = *planned_input.accumulators.input_requirement;
        accumulator = &input_requirements_[index];
        coverage = accumulator->coverage;
    } else {
        coverage = input_coverage(input);
    }
    auto required = requested & coverage;
    if (required.empty())
        return;
    input_selection_requests_by_port_[input].include(required);
    if (planned_input.input_access != InputAccessKind::sequential
        && planned_input.content_access == InputContentAccess::complete_buffer) {
        auto const& source = planning_state_ ? *planning_state_->root : *this;
        auto const regions = derive_input_regions(
            *plan_, source.candidate_output_region_ranges_, input, &required);
        IndexCoverage selected;
        for (auto const& region : regions) {
            if (required.intersects(region.range)) selected.include(region.range);
        }
        required = std::move(selected);
    }
    input_requirements_by_port_[input].include(required);

    for (auto const connection_index : planned_input.incoming_connections) {
        auto const& connection = plan_->connections[connection_index];
        auto require_source = [&](graph_jit::BackgroundPortIndex source,
                                  IndexCoverage source_required) {
            auto const& planned_source = port(source);
            auto const stored_boundary = planned_source.realization
                && *planned_source.realization == OutputRealization::buffered;
            bool cached = false;
            if (planning_state_) {
                auto const& expansion = planning_state_->group(this).expansion;
                cached = expansion.cached_output && expansion.cached_output(expansion.data, source);
            }
            require_output(
                source, source_required, !stored_boundary && !cached);
        };
        bool used_exact_routes = false;
        if (connection.kind == PortKind::sample
            && connection.sample_target_port == input
            && connection.sample_source_port_by_channel.size()
                == connection.sample_read_latencies.size()) {
            for (std::size_t channel = 0;
                 channel < connection.sample_source_port_by_channel.size();
                 ++channel) {
                auto const source =
                    connection.sample_source_port_by_channel[channel];
                if (!source) continue;
                require_source(
                    *source,
                    shifted_back(
                        required, connection.sample_read_latencies[channel]));
                used_exact_routes = true;
            }
        } else if (connection.kind == PortKind::event) {
            for (auto const& delivery : connection.event_deliveries) {
                if (delivery.target_port != input || !delivery.source_port)
                    continue;
                require_source(*delivery.source_port, required);
                used_exact_routes = true;
            }
        }
        if (!used_exact_routes) {
            for (auto const source : connection.source_coverage_ports) {
                require_source(source, required);
            }
        }
    }
}

void BackgroundPropagationWorkspace::require_sample_output(
    graph_jit::BackgroundPortIndex output, BackgroundSampleDemand const& requested)
{
    auto const& planned = port(output);
    auto const change_index = output_index(output);
    if (planning_state_ && planning_state_->root != this) {
        auto const& root = *planning_state_->root;
        candidate_output_coverages_[change_index] = root.candidate_output_coverages_[change_index];
        candidate_output_region_ranges_[change_index] = root.candidate_output_region_ranges_[change_index];
        candidate_output_coverage_known_[change_index] = root.candidate_output_coverage_known_[change_index];
    }
    auto selected = requested.intersected(candidate_output_coverages_[change_index]);
    if (selected.empty()) return;
    if (planned.output_production == OutputProductionKind::sequential) {
        auto const& expansion = planning_state_->group(this).expansion;
        auto const cached = expansion.cached_output && expansion.cached_output(expansion.data, output);
        require_output(output, selected.positional_ranges(expansion.sparse_sample_budget),
            !planned.persisted_tick_output && !cached);
        return;
    }

    auto& requirement = output_requirements_[*planned.accumulators.output_requirement];
    IndexCoverage dense;
    auto const positions = selected.view();
    for (auto const run : positions.runs)
        if (run.step == 1) dense.include({run.first, run.count});
    if (!dense.empty()) {
        auto const& expansion = planning_state_->group(this).expansion;
        auto const cached = expansion.cached_output && expansion.cached_output(expansion.data, output);
        require_output(output, dense, !cached && planned.realization != OutputRealization::buffered);
    }
    bool grew = false;
    for (auto const run : positions.runs) {
        if (run.step == 1) continue;
        grew |= requirement.positional_sample_positions.include(run);
        requirement.sample_positions.include(run);
    }
    for (auto const position : positions.individual_positions) {
        grew |= requirement.positional_sample_positions.include(position);
        requirement.sample_positions.include(position);
    }
    requirement.positions_view = requirement.sample_positions.view();
    if (grew) {
        requirement.sample_selections_dirty = true;
        activate_node(planned.node, graph_jit::BackgroundNodeActivity::reverse);
    }
}

void BackgroundPropagationWorkspace::require_sample_input(
    graph_jit::BackgroundPortIndex input, SamplePositionsView requested)
{
    auto const& planned = port(input);
    IndexCoverage dense;
    BackgroundSampleDemand sparse;
    for (auto const run : requested.runs) {
        if (run.step == 1) dense.include({run.first, run.count});
        else sparse.include(run);
    }
    for (auto const position : requested.individual_positions) sparse.include(position);
    if (!dense.empty()) require_input(input, dense);
    if (sparse.empty()) return;

    auto const available = planned.accumulators.input_requirement
        ? input_requirements_[*planned.accumulators.input_requirement].coverage : input_coverage(input);
    sparse = sparse.intersected(available);
    if (sparse.empty()) return;
    if (planned.content_access == InputContentAccess::complete_buffer) {
        auto const& source = *planning_state_->root;
        IndexCoverage complete;
        for (auto const& region : derive_input_regions(*plan_, source.candidate_output_region_ranges_, input))
            if (sparse.intersects(region.range)) complete.include(region.range);
        require_input(input, complete);
        return;
    }
    if (!input_sample_requirements_by_port_[input].include(sparse.view())) return;
    for (auto const connection_index : planned.incoming_connections) {
        auto const& connection = plan_->connections[connection_index];
        if (connection.sample_target_port == input
            && connection.sample_source_port_by_channel.size() == connection.sample_read_latencies.size()) {
            for (std::size_t channel = 0; channel < connection.sample_source_port_by_channel.size(); ++channel) {
                auto const source = connection.sample_source_port_by_channel[channel];
                if (source) require_sample_output(*source, sparse.retreated_by(connection.sample_read_latencies[channel]));
            }
        } else {
            for (auto const source : connection.source_coverage_ports) require_sample_output(source, sparse);
        }
    }
}

void BackgroundPropagationWorkspace::publish_input_requirement_callback(
    void* opaque, IndexCoverage const& required)
{
    auto& accumulator = *static_cast<InputRequirementAccumulator*>(opaque);
    before_metadata_read(&accumulator.owner->reverse_reads_[accumulator.port]);
    accumulator.owner->require_input(accumulator.port, required);
}

void BackgroundPropagationWorkspace::publish_input_sample_positions(
    void* opaque, SamplePositionsView requested)
{
    auto& accumulator = *static_cast<InputRequirementAccumulator*>(opaque);
    before_metadata_read(&accumulator.owner->reverse_reads_[accumulator.port]);
    accumulator.owner->require_sample_input(accumulator.port, requested);
}

namespace {
bool add_index_runs(std::vector<IndexedRegionDemand>& demands, graph_jit::BackgroundPortIndex port,
                    std::size_t region, std::span<IndexRun const> runs, std::uint64_t count)
{
    for (auto const run : runs)
        if (!run.fits(count)) throw std::invalid_argument("indices are outside the published Region");
    if (std::ranges::all_of(runs, [](IndexRun run) { return run.count == 0; })) return false;
    auto found = std::ranges::find(demands, region, &IndexedRegionDemand::region_index);
    if (found == demands.end()) {
        demands.push_back({port, region});
        found = std::prev(demands.end());
    }
    bool grew = false;
    for (auto const run : runs) {
        if (run.count == 0 || std::ranges::contains(found->runs, run)) continue;
        found->runs.push_back(run);
        grew = true;
    }
    return grew;
}
} // namespace

void BackgroundPropagationWorkspace::require_event_output(
    graph_jit::BackgroundPortIndex output, std::size_t region, std::span<IndexRun const> requested)
{
    auto const& planned = port(output);
    auto const& published = planning_state_->root->candidate_output_event_regions_[output_index(output)];
    if (planned.kind != PortKind::event || region >= published.size() || !published[region].index)
        throw std::invalid_argument("output Region does not support indexed events");
    auto& demands = indexed_output_demands_[output];
    if (!add_index_runs(demands, output, region, requested, published[region].index->event_count)) return;
    auto& requirement = output_requirements_[*planned.accumulators.output_requirement];
    requirement.selection_views.clear();
    for (auto const& demand : demands) {
        requirement.selection_views.push_back({
            details::RegionRefAccess::make(planning_state_->snapshot, output, demand.region_index),
            demand.runs});
    }
    requirement.selections = requirement.selection_views;
    activate_node(planned.node, graph_jit::BackgroundNodeActivity::reverse);
}

void BackgroundPropagationWorkspace::update_sample_selections(graph_jit::BackgroundPortIndex output)
{
    auto const& planned = port(output);
    if (planned.output_production == OutputProductionKind::sequential) return;
    auto& required = output_requirements_[*planned.accumulators.output_requirement];
    if (!required.sample_selections_dirty) return;
    required.sample_selections_dirty = false;
    auto const& regions = candidate_output_region_ranges_[output_index(output)];
    auto const& selected = indexed_output_demands_[output];
    required.sample_selections.clear();
    for (std::size_t index = 0; index < regions.size(); ++index) {
        auto const region = regions[index];
        IndexCoverage const positions{region};
        auto sparse = required.positional_sample_positions.intersected(positions);
        auto const dense = required.required & positions;
        auto const explicit_runs = std::ranges::find(selected, index, &IndexedRegionDemand::region_index);
        if (explicit_runs != selected.end()) {
            for (auto const run : explicit_runs->runs)
                sparse.include(SamplePositionRun{region.begin.advanced_by(run.first), run.count, run.step});
        }
        if (sparse.empty() && dense.empty()) continue;
        auto& selection = required.sample_selections.emplace_back(output, index);
        // The borrowed reverse view preserves overlaps. Only the storage
        // preparation step normalizes them to a unique destination per slot.
        auto const append = [&](SamplePositionRun run) {
            selection.runs.push_back({region.begin.forward_distance_to(run.first), run.count, run.step});
        };
        for (auto const part : dense) append({part.begin, part.extent});
        for (auto const run : sparse.view().runs) append(run);
        for (auto const position : sparse.view().individual_positions) append({position, 1});
    }
    required.selection_views.clear();
    for (auto const& demand : required.sample_selections) {
        required.selection_views.push_back({
            details::RegionRefAccess::make(planning_state_->snapshot, output, demand.region_index), demand.runs});
    }
    required.selections = required.selection_views;
}

void BackgroundPropagationWorkspace::require_sample_region(
    graph_jit::BackgroundPortIndex output, std::size_t region, std::span<IndexRun const> runs)
{
    auto const& planned = port(output);
    auto const change = output_index(output);
    auto const& root = *planning_state_->root;
    auto const& regions = root.candidate_output_region_ranges_[change];
    if (planned.output_production == OutputProductionKind::sequential || region >= regions.size())
        throw std::invalid_argument("indexed sample output requires a published authored Region");
    if (&root != this) {
        candidate_output_coverages_[change] = root.candidate_output_coverages_[change];
        candidate_output_region_ranges_[change] = regions;
        candidate_output_coverage_known_[change] = root.candidate_output_coverage_known_[change];
    }
    auto const range = regions[region];
    auto& demands = indexed_output_demands_[output];
    if (!add_index_runs(demands, output, region, runs, range.extent)) return;
    auto& requirement = output_requirements_[*planned.accumulators.output_requirement];
    for (auto const run : runs)
        if (run.count) requirement.sample_positions.include(SamplePositionRun{
            range.begin.advanced_by(run.first), run.count, run.step});
    requirement.positions_view = requirement.sample_positions.view();
    requirement.sample_selections_dirty = true;
    activate_node(planned.node, graph_jit::BackgroundNodeActivity::reverse);
}

void BackgroundPropagationWorkspace::require_region_selection(
    graph_jit::BackgroundPortIndex input, RegionSelection selection)
{
    using Ref = details::RegionRefAccess;
    if (Ref::port(selection.region) != input || Ref::snapshot(selection.region) != planning_state_->snapshot)
        throw std::invalid_argument("Region reference does not belong to this input and snapshot");
    auto const index = Ref::index(selection.region);
    auto const& root = *planning_state_->root;
    auto const resolved = derive_input_regions(*plan_, root.candidate_output_region_ranges_, input);
    if (index >= resolved.size())
        throw std::invalid_argument("input Region reference is outside the published list");
    if (port(input).kind == PortKind::sample) {
        auto const& region = resolved[index];
        for (auto const run : selection.runs)
            if (!run.fits(region.range.extent)) throw std::invalid_argument("indices are outside the published Region");
        if (std::ranges::all_of(selection.runs, [](IndexRun const& run) { return run.count == 0; })) return;
        IndexRun const complete{0, region.range.extent};
        if (port(input).content_access == InputContentAccess::complete_buffer)
            selection.runs = std::span{&complete, 1};
        if (!add_index_runs(indexed_input_demands_[input], input, index, selection.runs, region.range.extent))
            return;
        auto const& connection = plan_->connections[region.connection];
        for (auto const& source : region.sources) {
            auto const latency = source.route_index ? connection.sample_read_latencies[*source.route_index] : 0;
            auto const& planned = port(source.output);
            auto const& regions = root.candidate_output_region_ranges_[output_index(source.output)];
            if (!source.region_index || planned.output_production == OutputProductionKind::sequential) {
                BackgroundSampleDemand demand;
                for (auto const run : selection.runs) {
                    if (!run.count) continue;
                    demand.include(SamplePositionRun{region.range.begin.advanced_by(run.first).retreated_by(latency),
                        run.count, run.step});
                }
                require_sample_output(source.output, demand);
                continue;
            }
            auto const source_index = *source.region_index;
            auto const source_range = regions[source_index];
            auto const first = source_range.begin.forward_distance_to(region.range.begin.retreated_by(latency));
            std::vector<IndexRun> runs;
            for (auto run : selection.runs) {
                if (run.count) {
                    run.first += first;
                    runs.push_back(run);
                }
            }
            require_sample_region(source.output, source_index, runs);
        }
        return;
    }
    if (!preserves_event_indices(*plan_, resolved[index], input))
        throw std::invalid_argument("input route does not preserve event indices");
    auto const& source = resolved[index].sources.front();
    auto const& published = root.candidate_output_event_regions_[output_index(source.output)];
    if (!source.region_index || *source.region_index >= published.size()
        || published[*source.region_index].range != resolved[index].range
        || !published[*source.region_index].index)
        throw std::invalid_argument("input route does not preserve event indices");
    if (add_index_runs(indexed_input_demands_[input], input, index, selection.runs,
            published[*source.region_index].index->event_count)) {
        require_event_output(source.output, *source.region_index, selection.runs);
    }
}

void BackgroundPropagationWorkspace::publish_input_region_selection(void* opaque, RegionSelection selection)
{
    auto& accumulator = *static_cast<InputRequirementAccumulator*>(opaque);
    before_metadata_read(&accumulator.owner->reverse_reads_[accumulator.port]);
    accumulator.owner->require_region_selection(accumulator.port, selection);
}

void BackgroundPropagationWorkspace::publish_corresponding_sample_requirements(
    void* opaque, OutputCoverageRequirement const& output)
{
    auto const& input = *static_cast<InputRequirementAccumulator*>(opaque);
    auto const* source = static_cast<OutputChangeAccumulator const*>(output.selection_owner);
    auto& workspace = *input.owner;
    if (!source || source->owner != &workspace)
        throw std::logic_error("corresponding sample demand has no output binding in this workspace");
    before_metadata_read(&workspace.reverse_reads_[input.port]);
    workspace.require_input(input.port, output.required());
    auto const& plan = workspace.port(source->port);
    auto const& requirements = workspace.output_requirements_[*plan.accumulators.output_requirement];
    workspace.require_sample_input(input.port, requirements.positional_sample_positions.view());
    for (auto const& demand : workspace.indexed_output_demands_[source->port]) {
        workspace.require_region_selection(input.port, {
            details::RegionRefAccess::make(workspace.planning_state_->snapshot, input.port, demand.region_index),
            demand.runs,
        });
    }
}

namespace {
IndexCoverage extend_tick_coverage(IndexCoverage const& coverage,
    std::size_t before, std::size_t after)
{
    if (coverage.empty() || (before == 0 && after == 0)) return coverage;
    auto const maximum = std::numeric_limits<GlobalIndex::extent_type>::max();
    if (after > maximum - before) return graph_jit::full_index_coverage();
    auto const extra = before + after;
    IndexCoverage result;
    for (auto const range : coverage) {
        if (extra > maximum - range.extent)
            return graph_jit::full_index_coverage();
        result.include({range.begin.retreated_by(before), range.extent + extra});
    }
    return result;
}
} // namespace

IndexCoverage replay_callback_coverage(IndexCoverage const& output, std::size_t latency)
{
    return extend_tick_coverage(output, 0, latency);
}

IndexCoverage replay_input_coverage(IndexCoverage const& callbacks, std::size_t history)
{
    return extend_tick_coverage(callbacks, history, 0);
}

PreparedCoveragePropagation BackgroundPropagationWorkspace::prepare_replay_inputs(
    CompiledGraphBackgroundOperations const& operations,
    PreparedCoveragePropagation const& candidate,
    std::span<InputCoverageRequest const> requests,
    BackgroundCoverageDemandExpansion expansion,
    BackgroundCoverageDiscovery discovery)
{
    BackgroundCoverageState snapshot;
    snapshot.output_coverages_ = candidate.output_coverages_;
    snapshot.output_region_ranges_ = candidate.output_region_ranges_;
    snapshot.output_event_regions_ = candidate.output_event_regions_;
    snapshot.output_coverage_known_ = candidate.output_coverage_known_;
    CoveragePropagationRequest roots;
    roots.input_demands.assign(requests.begin(), requests.end());
    return prepare(operations, snapshot, roots, expansion, discovery,
        candidate.result().output_changes);
}

void BackgroundPropagationWorkspace::tick_forward_coverage(
    void*, ReflectedNodeForwardCoverageContext const& context)
{
    using Op = details::TickCoverageOp;
    auto const sample_outputs = std::span<OutputCoverageChange>(context.outputs);
    auto const event_outputs = std::span<OutputCoverageChange>(context.event_outputs);
    if (sample_outputs.empty() && event_outputs.empty()) {
        context.planning.finish();
        return;
    }
    auto const& first_output = sample_outputs.empty() ? event_outputs.front() : sample_outputs.front();
    auto const& first = *static_cast<OutputChangeAccumulator const*>(first_output.data);
    auto const& workspace = *first.owner;
    auto const& calls = workspace.node_call_data_[workspace.port(first.port).node];
    IndexCoverage input_changes;
    IndexCoverage input_history_changes;
    auto const replays_tick = workspace.port(first.port).replayed_tick_output;
    bool source_changed = false;
    auto gather_changes = [&](std::span<graph_jit::BackgroundPortIndex const> ports,
                              std::span<InputCoverageChange const> inputs) {
        for (std::size_t i = 0; i < ports.size(); ++i) {
            auto const& changed = inputs[i].changed();
            // A source coordinate need not overlap any affected output position.
            auto const& input = workspace.port(ports[i]);
            if (input.random_access_input) {
                source_changed |= !changed.empty();
            } else {
                input_changes.include(changed);
                if (replays_tick && input.sequential_history != 0)
                    input_history_changes.include(
                        extend_tick_coverage(changed, 0, input.sequential_history));
            }
        }
    };
    gather_changes(calls.sample_inputs, context.inputs);
    gather_changes(calls.event_inputs, context.event_inputs);
    auto publish = [&](OutputCoverageChange const& output) {
        auto const& accumulator = *static_cast<OutputChangeAccumulator const*>(output.data);
        auto const& port = workspace.port(accumulator.port);
        if (port.persisted_tick_output) return;
        auto read_input = [&](details::TickCoverageTerm const& term) -> IndexCoverage {
            auto const sample = term.kind == PortKind::sample;
            auto const& ports = sample ? calls.sample_inputs : calls.event_inputs;
            auto const found = std::ranges::find_if(ports, [&](auto input) {
                return workspace.port(input).configured_port.port_index == term.first;
            });
            if (found == ports.end()) throw std::logic_error("Tick coverage rule lost its input");
            if (workspace.port(*found).tick_coverage_has_manual_source)
                return graph_jit::full_index_coverage();
            // Gather input metadata before publishing any output.
            return workspace.input_changes_[workspace.input_index(*found)].coverage;
        };
        auto coverage = graph_jit::full_index_coverage();
        std::size_t tail = 0;
        if (!port.tick_coverage.empty()) {
            struct TermValue {
                IndexCoverage coverage;
                std::size_t tail = 0;
            };
            std::vector<TermValue> values;
            values.reserve(port.tick_coverage.size());
            for (auto const& term : port.tick_coverage) {
                switch (term.op) {
                case Op::input: values.push_back({read_input(term)}); break;
                case Op::union_:
                case Op::intersection: {
                    auto const& lhs = values[term.first];
                    auto const& rhs = values[term.second];
                    values.push_back({term.op == Op::union_ ? lhs.coverage | rhs.coverage
                        : lhs.coverage & rhs.coverage, std::max(lhs.tail, rhs.tail)});
                    break;
                }
                case Op::extend_right: {
                    auto const& operand = values[term.first];
                    auto const maximum = std::numeric_limits<std::size_t>::max();
                    values.push_back({extend_tick_coverage(operand.coverage, 0, term.second),
                        operand.tail + std::min(term.second, maximum - operand.tail)});
                    break;
                }
                }
            }
            coverage = std::move(values.back().coverage);
            tail = values.back().tail;
        }
        auto const latency = port.replayed_tick_output ? port.output_latency : 0;
        auto changed = extend_tick_coverage(input_changes, latency, tail);
        changed.include(extend_tick_coverage(input_history_changes, latency, 0));
        if (context.local_state_changed || source_changed) {
            changed.include(coverage | output.previous_coverage());
        }
        output.publish_coverage(coverage);
        output.change(changed);
    };
    for (auto const& output : sample_outputs) publish(output);
    for (auto const& output : event_outputs) publish(output);
    context.planning.finish();
}

void BackgroundPropagationWorkspace::replay_reverse_coverage(
    void*, ReflectedNodeReverseCoverageContext const& context)
{
    IndexCoverage required;
    auto require_output_callbacks = [&](OutputCoverageRequirement const& output) {
        auto const& change = *static_cast<OutputChangeAccumulator const*>(output.selection_owner);
        required.include(replay_callback_coverage(output.required(),
            change.owner->port(change.port).output_latency));
    };
    for (auto const& output :
         static_cast<std::span<OutputCoverageRequirement const>>(
             context.outputs)) {
        require_output_callbacks(output);
    }
    for (auto const& output :
         static_cast<std::span<OutputCoverageRequirement const>>(
             context.event_outputs)) {
        require_output_callbacks(output);
    }
    if (!required.empty()) {
        auto require_input = [&](InputCoverageRequirement const& input) {
            auto const& accumulator = *static_cast<InputRequirementAccumulator const*>(input.data);
            auto const& planned = accumulator.owner->port(accumulator.port);
            if (planned.deferred_tick_lead != 0) return;
            auto const& covered = input.coverage();
            input.require(planned.random_access_input ? covered
                : replay_input_coverage(required, planned.sequential_history) & covered);
        };
        for (auto const& input : std::span<InputCoverageRequirement>(context.inputs)) require_input(input);
        for (auto const& input : std::span<InputCoverageRequirement>(context.event_inputs)) require_input(input);
    }
    context.planning.finish();
}

void BackgroundPropagationWorkspace::initialize_calls()
{
    for (graph_jit::BackgroundPortIndex index = 0; index < plan_->ports.size();
         ++index) {
        auto const& planned = plan_->ports[index];
        if (planned.direction == graph_jit::PortDirection::input &&
            planned.accumulators.input_change) {
            auto const change_index = *planned.accumulators.input_change;
            auto const requirement_index =
                *planned.accumulators.input_requirement;
            auto& requirement = input_requirements_[requirement_index];
            requirement.owner = this;
            requirement.port = index;
            forward_reads_[index] = {.owner = this, .input = index};
            reverse_reads_[index] = {.owner = this, .input = index, .reverse = true};
            callback_input_changes_[change_index] = InputCoverageChange{
                .region_ranges_view_value =
                    &input_changes_[change_index].region_view,
                .coverage_value = &input_changes_[change_index].coverage,
                .changed_value = &input_changes_[change_index].changed,
                .read_owner = &forward_reads_[index],
                .before_read = &before_metadata_read,
            };
            callback_input_requirements_[requirement_index] =
                InputCoverageRequirement{
                    .data = &requirement,
                    .coverage_value = &requirement.coverage,
                    .publish_required_value =
                        &publish_input_requirement_callback,
                    .read_owner = &reverse_reads_[index],
                    .before_read = &before_metadata_read,
                    .publish_sample_positions = &publish_input_sample_positions,
                    .publish_region_selection = &publish_input_region_selection,
                    .publish_corresponding_sample_requirements = &publish_corresponding_sample_requirements,
            };
        } else if (planned.direction == graph_jit::PortDirection::output) {
            auto const change_index = *planned.accumulators.output_change;
            auto const requirement_index =
                *planned.accumulators.output_requirement;
            auto& change = output_changes_[change_index];
            change.owner = this;
            change.port = index;
            callback_output_changes_[change_index] = OutputCoverageChange{
                .data = &change,
                .previous_region_ranges_view_value =
                    &change.previous_region_view,
                .previous_coverage_value = &change.previous_coverage,
                .publish_regions_value = &publish_output_regions_callback,
                .publish_coverage_value = &publish_output_coverage_callback,
                .publish_changed_value = &publish_output_change_callback,
                    .publish_event_regions_value = &publish_output_event_regions,
            };
            callback_output_requirements_[requirement_index] =
                OutputCoverageRequirement{
                    .required_value =
                        &output_requirements_[requirement_index].required,
                    .required_sample_positions = &output_requirements_[requirement_index].positions_view,
                    .required_region_selections = &output_requirements_[requirement_index].selections,
                    .selection_owner = &change,
                    .sample_selections = +[](void* opaque) -> std::span<RegionSelection const> {
                        auto const& output = *static_cast<OutputChangeAccumulator const*>(opaque);
                        auto& workspace = *output.owner;
                        workspace.update_sample_selections(output.port);
                        auto const index = *workspace.port(output.port).accumulators.output_requirement;
                        return workspace.output_requirements_[index].selections;
                    },
            };
        }
    }
    for (graph_jit::BackgroundNodeIndex index = 0; index < plan_->nodes.size();
         ++index) {
        auto const& node = plan_->nodes[index];

        auto& calls = node_call_data_[index];
        for (auto const port_index : node.inputs) {
            auto const& planned = port(port_index);
            auto const callback_input =
                node.authored_tock_execution ? planned.random_access_input
                : (node.replays_tick || node.tracks_live_tick_coverage)
                    ? planned.accumulators.input_change.has_value() : false;
            if (!callback_input)
                continue;
            auto const change_index = *planned.accumulators.input_change;
            auto const requirement_index =
                *planned.accumulators.input_requirement;
            if (planned.kind == PortKind::sample) {
                calls.sample_inputs.push_back(port_index);
                calls.sample_input_changes.push_back(
                    callback_input_changes_[change_index]);
                calls.sample_input_requirements.push_back(
                    callback_input_requirements_[requirement_index]);
            } else {
                calls.event_inputs.push_back(port_index);
                calls.event_input_changes.push_back(
                    callback_input_changes_[change_index]);
                calls.event_input_requirements.push_back(
                    callback_input_requirements_[requirement_index]);
            }
        }

        for (auto const port_index : node.outputs) {
            auto const& planned = port(port_index);
            auto const callback_output =
                node.authored_tock_execution ? planned.authored_tock_output
                : (node.replays_tick || node.tracks_live_tick_coverage)
                    ? planned.accumulators.output_change.has_value() : false;
            if (!callback_output)
                continue;
            auto const change_index = *planned.accumulators.output_change;
            auto const requirement_index =
                *planned.accumulators.output_requirement;
            if (planned.kind == PortKind::sample) {
                calls.sample_output_changes.push_back(
                    callback_output_changes_[change_index]);
                calls.sample_output_requirements.push_back(
                    callback_output_requirements_[requirement_index]);
            } else {
                calls.event_output_changes.push_back(
                    callback_output_changes_[change_index]);
                calls.event_output_requirements.push_back(
                    callback_output_requirements_[requirement_index]);
            }
        }

        auto& frame = node_calls_[index];
        frame.replay_context = this;
        frame.forward = ReflectedNodeForwardCoverageContext{
            .inputs = calls.sample_input_changes,
            .outputs = calls.sample_output_changes,
            .event_inputs = calls.event_input_changes,
            .event_outputs = calls.event_output_changes,
            .sample_rate = sample_rate_,
        };
        frame.reverse = ReflectedNodeReverseCoverageContext{
            .inputs = calls.sample_input_requirements,
            .outputs = calls.sample_output_requirements,
            .event_inputs = calls.event_input_requirements,
            .event_outputs = calls.event_output_requirements,
            .sample_rate = sample_rate_,
        };
        if (node.uses_tick_forward_coverage) {
            frame.replay_forward = &tick_forward_coverage;
        }
        if (node.uses_replay_reverse_coverage) {
            frame.replay_reverse = &replay_reverse_coverage;
        }
    }
    call_.nodes = node_calls_;
}

BackgroundCoverageState::BackgroundCoverageState(std::size_t output_count)
    : output_coverages_(output_count)
    , output_region_ranges_(output_count)
    , output_event_regions_(output_count)
    , output_coverage_known_(output_count, false)
{
}

BackgroundCoverageState::Migration
BackgroundCoverageState::prepare_migration_from(
    BackgroundCoverageState& previous,
    graph_jit::BackgroundEvaluationPlan const& current_plan,
    graph_jit::BackgroundEvaluationPlan const& previous_plan,
    bool local_semantic_environment_changed)
{
    if (output_coverages_.size()
            != current_plan.accumulators.output_change_count
        || output_region_ranges_.size()
            != current_plan.accumulators.output_change_count
        || output_coverage_known_.size()
            != current_plan.accumulators.output_change_count
        || previous.output_coverages_.size()
            != previous_plan.accumulators.output_change_count
        || previous.output_region_ranges_.size()
            != previous_plan.accumulators.output_change_count
        || previous.output_coverage_known_.size()
            != previous_plan.accumulators.output_change_count) {
        throw std::invalid_argument(
            "background coverage state does not match its compiled plan");
    }

    auto is_output = [](graph_jit::BackgroundPortPlan const& port) {
        return port.direction == graph_jit::PortDirection::output
            && background_output_identity(port)
            && port.accumulators.output_change;
    };
    auto compatible = [](graph_jit::BackgroundPortPlan const& current,
                         graph_jit::BackgroundPortPlan const& prior) {
        if (current.kind != prior.kind) return false;
        return current.kind == PortKind::sample
            ? current.sample_layout == prior.sample_layout
                && current.continuous_geometry == prior.continuous_geometry
                && current.output_production == prior.output_production
            : current.event_type == prior.event_type
                && current.output_production == prior.output_production;
    };

    Migration migration;
    migration.current = this;
    migration.previous = &previous;
    migration.current_plan = &current_plan;
    migration.previous_plan = &previous_plan;

    std::map<BackgroundOutputIdentity, graph_jit::BackgroundPortIndex>
        current_outputs;
    std::map<BackgroundOutputIdentity, graph_jit::BackgroundPortIndex>
        previous_outputs;
    for (graph_jit::BackgroundPortIndex index = 0;
         index < current_plan.ports.size(); ++index) {
        auto const& port = current_plan.ports[index];
        if (is_output(port)
            && !current_outputs.emplace(*background_output_identity(port), index).second) {
            throw std::invalid_argument(
                "current background plan has duplicate stable output identity");
        }
    }
    for (graph_jit::BackgroundPortIndex index = 0;
         index < previous_plan.ports.size(); ++index) {
        auto const& port = previous_plan.ports[index];
        if (is_output(port)
            && !previous_outputs.emplace(*background_output_identity(port), index).second) {
            throw std::invalid_argument(
                "previous background plan has duplicate stable output identity");
        }
    }
    for (std::size_t current_index = 0;
         current_index < current_plan.ports.size(); ++current_index) {
        auto const& current = current_plan.ports[current_index];
        if (!is_output(current)) continue;
        auto const found = previous_outputs.find(*background_output_identity(current));
        if (found == previous_outputs.end()) continue;
        auto const& matched = previous_plan.ports[found->second];
        if (!compatible(current, matched)) continue;
        if (*current.accumulators.output_change >= output_coverages_.size()
            || *matched.accumulators.output_change
                >= previous.output_coverages_.size()) {
            throw std::invalid_argument(
                "background stable output has an invalid coverage accumulator");
        }
        migration.outputs.push_back({
            .current = *current.accumulators.output_change,
            .previous = *matched.accumulators.output_change,
        });
    }

    std::map<BackgroundNodeIdentity, graph_jit::BackgroundNodeIndex>
        current_nodes;
    std::map<BackgroundNodeIdentity, graph_jit::BackgroundNodeIndex>
        previous_nodes;
    for (graph_jit::BackgroundNodeIndex index = 0;
         index < current_plan.nodes.size(); ++index) {
        auto const& node = current_plan.nodes[index];
        if (auto identity = background_node_identity(node); identity
            && !current_nodes.emplace(*identity, index).second) {
            throw std::invalid_argument(
                "current background plan has duplicate stable node identity");
        }
    }
    for (graph_jit::BackgroundNodeIndex index = 0;
         index < previous_plan.nodes.size(); ++index) {
        auto const& node = previous_plan.nodes[index];
        if (auto identity = background_node_identity(node); identity
            && !previous_nodes.emplace(*identity, index).second) {
            throw std::invalid_argument(
                "previous background plan has duplicate stable node identity");
        }
    }
    for (std::size_t current_index = 0;
         current_index < current_plan.nodes.size(); ++current_index) {
        auto const& current = current_plan.nodes[current_index];
        graph_jit::BackgroundNodePlan const* matched = nullptr;
        if (auto identity = background_node_identity(current)) {
            auto const found = previous_nodes.find(*identity);
            if (found != previous_nodes.end()) {
                matched = &previous_plan.nodes[found->second];
            }
        }
        auto const unchanged = matched
            && node_local_semantics_equal(
                current_plan,
                current,
                previous_plan,
                *matched,
                local_semantic_environment_changed);
        if (!unchanged) {
            migration.reconciliation_roots.locally_changed_nodes.push_back(
                current_index);
        } else if (current.authored_forward_coverage || current.uses_tick_forward_coverage) {
            // Commit checks which retained outputs still need discovery.
            migration.reconciliation_roots.forward_nodes.push_back(current_index);
        }
    }

    std::map<graph_jit::StableInputPortId, graph_jit::BackgroundPortIndex>
        current_inputs;
    std::map<graph_jit::StableInputPortId, graph_jit::BackgroundPortIndex>
        previous_inputs;
    auto index_inputs = [](auto const& plan, auto& indexed, char const* error) {
        for (graph_jit::BackgroundPortIndex index = 0;
             index < plan.ports.size(); ++index) {
            auto const& port = plan.ports[index];
            if (port.direction == graph_jit::PortDirection::input
                && port.stable_input_identity
                && !indexed.emplace(
                    *port.stable_input_identity, index).second) {
                throw std::invalid_argument(error);
            }
        }
    };
    index_inputs(
        current_plan,
        current_inputs,
        "current background plan has duplicate stable input identity");
    index_inputs(
        previous_plan,
        previous_inputs,
        "previous background plan has duplicate stable input identity");
    for (graph_jit::BackgroundPortIndex current_index = 0;
         current_index < current_plan.ports.size(); ++current_index) {
        auto const& current = current_plan.ports[current_index];
        if (current.direction != graph_jit::PortDirection::input
            || !current.stable_input_identity) {
            continue;
        }
        auto const found = previous_inputs.find(*current.stable_input_identity);
        if (found == previous_inputs.end()) continue;
        auto const& previous_input = previous_plan.ports[found->second];
        auto const tracks_changes = current.accumulators.input_change && previous_input.accumulators.input_change;
        auto const transfers_requests = current.deferred_tick_lead != 0
            && current.deferred_tick_lead == previous_input.deferred_tick_lead
            && !std::ranges::contains(migration.reconciliation_roots.locally_changed_nodes, current.node);
        if (!tracks_changes && !transfers_requests) continue;
        auto const same_connections = input_connections_equal(
            current_plan, current_index, previous_plan, found->second);
        if (tracks_changes && !same_connections) {
            migration.changed_inputs.push_back({
                .current = current_index,
                .previous = found->second,
            });
        }
        if (transfers_requests && same_connections) {
            migration.deferred_input_successors.resize(previous_plan.ports.size());
            migration.deferred_input_successors[found->second] = current_index;
        }
    }
    return migration;
}

void BackgroundCoverageState::Migration::commit()
{
    if (committed || !current || !previous) {
        throw std::logic_error(
            "invalid or already committed background coverage migration");
    }
    for (auto const& output : outputs) {
        current->output_coverages_[output.current] =
            previous->output_coverages_[output.previous];
        current->output_region_ranges_[output.current] =
            previous->output_region_ranges_[output.previous];
        current->output_event_regions_[output.current] =
            previous->output_event_regions_[output.previous];
        current->output_coverage_known_[output.current] =
            previous->output_coverage_known_[output.previous];
    }
    // The predecessor may finish an output between preparation and the cutover.
    std::erase_if(reconciliation_roots.forward_nodes, [&](auto node) {
        return std::ranges::none_of(current_plan->nodes[node].outputs, [&](auto output) {
            auto const index = current_plan->ports[output].accumulators.output_change;
            return index && !current->output_coverage_known_[*index];
        });
    });
    for (auto const& input : changed_inputs) {
        auto current_coverage = derive_input_coverage(
            *current_plan, current->output_coverages_, input.current);
        auto previous_coverage = derive_input_coverage(
            *previous_plan, previous->output_coverages_, input.previous);
        auto changed = current_coverage | previous_coverage;
        auto regions = derive_input_regions(
            *current_plan, current->output_region_ranges_, input.current);
        reconciliation_roots.input_changes.push_back({
            .port = input.current,
            .regions = input_region_ranges(regions),
            .coverage = std::move(current_coverage),
            .changed = std::move(changed),
        });
    }
    current->read_dependencies_.clear();
    for (auto dependency : previous->read_dependencies_) {
        if (dependency.input >= previous_plan->ports.size()) continue;
        auto const& old_input = previous_plan->ports[dependency.input];
        if (!old_input.stable_input_identity) continue;
        auto const found = std::ranges::find_if(current_plan->ports, [&](auto const& input) {
            return input.direction == graph_jit::PortDirection::input
                && input.stable_input_identity == old_input.stable_input_identity;
        });
        if (found == current_plan->ports.end()) continue;
        dependency.input = static_cast<graph_jit::BackgroundPortIndex>(found - current_plan->ports.begin());
        dependency.node = found->node;
        current->read_dependencies_.push_back(std::move(dependency));
    }
    committed = true;
}

CoveragePropagationRequest
BackgroundCoverageState::Migration::take_reconciliation_roots()
{
    if (!committed) {
        throw std::logic_error(
            "background coverage reconciliation roots are not committed");
    }
    return std::move(reconciliation_roots);
}

void BackgroundPropagationWorkspace::reset(
    BackgroundCoverageState const& coverage)
{
    std::fill(output_ready_.begin(), output_ready_.end(), true);
    std::fill(published_outputs_.begin(), published_outputs_.end(), false);
    std::fill(finalized_outputs_.begin(), finalized_outputs_.end(), false);
    candidate_output_coverages_ = coverage.output_coverages_;
    candidate_output_region_ranges_ = coverage.output_region_ranges_;
    candidate_output_event_regions_ = coverage.output_event_regions_;
    candidate_output_coverage_known_ = coverage.output_coverage_known_;
    for (std::size_t index = 0; index < candidate_output_coverages_.size();
         ++index) {
        output_changes_[index].previous_coverage =
            coverage.output_coverages_[index];
        output_changes_[index].previous_regions =
            coverage.output_region_ranges_[index];
        output_changes_[index].previous_region_view =
            output_changes_[index].previous_regions;
    }
    for (auto& value : input_changes_)
        value = {};
    for (auto& value : output_changes_) {
        value.changed = {};
        value.touched = false;
    }
    for (auto& value : input_requirements_) {
        value.coverage = {};
    }
    for (auto& value : output_requirements_)
        value = {};
    for (auto& value : input_requirements_by_port_)
        value = {};
    for (auto& value : input_sample_requirements_by_port_)
        value = {};
    for (auto& demands : indexed_input_demands_) demands.clear();
    for (auto& demands : indexed_output_demands_) demands.clear();
    for (auto& value : input_selection_requests_by_port_)
        value = {};
    for (auto& frame : node_calls_) {
        frame.activity = graph_jit::BackgroundNodeActivity::none;
        frame.forward.local_state_changed = false;
    }

    for (graph_jit::BackgroundPortIndex index = 0; index < plan_->ports.size();
         ++index) {
        auto const& planned = plan_->ports[index];
        if (planned.direction != graph_jit::PortDirection::input ||
            !planned.accumulators.input_change) {
            continue;
        }
        auto const change_index = input_index(index);
        auto const requirement_index = *planned.accumulators.input_requirement;
        auto coverage = input_coverage(index);
        auto regions = input_regions(index);
        input_changes_[change_index].coverage = coverage;
        input_changes_[change_index].regions = std::move(regions);
        input_changes_[change_index].region_view =
            input_changes_[change_index].regions;
        input_requirements_[requirement_index].coverage = std::move(coverage);
    }
}

BackgroundPropagationWorkspace::BackgroundPropagationWorkspace(
    graph_jit::BackgroundEvaluationPlan const& plan, std::size_t sample_rate)
    : plan_(&plan), sample_rate_(sample_rate),
      candidate_output_coverages_(plan.accumulators.output_change_count),
      candidate_output_region_ranges_(plan.accumulators.output_change_count),
      candidate_output_event_regions_(plan.accumulators.output_change_count),
      candidate_output_coverage_known_(plan.accumulators.output_change_count),
      input_changes_(plan.accumulators.input_change_count),
      output_changes_(plan.accumulators.output_change_count),
      input_requirements_(plan.accumulators.input_requirement_count),
      output_requirements_(plan.accumulators.output_requirement_count),
      input_requirements_by_port_(plan.ports.size()),
      input_sample_requirements_by_port_(plan.ports.size()),
      indexed_output_demands_(plan.ports.size()),
      indexed_input_demands_(plan.ports.size()),
      input_selection_requests_by_port_(plan.ports.size()),
      callback_input_changes_(plan.accumulators.input_change_count),
      callback_output_changes_(plan.accumulators.output_change_count),
      callback_input_requirements_(plan.accumulators.input_requirement_count),
      callback_output_requirements_(plan.accumulators.output_requirement_count),
      node_call_data_(plan.nodes.size()), node_calls_(plan.nodes.size()),
      forward_reads_(plan.ports.size()), reverse_reads_(plan.ports.size()),
      output_ready_(plan.ports.size(), true), published_outputs_(plan.ports.size(), false),
      finalized_outputs_(plan.ports.size(), false)
{
    initialize_calls();
}

[[nodiscard]] PreparedCoveragePropagation
BackgroundPropagationWorkspace::prepare(
    CompiledGraphBackgroundOperations const& operations,
    BackgroundCoverageState const& coverage,
    CoveragePropagationRequest const& request,
    BackgroundCoverageDemandExpansion expansion,
    BackgroundCoverageDiscovery discovery,
    std::span<PropagatedOutputChange const> selected_changes)
{
    auto const check_port = [this](graph_jit::BackgroundPortIndex index,
                                   graph_jit::PortDirection direction) {
        if (index >= plan_->ports.size()) {
            throw std::out_of_range(
                "Background coverage request port index is out of range");
        }
        if (port(index).direction != direction) {
            throw std::invalid_argument(
                "Background coverage request has the wrong port direction");
        }
    };
    for (auto const node : request.forward_nodes) {
        if (node >= node_calls_.size())
            throw std::out_of_range("forward refresh node index is out of range");
    }
    for (auto const node : request.locally_changed_nodes) {
        if (node >= node_calls_.size()) {
            throw std::out_of_range(
                "Background coverage request node index is out of range");
        }
    }
    for (auto const& change : request.input_changes) {
        check_port(change.port, graph_jit::PortDirection::input);
    }
    for (auto const& change : request.output_changes) {
        check_port(change.port, graph_jit::PortDirection::output);
    }
    for (auto const& demand : request.input_demands) {
        check_port(demand.port, graph_jit::PortDirection::input);
    }
    for (auto const& demand : request.output_demands) {
        check_port(demand.port, graph_jit::PortDirection::output);
    }
    PlanningState owned;
    auto const root_scope = planning_state_ == nullptr;
    if (root_scope) {
        static std::atomic<std::uint64_t> next_snapshot{1};
        owned.snapshot = next_snapshot.fetch_add(1, std::memory_order_relaxed);
        owned.root = this;
        owned.operations = &operations;
        owned.expansion = expansion;
        owned.discovery = discovery;
        planning_state_ = &owned;
    }
    struct Retire {
        BackgroundPropagationWorkspace& workspace;
        ~Retire() {
            workspace.planning_state_->scheduler.retire(&workspace);
            for (auto& group : workspace.planning_state_->groups) {
                if (group->workspace == &workspace) group->workspace = nullptr;
            }
            workspace.planning_state_->group_by_workspace.erase(&workspace);
            workspace.planning_state_ = nullptr;
        }
    } retire{*this};
    initialize_planning();
    planning_state_->group(this).expansion = expansion;
    reset(coverage);
    // The containing transaction already settled forward metadata. Its
    // pending output changes still exclude stale retained values.
    for (auto const& change : selected_changes) {
        auto& output = output_changes_[output_index(change.port)];
        output.changed = change.changed;
        output.touched = !change.changed.empty();
    }
    for (auto const node : request.forward_nodes)
        activate_node(node, graph_jit::BackgroundNodeActivity::forward);
    for (auto const node : request.locally_changed_nodes) {
        node_calls_[node].forward.local_state_changed = true;
        activate_node(node, graph_jit::BackgroundNodeActivity::forward);
    }
    for (auto const& root : request.output_changes) {
        if (port(root.port).output_production == OutputProductionKind::sequential)
            publish_output_coverage(root.port, root.coverage);
        else {
            if (region_coverage(root.regions) != root.coverage)
                throw std::invalid_argument("output root Regions differ from their coverage");
            publish_output_regions(root.port, root.regions);
        }
        publish_output_change(root.port, root.changed);
        if (root.finalized) finalized_outputs_[root.port] = true;
    }
    for (auto const& root : request.input_changes) {
        set_input_change(root.port, root.regions, root.coverage, root.changed);
    }

    if (root_scope) {
        std::vector<bool> dirty(node_calls_.size(), false);
        std::vector<graph_jit::BackgroundNodeIndex> pending;
        for (graph_jit::BackgroundNodeIndex node = 0; node < node_calls_.size(); ++node) {
            if (graph_jit::has_activity(node_calls_[node].activity,
                    graph_jit::BackgroundNodeActivity::forward)) {
                dirty[node] = true;
                pending.push_back(node);
            }
        }
        for (std::size_t next = 0; next < pending.size(); ++next) {
            for (auto const output : plan_->nodes[pending[next]].outputs) {
                for (auto const connection : port(output).outgoing_connections) {
                    for (auto const input : plan_->connections[connection].target_coverage_ports) {
                        auto const target = port(input).node;
                        if (!dirty[target]) { dirty[target] = true; pending.push_back(target); }
                    }
                }
            }
        }
        for (auto node : pending) mark_forward_pending(node);
    }
    run_planning(false);

    if (!root_scope) {
        auto const& root = *planning_state_->root;
        candidate_output_coverages_ = root.candidate_output_coverages_;
        candidate_output_region_ranges_ = root.candidate_output_region_ranges_;
        candidate_output_event_regions_ = root.candidate_output_event_regions_;
        candidate_output_coverage_known_ = root.candidate_output_coverage_known_;
        for (std::size_t index = 0; index < output_changes_.size(); ++index) {
            output_changes_[index].changed = root.output_changes_[index].changed;
            output_changes_[index].touched = root.output_changes_[index].touched;
        }
        for (auto const& planned : plan_->ports) {
            if (planned.direction == graph_jit::PortDirection::input && planned.accumulators.input_change) {
                refresh_input(static_cast<graph_jit::BackgroundPortIndex>(&planned - plan_->ports.data()));
            }
        }
    }
    BackgroundForwardOutputChanges const forward{
        .data = this,
        .changed_value = +[](void const* opaque,
                             graph_jit::BackgroundPortIndex output)
            -> IndexCoverage const& {
            auto const& workspace =
                *static_cast<BackgroundPropagationWorkspace const*>(opaque);
            return workspace.output_changes_[
                workspace.output_index(output)].changed;
        },
    };
    CoveragePropagationRequest selected;
    if (expansion.select_demands) {
        selected = request;
        BackgroundDemandSelection const candidate{
            .data = this,
            .output_regions = +[](void const* data, graph_jit::BackgroundPortIndex port) -> std::span<GlobalIndexRange const> {
                auto const& workspace = *static_cast<BackgroundPropagationWorkspace const*>(data);
                return workspace.candidate_output_region_ranges_[workspace.output_index(port)];
            },
            .event_regions = +[](void const* data, graph_jit::BackgroundPortIndex port) -> std::span<EventRegionInfo const> {
                auto const& workspace = *static_cast<BackgroundPropagationWorkspace const*>(data);
                return workspace.candidate_output_event_regions_[workspace.output_index(port)];
            },
            .read_events = +[](void* data, std::span<IndexedRegionDemand const> demands) {
                auto& workspace = *static_cast<BackgroundPropagationWorkspace*>(data);
                auto const& discovery = workspace.planning_state_->discovery;
                if (!discovery.read_events) throw std::logic_error("indexed event read requires a background transaction");
                CoveragePropagationRequest probe;
                probe.indexed_output_demands.assign(demands.begin(), demands.end());
                return discovery.read_events(discovery.data, workspace.prepare_discovery(probe), demands);
            },
        };
        expansion.select_demands(expansion.data, candidate, selected);
    }
    auto const& demands = expansion.select_demands ? selected : request;
    for (auto const& root : demands.input_demands) {
        if (!root.sample_positions.empty()) {
            if (port(root.port).kind != PortKind::sample)
                throw std::invalid_argument("sample position demand requires a continuous input");
            require_sample_input(root.port, root.sample_positions.view());
        }
        if (!expansion.restrict_input) {
            require_input(root.port, root.required);
            continue;
        }
        auto required = root.required;
        expansion.restrict_input(expansion.data, root.port, forward, required);
        require_input(root.port, required);
    }
    for (auto const& root : demands.output_demands) {
        auto const& planned = port(root.port);
        if (!root.sample_positions.empty()) {
            if (planned.kind != PortKind::sample)
                throw std::invalid_argument("sample position demand requires a continuous output");
            require_sample_output(root.port, root.sample_positions);
        }
        auto const stored_boundary = planned.realization
            && *planned.realization == OutputRealization::buffered;
        auto const cached = expansion.cached_output && expansion.cached_output(expansion.data, root.port);
        require_output(root.port, root.required, !stored_boundary && !cached);
    }
    for (auto const& root : demands.indexed_output_demands) {
        if (root.port >= plan_->ports.size() || port(root.port).direction != graph_jit::PortDirection::output)
            throw std::invalid_argument("indexed event demand requires an output port");
        if (port(root.port).kind == PortKind::event)
            require_event_output(root.port, root.region_index, root.runs);
        else
            require_sample_region(root.port, root.region_index, root.runs);
    }
    for (auto const& root : demands.indexed_input_demands) {
        if (root.port >= plan_->ports.size() || port(root.port).direction != graph_jit::PortDirection::input)
            throw std::invalid_argument("indexed event demand requires an input port");
        require_region_selection(root.port, {
            details::RegionRefAccess::make(planning_state_->snapshot, root.port, root.region_index), root.runs});
    }
    if (expansion.expand_output) {
        for (graph_jit::BackgroundPortIndex index = 0;
             index < plan_->ports.size(); ++index) {
            auto const& planned = port(index);
            if (planned.direction != graph_jit::PortDirection::output)
                continue;
            auto const change_index = output_index(index);
            IndexCoverage additional;
            expansion.expand_output(
                expansion.data, index,
                candidate_output_coverages_[change_index],
                candidate_output_region_ranges_[change_index],
                output_changes_[change_index].changed,
                output_requirements_[
                    *planned.accumulators.output_requirement].required,
                additional);
            require_output(index, additional, true);
        }
    }
    run_planning(true);

    CoveragePropagationResult result;
    for (auto& demands : indexed_output_demands_)
        result.indexed_outputs.insert(result.indexed_outputs.end(), demands.begin(), demands.end());
    for (auto& demands : indexed_input_demands_)
        result.indexed_inputs.insert(result.indexed_inputs.end(), demands.begin(), demands.end());
    result.output_changes.reserve(output_changes_.size());
    for (std::size_t index = 0; index < output_changes_.size(); ++index) {
        auto const& change = output_changes_[index];
        if (!change.touched)
            continue;
        result.output_changes.push_back(PropagatedOutputChange{
            .port = change.port,
            .coverage = candidate_output_coverages_[index],
            .changed = change.changed,
        });
    }
    result.input_requirements.reserve(plan_->ports.size());
    for (graph_jit::BackgroundPortIndex index = 0; index < plan_->ports.size();
         ++index) {
        auto const& required = input_requirements_by_port_[index];
        auto const& sample_positions = input_sample_requirements_by_port_[index];
        if (required.empty() && sample_positions.empty())
            continue;
        result.input_requirements.push_back(RequiredInputCoverage{
            .port = index,
            .required = required,
            .sample_positions = sample_positions,
        });
    }
    result.output_requirements.reserve(output_requirements_.size());
    for (graph_jit::BackgroundPortIndex index = 0; index < plan_->ports.size();
         ++index) {
        auto const& planned = plan_->ports[index];
        if (planned.direction != graph_jit::PortDirection::output ||
            !planned.accumulators.output_requirement) {
            continue;
        }
        auto const requirement_index = *planned.accumulators.output_requirement;
        auto const& demand = output_requirements_[requirement_index];
        if (demand.required.empty() && demand.positional_sample_positions.empty())
            continue;
        result.output_requirements.push_back(RequiredOutputCoverage{
            .port = index,
            .required = demand.required,
            .sample_positions = demand.positional_sample_positions,
        });
    }

    PreparedCoveragePropagation prepared;
    prepared.read_dependencies_ = coverage.read_dependencies_;
    for (auto const& group : planning_state_->groups) {
        for (auto const* operation : group->forward) {
            if (operation->queued) std::erase_if(prepared.read_dependencies_, [&](auto const& dependency) {
                return dependency.node == operation->node && !dependency.reverse;
            });
        }
        for (auto const* operation : group->reverse) {
            if (operation->queued) std::erase_if(prepared.read_dependencies_, [&](auto const& dependency) {
                return dependency.node == operation->node && dependency.reverse;
            });
        }
    }
    for (auto const& request : planning_state_->requests) {
        auto const& reads = request->operation->reads;
        if (std::ranges::find(reads, request->id) == reads.end()) continue;
        prepared.read_dependencies_.push_back({
            .node = static_cast<graph_jit::BackgroundNodeIndex>(request->operation->node),
            .reverse = request->operation->reverse,
            .input = request->input,
            .information = request->information,
            .positions = request->positions,
            .regions = request->metadata.regions,
        });
    }
    prepared.result_ = std::move(result);
    prepared.input_regions_by_port_.resize(plan_->ports.size());
    prepared.input_coverages_by_port_.resize(plan_->ports.size());
    prepared.requested_input_regions_by_port_.resize(plan_->ports.size());
    for (graph_jit::BackgroundPortIndex index = 0;
         index < plan_->ports.size(); ++index) {
        auto const& planned = plan_->ports[index];
        if (planned.direction == graph_jit::PortDirection::input) {
            auto const& required = input_requirements_by_port_[index];
            auto const& sparse = input_sample_requirements_by_port_[index];
            auto& input_regions = prepared.input_regions_by_port_[index];
            input_regions = derive_input_regions(
                *plan_, candidate_output_region_ranges_, index, &required, &sparse, expansion.sparse_sample_budget);
            prepared.input_coverages_by_port_[index] = input_coverage(index);
            auto const& requested = input_selection_requests_by_port_[index];
            auto& selected = prepared.requested_input_regions_by_port_[index];
            auto const& scoped = indexed_input_demands_[index];
            if (requested.empty() && sparse.empty() && scoped.empty()) continue;
            auto const append_requested = [&](std::span<BackgroundInputRegion const> regions) {
                for (auto const& region : regions) {
                    if (requested.intersects(region.range) || sparse.intersects(region.range)
                        || std::ranges::contains(scoped, region.logical_index, &IndexedRegionDemand::region_index))
                        selected.push_back(region);
                }
            };
            if (requested == required) {
                append_requested(input_regions);
            } else {
                append_requested(derive_input_regions(
                    *plan_, candidate_output_region_ranges_, index, &requested, &sparse, expansion.sparse_sample_budget));
            }
        }
    }
    prepared.output_coverages_ = std::move(candidate_output_coverages_);
    prepared.output_region_ranges_ =
        std::move(candidate_output_region_ranges_);
    prepared.output_event_regions_ = std::move(candidate_output_event_regions_);
    prepared.output_coverage_known_ = std::move(candidate_output_coverage_known_);
    prepared.selection_snapshot_ = planning_state_->snapshot;
    prepared.output_regions_by_port_.resize(plan_->ports.size());
    prepared.event_regions_by_port_.resize(plan_->ports.size());
    for (graph_jit::BackgroundPortIndex index = 0;
         index < plan_->ports.size(); ++index) {
        auto const& planned = plan_->ports[index];
        if (planned.direction == graph_jit::PortDirection::output
            && planned.accumulators.output_change) {
            prepared.output_regions_by_port_[index] =
                prepared.output_region_ranges_[output_index(index)];
            prepared.event_regions_by_port_[index] =
                prepared.output_event_regions_[output_index(index)];
        }
    }
    prepared.node_activity_.reserve(node_calls_.size());
    for (auto const& frame : node_calls_) {
        auto activity = frame.activity;
        if (graph_jit::has_activity(
                activity, graph_jit::BackgroundNodeActivity::reverse)) {
            activity = activity | graph_jit::BackgroundNodeActivity::evaluate;
        }
        prepared.node_activity_.push_back(activity);
    }
    return prepared;
}

CoveragePropagationResult BackgroundPropagationWorkspace::commit(
    BackgroundCoverageState& coverage,
    PreparedCoveragePropagation&& prepared) noexcept
{
    coverage.read_dependencies_.swap(prepared.read_dependencies_);
    coverage.output_coverages_.swap(prepared.output_coverages_);
    coverage.output_region_ranges_.swap(prepared.output_region_ranges_);
    coverage.output_event_regions_.swap(prepared.output_event_regions_);
    coverage.output_coverage_known_.swap(prepared.output_coverage_known_);
    candidate_output_coverages_.swap(prepared.output_coverages_);
    candidate_output_region_ranges_.swap(prepared.output_region_ranges_);
    candidate_output_event_regions_.swap(prepared.output_event_regions_);
    candidate_output_coverage_known_.swap(prepared.output_coverage_known_);
    return std::move(prepared.result_);
}

void BackgroundPropagationWorkspace::discard(
    PreparedCoveragePropagation&& prepared) noexcept
{
    candidate_output_coverages_.swap(prepared.output_coverages_);
    candidate_output_region_ranges_.swap(prepared.output_region_ranges_);
    candidate_output_event_regions_.swap(prepared.output_event_regions_);
    candidate_output_coverage_known_.swap(prepared.output_coverage_known_);
}

} // namespace iv
