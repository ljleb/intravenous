#include <intravenous/graph_jit/connections/port_group_plan.h>

#include <algorithm>
#include <iterator>
#include <limits>
#include <numeric>
#include <tuple>
#include <utility>

namespace iv::graph_jit::detail {
namespace {

std::size_t retained_extent(
    std::size_t source_history,
    std::size_t read_latency,
    std::size_t target_history) noexcept
{
    return iv::detail::saturating_add(
        std::max(source_history, target_history), read_latency);
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
    // A producer whose callback is elided has no execution position. Its
    // immutable storage is live from the first realtime consumer.
    bool source_positioned = false;

    if (group.source_port) {
        saw_port = true;
        auto const source_bundle = group.source_port->node_bundle_handle;
        if (source_bundle == plan.boundary_bundle) {
            live.begin = 0;
            source_positioned = true;
        } else if (source_bundle < plan.schedule.bundle_execution_position.size()
            && plan.schedule.bundle_execution_position[source_bundle]) {
            live.begin = *plan.schedule.bundle_execution_position[source_bundle];
            source_positioned = true;
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
            if (!source_positioned) live.begin = 0;
            live.end = execution_count;
        } else if (target < plan.schedule.bundle_execution_position.size()
            && plan.schedule.bundle_execution_position[target]) {
            auto const position = *plan.schedule.bundle_execution_position[target];
            if (!source_positioned) live.begin = std::min(live.begin, position);
            live.end = std::max(live.end, position);
        }
        live.crosses_kernel_invocations =
            live.crosses_kernel_invocations || has_detached_source(connection);
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
            live.crosses_kernel_invocations
            || has_detached_source(connection);
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
    target.persisted_data = target.persisted_data
        || source.persisted_data;
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
    OutputRealization realization,
    PlannedDeliveryMechanism delivery) noexcept
{
    PortStorageRequirements result;
    if (realization == OutputRealization::buffered) {
        result.persisted_data = true;
        result.capture =
            production == PlannedSourceProduction::tick;
    }

    switch (delivery) {
    case PlannedDeliveryMechanism::tick_to_sequential:
        result.current_tick = true;
        break;
    case PlannedDeliveryMechanism::tock_to_sequential:
        if (realization == OutputRealization::computed) {
            result.tick_sequential = true;
        }
        break;
    case PlannedDeliveryMechanism::tock_to_random_access:
    case PlannedDeliveryMechanism::replayed_tick_to_random_access:
        if (realization == OutputRealization::computed) {
            // The configured Coverage input does not yet say which legal callback
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
    OutputRealization realization,
    PlannedDeliveryMechanism delivery) noexcept
{
    auto result = storage_capabilities_for(production, realization, delivery);
    // Capture is an intrinsic source-side transfer obligation. A target may
    // consume the resulting published data, but it never owns another copy of
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
    ContinuousValueStorageGeometry geometry{};
    PlannedSourceProduction production = PlannedSourceProduction::tick;
    OutputRealization realization = OutputRealization::computed;
    std::vector<SampleSourceAtomUse> uses{};
    PortStorageRequirements capabilities{};
};

struct SampleTargetAtomCandidate {
    SampleInputChannelId channel{};
    ChannelLayout layout{};
    ContinuousValueStorageGeometry geometry{};
    PlannedDestinationAccess access = PlannedDestinationAccess::sequential;
    std::vector<SampleTargetAtomUse> uses{};
    PortStorageRequirements capabilities{};
};

void plan_port_atoms(
    ConfiguredGraph const& graph,
    ResolvedPortRealization const& realization,
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
                                ContinuousValueStorageGeometry geometry,
                                PlannedSourceProduction production,
                                OutputRealization realization)
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
            .geometry = geometry,
            .production = production,
            .realization = realization,
        });
        return sample_sources.back();
    };
    auto target_candidate = [&](SampleInputChannelId channel,
                                ChannelLayout layout,
                                ContinuousValueStorageGeometry geometry,
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
            .geometry = geometry,
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
            auto const& resolved = realization.input(node.bundle, port);
            auto const geometry = continuous_value_storage_geometry(
                resolved.value_type, resolved.size);
            IV_ASSERT(geometry.has_value(),
                "resolved continuous input has invalid geometry");
            auto const count = channel_count(config.channel_layout);
            for (std::size_t channel = 0; channel < count; ++channel) {
                target_candidate(
                    SampleInputChannelId{
                        .bundle = node.bundle,
                        .port = port,
                        .channel = channel,
                    },
                    config.channel_layout,
                    *geometry,
                    destination_access(input_access(config)));
            }
        }
        for (std::size_t port = 0; port < node.sample_output_count; ++port) {
            NodeBundlePortId const id{node.bundle, PortKind::sample, port};
            auto const config = graph.node_bundles.resolve_sample_output(id).config;
            auto const& resolved = realization.output(node.bundle, port);
            auto const geometry = continuous_value_storage_geometry(
                resolved.value_type, resolved.size);
            IV_ASSERT(geometry.has_value(),
                "resolved continuous output has invalid geometry");
            auto const production = source_production(plan, node.bundle);
            PortStorageRequirements intrinsic;
            if (config.realization == OutputRealization::buffered) {
                intrinsic.persisted_data = true;
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
                    *geometry,
                    production,
                    config.realization);
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
                connection.geometry,
                timing.production,
                timing.realization);
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
                    .detached = has_detached_source(connection),
                });
            }
            join_capabilities(
                candidate.capabilities,
                storage_capabilities_for(
                    timing.production, timing.realization, timing.delivery));
        }

        for (std::size_t target_channel = 0;
             target_channel < connection.target_channels.size();
             ++target_channel) {
            auto& candidate = target_candidate(
                connection.target_channels[target_channel],
                connection.target_layout,
                connection.geometry,
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
                    .detached = has_detached_source(connection),
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
                            timing.realization,
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
                    .detached = has_detached_source(connection),
                });
                for (auto const& timing : connection.source_channel_timings) {
                    join_capabilities(
                        candidate.capabilities,
                        target_capabilities_for(
                            timing.production,
                            timing.realization,
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
                    || existing.geometry != candidate.geometry
                    || existing.production != candidate.production
                    || existing.realization != candidate.realization
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
                    .geometry = candidate.geometry,
                    .production = candidate.production,
                    .realization = candidate.realization,
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
                    || existing.geometry != candidate.geometry
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
                    .geometry = candidate.geometry,
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
                .access = destination_access(input_access(config)),
            });
        }
        for (std::size_t port = 0; port < node.event_output_count; ++port) {
            NodeBundlePortId const id{node.bundle, PortKind::event, port};
            auto const config = graph.node_bundles.resolve_event_output(id).config;
            auto const production = source_production(plan, node.bundle);
            PortStorageRequirements intrinsic;
            if (config.realization == OutputRealization::buffered) {
                intrinsic.persisted_data = true;
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
                .realization = config.realization,
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
                        .type = source.source_type,
                        .production = source.production,
                        .realization = source.realization,
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
                        source.realization,
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
                        source.realization,
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

std::expected<void, std::string> plan_sample_groups(
    ConnectionAnalysisPlan& plan,
    std::size_t kernel_block_size)
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
            for (std::size_t channel_index = 0;
                 channel_index < connection.source_channel_timings.size();
                 ++channel_index) {
                auto const& channel =
                    connection.source_channel_timings[channel_index];
                if (!uses_realtime_storage(channel.delivery)) continue;
                // A callback-elided producer's immutable storage serves every
                // consumer window, whatever its invocation block.
                if (channel.source.bundle < plan.schedule.callback_elided.size()
                    && plan.schedule.callback_elided[channel.source.bundle] != 0) {
                    continue;
                }
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
    // materialization/persisted-data ports.
    std::vector<NodeBundlePortId> connected_source_ports;
    for (auto const& atom : plan.background.sample_source_subsets) {
        if (!atom.connection_indices.empty()) {
            append_unique(connected_source_ports, atom.port);
        }
    }
    auto connection_for_source_port = [&](NodeBundlePortId port)
        -> SampleConnectionPlan const* {
        auto const found = std::ranges::find_if(
            plan.sample_connections,
            [&](SampleConnectionPlan const& connection) {
                return std::ranges::any_of(
                    connection.source_channels,
                    [&](SampleOutputChannelId channel) {
                        return channel.bundle == port.node_bundle_handle
                            && channel.port == port.port_index;
                    });
            });
        return found == plan.sample_connections.end() ? nullptr : &*found;
    };
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
            auto const* connection = connection_for_source_port(atom.port);
            IV_ASSERT(connection != nullptr,
                "continuous producer group has no connection realization");
            plan.sample_producer_groups.push_back(SampleProducerGroupPlan{
                .source_port = atom.port,
                .source_type = atom.source_layout.channel_type,
                .canonical_source_layout = atom.source_layout,
                .geometry = connection->geometry,
                .absolute_pace_exponent =
                    connection->absolute_pace_exponent,
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

    for (auto& group : plan.sample_producer_groups) {
        std::size_t retained = 0;
        for (auto const connection_index : group.connection_indices) {
            auto const& connection = plan.sample_connections[connection_index];
            std::size_t connection_retained = 0;
            bool saw_group_realtime_channel = false;
            bool saw_group_background_channel = false;
            for (std::size_t channel_index = 0;
                 channel_index < connection.source_channel_timings.size();
                 ++channel_index) {
                auto const& channel =
                    connection.source_channel_timings[channel_index];
                if (!group.source_port
                    || source_port_for(channel.source) != *group.source_port) {
                    continue;
                }
                if (!uses_realtime_storage(channel.delivery)) {
                    saw_group_background_channel = true;
                    continue;
                }
                saw_group_realtime_channel = true;
                // A detached consumer owns its delay/history storage in
                // the branch-local retained feedback timeline. The canonical producer still
                // retains its own declared output history, which its callback
                // may address independently of any consumer.
                auto const has_detached_use = channel.detach
                    || std::ranges::any_of(
                        connection.routed_contributions,
                        [&](SampleRoutedContributionPlan const& contribution) {
                            return contribution.detach
                                && contribution.source_coefficients[channel_index]
                                    != 0.0f;
                        });
                auto const has_ordinary_use = !channel.detach
                    && (connection.routed_contributions.empty()
                        || std::ranges::any_of(
                            connection.routed_contributions,
                            [&](SampleRoutedContributionPlan const& contribution) {
                                return !contribution.detach
                                    && contribution.source_coefficients[channel_index]
                                        != 0.0f;
                            }));
                if (has_detached_use) {
                    connection_retained = std::max(
                        connection_retained,
                        iv::detail::saturating_add(
                            channel.source_history, channel.source_latency));
                }
                if (has_ordinary_use) {
                    connection_retained = std::max(
                        connection_retained,
                        retained_extent(
                            channel.source_history,
                            channel.read_latency,
                            connection.target_history));
                }
            }
            group.has_background_connections = group.has_background_connections
                || saw_group_background_channel;
            if (!saw_group_realtime_channel) continue;
            group.has_realtime_connections = true;
            // Only Tick -> Sequential contributions enter realtime storage
            // planning. A mixed composition may also have background-materialized
            // source channels, but those are not represented by this buffer.
            group.external_boundary = group.external_boundary
                || connection.external_boundary;
            retained = std::max(retained, connection_retained);
        }
        auto const current_value_count =
            FractionalGlobalIndexRange::maximum_projection_count(
                kernel_block_size, group.absolute_pace_exponent);
        if (!current_value_count) {
            return std::unexpected(
                "GraphJit continuous producer block projection cannot fit size_t");
        }
        group.storage_requirements = SampleConnectionStorageRequirements{
            .current_block_frames = *current_value_count,
            .retained_frames = retained,
            .channel_count = group.canonical_source_layout
                ? channel_count(*group.canonical_source_layout)
                : channel_count(group.source_type),
            .value_size_bytes = group.geometry.value_size_bytes,
        };
        if (!group.has_realtime_connections) continue;
        group.live_interval = live_interval_for_sample_group(plan, group);
        group.live_interval.crosses_kernel_invocations =
            group.live_interval.crosses_kernel_invocations || retained != 0;
    }
    return {};
}

std::expected<void, std::string> plan_event_groups(
    ConnectionAnalysisPlan& plan,
    std::size_t kernel_block_size)
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

        std::vector<EventConversionPlan> source_conversions;
        source_conversions.reserve(connection.source_plans.size());
        for (auto const& source : connection.source_plans)
            source_conversions.push_back(source.conversion);
        // Each realtime target reads its sources at its own compensation; the
        // targets that need the same compensation for every source read one
        // group. A connection without realtime targets joins one group.
        struct TargetPartition {
            std::vector<std::size_t> source_compensations{};
            std::vector<EventInputPortId> targets{};
        };
        std::vector<TargetPartition> partitions;
        for (std::size_t target_index = 0;
             target_index < connection.target_plans.size(); ++target_index) {
            std::vector<std::size_t> target_compensations(
                connection.source_plans.size(), 0);
            bool realtime_target = false;
            for (auto const& delivery : connection.deliveries) {
                if (delivery.target_index != target_index
                    || !uses_realtime_storage(delivery.mechanism)
                    || delivery.source_index >= connection.source_plans.size()) {
                    continue;
                }
                realtime_target = true;
                auto const latency =
                    connection.source_plans[delivery.source_index].latency;
                target_compensations[delivery.source_index] =
                    delivery.read_latency > latency
                    ? delivery.read_latency - latency
                    : 0;
            }
            if (!realtime_target) continue;
            auto const target = connection.target_plans[target_index].target;
            auto partition = std::ranges::find(
                partitions,
                target_compensations,
                &TargetPartition::source_compensations);
            if (partition == partitions.end()) {
                partitions.push_back(TargetPartition{
                    .source_compensations = std::move(target_compensations),
                });
                partition = std::prev(partitions.end());
            }
            if (!std::ranges::contains(partition->targets, target)) {
                partition->targets.push_back(target);
            }
        }
        if (partitions.empty()) {
            partitions.push_back(TargetPartition{
                .source_compensations = std::vector<std::size_t>(
                    connection.source_plans.size(), 0),
            });
        }
        for (auto& partition : partitions) {
        auto group = std::ranges::find_if(
            plan.event_producer_groups,
            [&](EventProducerGroupPlan const& candidate) {
                return candidate.source_type == connection.source_type
                    && candidate.sources == connection.sources
                    && candidate.source_conversions == source_conversions
                    && candidate.source_compensations
                        == partition.source_compensations;
            });
        if (group == plan.event_producer_groups.end()) {
            plan.event_producer_groups.push_back(EventProducerGroupPlan{
                .source_type = connection.source_type,
                .sources = connection.sources,
                .source_conversions = source_conversions,
                .source_compensations = partition.source_compensations,
            });
            group = std::prev(plan.event_producer_groups.end());
        }
        group->connection_indices.push_back(i);
        group->connection_targets.push_back(std::move(partition.targets));

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
    }

    for (auto& group : plan.event_producer_groups) {
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
            group.external_boundary = group.external_boundary
                || connection.external_boundary;
            requires_invocation_aggregate = requires_invocation_aggregate
                || connection.requires_conversion
                || connection.requires_block_materialization
                || has_detached_source(connection);
            for (auto const& delivery : connection.deliveries) {
                if (!uses_realtime_storage(delivery.mechanism)) continue;
                auto const& source =
                    connection.source_plans[delivery.source_index];
                auto const& target =
                    connection.target_plans[delivery.target_index];
                // The group stream holds each event where its readers read
                // it, its read latency after its position: it keeps the
                // targets' history behind the block and the positions not
                // read yet ahead of it.
                retained = std::max(
                    retained,
                    retained_extent(
                        source.history,
                        std::max(delivery.read_latency, source.latency),
                        target.history));
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
        group.live_interval = live_interval_for_event_group(plan, group);
        group.live_interval.crosses_kernel_invocations =
            group.live_interval.crosses_kernel_invocations || retained != 0;
    }
    return {};
}

} // namespace

std::expected<void, std::string> plan_port_groups(
    ConfiguredGraph const& graph,
    ResolvedPortRealization const& realization,
    ConnectionAnalysisPlan& plan,
    std::size_t kernel_block_size)
{
    plan_port_atoms(graph, realization, plan);
    if (auto samples = plan_sample_groups(
            plan, kernel_block_size); !samples) {
        return std::unexpected(std::move(samples.error()));
    }
    if (auto events = plan_event_groups(
            plan, kernel_block_size); !events) {
        return std::unexpected(std::move(events.error()));
    }
    return {};
}

} // namespace iv::graph_jit::detail
