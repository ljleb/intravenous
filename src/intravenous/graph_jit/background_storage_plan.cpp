#include <intravenous/graph_jit/background_storage_plan.h>

#include <algorithm>
#include <cstddef>
#include <expected>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace iv::graph_jit::detail {
namespace {

template<class T>
void append_unique(std::vector<T>& values, T value)
{
    if (!std::ranges::contains(values, value)) values.push_back(std::move(value));
}

std::optional<std::size_t> aliased_source_channel(
    ChannelTypeId source_type,
    ChannelTypeId target_type,
    std::size_t target_channel)
{
    if (source_type == target_type) {
        if (target_channel < channel_count(source_type)) return target_channel;
        return std::nullopt;
    }
    if (source_type == ChannelTypeId::mono
        && target_type == ChannelTypeId::stereo
        && target_channel < channel_count(ChannelTypeId::stereo)) {
        return 0;
    }
    return std::nullopt;
}

std::optional<BackgroundPortIndex> output_port_for(
    BackgroundEvaluationPlan const& background,
    NodeBundlePortId requested_port)
{
    auto const found = std::ranges::find_if(
        background.ports,
        [&](BackgroundPortPlan const& planned) {
            return planned.direction == PortDirection::output
                && planned.configured_port == requested_port;
        });
    if (found == background.ports.end()) return std::nullopt;
    return static_cast<BackgroundPortIndex>(
        std::distance(background.ports.begin(), found));
}

std::vector<BackgroundConnectionIndex> connections_for_source_subset(
    BackgroundEvaluationPlan const& background,
    PortKind kind,
    PortSubsetIndex subset)
{
    std::vector<BackgroundConnectionIndex> result;
    for (BackgroundConnectionIndex connection = 0;
         connection < background.connections.size(); ++connection) {
        auto const& candidate = background.connections[connection];
        if (candidate.kind == kind
            && std::ranges::contains(candidate.source_subsets, subset)) {
            result.push_back(connection);
        }
    }
    return result;
}

std::optional<PortSubsetIndex> sample_source_subset_for(
    BackgroundEvaluationPlan const& background,
    BackgroundConnectionPlan const& connection,
    SampleOutputChannelId channel)
{
    for (auto const subset : connection.source_subsets) {
        if (subset < background.sample_source_subsets.size()
            && std::ranges::contains(
                background.sample_source_subsets[subset].channels, channel)) {
            return subset;
        }
    }
    return std::nullopt;
}

std::optional<PortSubsetIndex> event_source_subset_for(
    BackgroundEvaluationPlan const& background,
    BackgroundConnectionPlan const& connection,
    EventOutputPortId port)
{
    for (auto const subset : connection.source_subsets) {
        if (subset < background.event_source_subsets.size()
            && background.event_source_subsets[subset].port == port) {
            return subset;
        }
    }
    return std::nullopt;
}

SampleConnectionPlan const* sample_connection_for(
    ConnectionAnalysisPlan const& connections,
    BackgroundConnectionPlan const& background)
{
    auto const found = std::ranges::find_if(
        connections.sample_connections,
        [&](SampleConnectionPlan const& connection) {
            return connection.configured_connection_index
                    == background.configured_connection_index
                && connection.source_type == background.sample_source_type
                && connection.target_type == background.sample_target_type
                && connection.source_channels == background.sample_source_channels
                && connection.target_channels == background.sample_target_channels;
        });
    return found == connections.sample_connections.end() ? nullptr : &*found;
}

EventConnectionPlan const* event_connection_for(
    ConnectionAnalysisPlan const& connections,
    BackgroundConnectionPlan const& background)
{
    auto const found = std::ranges::find_if(
        connections.event_connections,
        [&](EventConnectionPlan const& connection) {
            if (connection.configured_connection_index
                    != background.configured_connection_index
                || connection.source_type != background.event_source_type
                || connection.target_type != background.event_target_type
                || connection.deliveries.size()
                    != background.event_deliveries.size()) {
                return false;
            }
            for (std::size_t delivery = 0;
                 delivery < connection.deliveries.size(); ++delivery) {
                auto const& planned = connection.deliveries[delivery];
                if (planned.source_index >= connection.source_plans.size()
                    || planned.target_index >= connection.target_plans.size()) {
                    return false;
                }
                auto const& retained = background.event_deliveries[delivery];
                if (connection.source_plans[planned.source_index].source
                        != retained.source
                    || connection.target_plans[planned.target_index].target
                        != retained.target
                    || planned.mechanism != retained.mechanism) {
                    return false;
                }
            }
            return true;
        });
    return found == connections.event_connections.end() ? nullptr : &*found;
}

std::optional<std::size_t> direct_sample_source_index(
    SampleConnectionPlan const& connection,
    std::size_t target_channel)
{
    if (connection.projection_contributions.empty()) {
        auto const source_channel = aliased_source_channel(
            connection.source_type, connection.target_type, target_channel);
        if (!source_channel
            || *source_channel >= connection.source_channel_timings.size()) {
            return std::nullopt;
        }
        return *source_channel;
    }

    std::optional<std::size_t> result;
    for (auto const& projection : connection.projection_contributions) {
        auto const target = std::ranges::find(
            projection.target_channels, target_channel);
        if (target == projection.target_channels.end()) continue;
        if (result) return std::nullopt;
        auto const target_position = static_cast<std::size_t>(
            std::distance(projection.target_channels.begin(), target));
        auto const source_position = aliased_source_channel(
            projection.source_type,
            projection.target_type,
            target_position);
        if (!source_position
            || *source_position >= projection.source_channel_indices.size()) {
            return std::nullopt;
        }
        auto const source_index =
            projection.source_channel_indices[*source_position];
        if (source_index >= connection.source_channel_timings.size()) {
            return std::nullopt;
        }
        result = source_index;
    }
    return result;
}

std::optional<PortStorageIndex> find_storage(
    BackgroundStoragePlan const& plan,
    std::span<PortStorageIndex const> storage_indices,
    PortStorageKind kind)
{
    auto const found = std::ranges::find_if(
        storage_indices,
        [&](PortStorageIndex index) {
            return index < plan.ports.size()
                && plan.ports[index].storage == kind;
        });
    return found == storage_indices.end()
        ? std::nullopt
        : std::optional<PortStorageIndex>{*found};
}

template<class SourceSubset>
std::optional<PortStorageIndex> storage_for_delivery(
    BackgroundStoragePlan const& plan,
    std::span<PortStorageIndex const> storage_indices,
    SourceSubset const& subset,
    PlannedDeliveryMechanism delivery,
    PortStorageKind requested_storage)
{
    // Replay reads background-evaluation storage, never the live current-Tick
    // buffer.
    if (requested_storage == PortStorageKind::background) {
        if (subset.retention == OutputRetention::persisted) {
            return find_storage(
                plan,
                storage_indices,
                PortStorageKind::persisted_pages);
        }
        return find_storage(
            plan,
            storage_indices,
            PortStorageKind::background);
    }
    if (subset.retention == OutputRetention::persisted) {
        if (delivery == PlannedDeliveryMechanism::tick_to_sequential) {
            return find_storage(
                plan,
                storage_indices,
                PortStorageKind::current_tick);
        }
        return find_storage(
            plan,
            storage_indices,
            PortStorageKind::persisted_pages);
    }
    if (delivery == PlannedDeliveryMechanism::tick_to_sequential) {
        return find_storage(
            plan,
            storage_indices,
            PortStorageKind::current_tick);
    }
    if (requested_storage == PortStorageKind::tick_random_access) {
        return find_storage(
            plan,
            storage_indices,
            PortStorageKind::tick_random_access);
    }
    if (auto const addressable = find_storage(
            plan,
            storage_indices,
            PortStorageKind::tick_random_access)) {
        return addressable;
    }
    return find_storage(
        plan,
        storage_indices,
        PortStorageKind::tick_sequential);
}

bool target_is_replay_input(
    BackgroundEvaluationPlan const& background,
    NodeBundlePortId requested_port)
{
    return std::ranges::any_of(
        background.ports,
        [&](BackgroundPortPlan const& planned) {
            return planned.direction == PortDirection::input
                && planned.configured_port == requested_port
                && planned.replay_sequential_input;
        });
}

bool equal_sample_materialization_operation(
    SampleMaterializationPlan const& left,
    SampleMaterializationPlan const& right)
{
    return left.source_subsets == right.source_subsets
        && left.inputs == right.inputs
        && left.source_channels == right.source_channels
        && left.source_read_latencies == right.source_read_latencies
        && left.source_type == right.source_type
        && left.target_layout == right.target_layout
        && left.target_channels == right.target_channels
        && left.projections == right.projections
        && left.target_history == right.target_history;
}

bool equal_sample_materialization_key(
    SampleMaterializationPlan const& left,
    SampleMaterializationPlan const& right)
{
    return left.storage == right.storage
        && equal_sample_materialization_operation(left, right);
}

bool equal_event_materialization_operation(
    EventMaterializationPlan const& left,
    EventMaterializationPlan const& right)
{
    return left.source_subsets == right.source_subsets
        && left.inputs == right.inputs
        && left.source_type == right.source_type
        && left.target_type == right.target_type
        && left.conversion == right.conversion
        && left.target_history == right.target_history;
}

bool equal_event_materialization_key(
    EventMaterializationPlan const& left,
    EventMaterializationPlan const& right)
{
    return left.storage == right.storage
        && equal_event_materialization_operation(left, right);
}

} // namespace

std::expected<BackgroundStoragePlan, std::string> build_background_storage_plan(
    ConnectionAnalysisPlan const& connections)
{
    auto const& background = connections.background;
    BackgroundStoragePlan result;
    result.sample_source_storage.resize(
        background.sample_source_subsets.size());
    result.sample_target_storage.resize(
        background.sample_target_subsets.size());
    result.event_source_storage.resize(
        background.event_source_subsets.size());
    result.event_target_storage.resize(
        background.event_target_subsets.size());
    result.connections.resize(background.connections.size());

    auto append_storage = [&](PortStoragePlan storage) {
        auto const index = result.ports.size();
        result.ports.push_back(std::move(storage));
        return static_cast<PortStorageIndex>(index);
    };

    auto append_source_storage = [&] (
        PortKind kind,
        PortSubsetIndex subset,
        NodeBundlePortId port,
        PortStorageKind storage_kind,
        ChannelLayout sample_layout,
        std::span<std::size_t const> sample_channels,
        EventTypeId event_type,
        double max_events_per_index,
        bool coalesce_port) {
        auto existing = result.ports.end();
        if (coalesce_port) {
            existing = std::ranges::find_if(
                result.ports,
                [&](PortStoragePlan const& candidate) {
                    return candidate.kind == kind
                        && candidate.source_port
                        && candidate.storage == storage_kind
                        && *candidate.source_port == port;
                });
        }
        PortStorageIndex storage_index;
        if (existing == result.ports.end()) {
            storage_index = append_storage(PortStoragePlan{
                .kind = kind,
                .storage = storage_kind,
                .source_subsets = {subset},
                .connections = connections_for_source_subset(background, kind, subset),
                .source_port = port,
                .output_port = output_port_for(background, port),
                .sample_layout = sample_layout,
                .sample_channels = {
                    sample_channels.begin(), sample_channels.end()},
                .event_type = event_type,
                .max_events_per_index = max_events_per_index,
            });
        } else {
            storage_index = static_cast<PortStorageIndex>(
                std::distance(result.ports.begin(), existing));
            append_unique(existing->source_subsets, subset);
            for (auto const connection :
                 connections_for_source_subset(background, kind, subset)) {
                append_unique(existing->connections, connection);
            }
            for (auto const channel : sample_channels) {
                append_unique(existing->sample_channels, channel);
            }
        }
        auto& storage_indices = kind == PortKind::sample
            ? result.sample_source_storage[subset]
            : result.event_source_storage[subset];
        append_unique(storage_indices, storage_index);
    };

    for (PortSubsetIndex subset = 0;
         subset < background.sample_source_subsets.size(); ++subset) {
        auto const& source = background.sample_source_subsets[subset];
        std::vector<std::size_t> channels;
        channels.reserve(source.channels.size());
        for (auto const channel : source.channels) channels.push_back(channel.channel);
        auto const port_is_background = std::ranges::any_of(
            background.connections,
            [&](BackgroundConnectionPlan const& connection) {
                return connection.kind == PortKind::sample
                    && std::ranges::contains(connection.source_subsets, subset);
            });
        auto append = [&](PortStorageKind storage,
                          bool coalesce_port = false) {
            append_source_storage(
                PortKind::sample,
                subset,
                source.port,
                storage,
                source.source_layout,
                channels,
                EventTypeId::empty,
                0.0,
                coalesce_port);
        };
        if (source.storage.current_tick && port_is_background) {
            append(PortStorageKind::current_tick, true);
        }
        if (source.storage.persisted_pages) {
            append(
                PortStorageKind::persisted_pages,
                true);
        }
        if (source.storage.tick_random_access) {
            append(PortStorageKind::tick_random_access);
        } else if (source.storage.tick_sequential) {
            append(PortStorageKind::tick_sequential);
        }
        if (source.storage.background_random_access) {
            append(
                PortStorageKind::background);
        }
    }

    for (PortSubsetIndex subset = 0;
         subset < background.event_source_subsets.size(); ++subset) {
        auto const& source = background.event_source_subsets[subset];
        NodeBundlePortId const port{
            source.port.bundle, PortKind::event, source.port.port};
        auto const port_is_background = std::ranges::any_of(
            background.connections,
            [&](BackgroundConnectionPlan const& connection) {
                return connection.kind == PortKind::event
                    && std::ranges::contains(connection.source_subsets, subset);
            });
        auto append = [&](PortStorageKind storage,
                          bool coalesce_port = false) {
            append_source_storage(
                PortKind::event,
                subset,
                port,
                storage,
                {},
                {},
                source.type,
                source.max_events_per_index,
                coalesce_port);
        };
        if (source.storage.current_tick && port_is_background) {
            append(PortStorageKind::current_tick, true);
        }
        if (source.storage.persisted_pages) {
            append(
                PortStorageKind::persisted_pages,
                true);
        }
        if (source.storage.tick_random_access) {
            append(PortStorageKind::tick_random_access);
        } else if (source.storage.tick_sequential) {
            append(PortStorageKind::tick_sequential);
        }
        if (source.storage.background_random_access) {
            append(
                PortStorageKind::background);
        }
    }

    auto append_sample_materialization = [&] (
        SampleMaterializationPlan planned,
        PortSubsetIndex target_subset,
        BackgroundConnectionIndex connection)
        -> SampleMaterializationIndex {
        auto found = std::ranges::find_if(
            result.sample_materializations,
            [&](SampleMaterializationPlan const& existing) {
                return equal_sample_materialization_key(existing, planned);
            });
        if (found == result.sample_materializations.end()
            && (planned.storage
                    == PortStorageKind::tick_sequential
                || planned.storage
                    == PortStorageKind::tick_random_access)) {
            auto const subsuming_storage = planned.storage
                    == PortStorageKind::tick_sequential
                ? PortStorageKind::tick_random_access
                : PortStorageKind::tick_sequential;
            found = std::ranges::find_if(
                result.sample_materializations,
                [&](SampleMaterializationPlan const& existing) {
                    return existing.storage == subsuming_storage
                        && equal_sample_materialization_operation(
                            existing, planned);
                });
            if (found != result.sample_materializations.end()
                && planned.storage
                    == PortStorageKind::tick_random_access) {
                // Tick-time random-access storage supplies sequential slices too.
                // Promote an earlier sequential-only plan so planning is
                // independent of configured connection order.
                found->storage =
                    PortStorageKind::tick_random_access;
                result.ports[found->output].storage =
                    PortStorageKind::tick_random_access;
            }
        }
        if (found != result.sample_materializations.end()) {
            auto const index = static_cast<SampleMaterializationIndex>(
                std::distance(result.sample_materializations.begin(), found));
            append_unique(found->target_subsets, target_subset);
            append_unique(found->connections, connection);
            auto& storage =
                result.ports[found->output];
            append_unique(storage.target_subsets, target_subset);
            append_unique(storage.connections, connection);
            append_unique(
                result.sample_target_storage[target_subset],
                found->output);
            return index;
        }

        auto const output = append_storage(PortStoragePlan{
            .kind = PortKind::sample,
            .storage = planned.storage,
            .source_subsets = planned.source_subsets,
            .target_subsets = {target_subset},
            .connections = {connection},
            .inputs = planned.inputs,
            .sample_layout = planned.target_layout,
            .sample_channels = planned.target_channels,
        });
        planned.output = output;
        planned.target_subsets.push_back(target_subset);
        planned.connections.push_back(connection);
        auto const index = static_cast<SampleMaterializationIndex>(
            result.sample_materializations.size());
        result.sample_materializations.push_back(std::move(planned));
        result.ports[output].sample_materialization = index;
        append_unique(result.sample_target_storage[target_subset], output);
        return index;
    };

    auto append_event_materialization = [&] (
        EventMaterializationPlan planned,
        PortSubsetIndex target_subset,
        BackgroundConnectionIndex connection,
        double max_events_per_index)
        -> EventMaterializationIndex {
        auto found = std::ranges::find_if(
            result.event_materializations,
            [&](EventMaterializationPlan const& existing) {
                return equal_event_materialization_key(existing, planned);
            });
        if (found == result.event_materializations.end()
            && (planned.storage
                    == PortStorageKind::tick_sequential
                || planned.storage
                    == PortStorageKind::tick_random_access)) {
            auto const subsuming_storage = planned.storage
                    == PortStorageKind::tick_sequential
                ? PortStorageKind::tick_random_access
                : PortStorageKind::tick_sequential;
            found = std::ranges::find_if(
                result.event_materializations,
                [&](EventMaterializationPlan const& existing) {
                    return existing.storage == subsuming_storage
                        && equal_event_materialization_operation(existing, planned);
                });
            if (found != result.event_materializations.end()
                && planned.storage
                    == PortStorageKind::tick_random_access) {
                found->storage =
                    PortStorageKind::tick_random_access;
                result.ports[found->output].storage =
                    PortStorageKind::tick_random_access;
            }
        }
        if (found != result.event_materializations.end()) {
            auto const index = static_cast<EventMaterializationIndex>(
                std::distance(result.event_materializations.begin(), found));
            append_unique(found->target_subsets, target_subset);
            append_unique(found->connections, connection);
            auto& storage =
                result.ports[found->output];
            append_unique(storage.target_subsets, target_subset);
            append_unique(storage.connections, connection);
            append_unique(
                result.event_target_storage[target_subset],
                found->output);
            return index;
        }

        auto const output = append_storage(PortStoragePlan{
            .kind = PortKind::event,
            .storage = planned.storage,
            .source_subsets = planned.source_subsets,
            .target_subsets = {target_subset},
            .connections = {connection},
            .inputs = planned.inputs,
            .event_type = planned.target_type,
            .max_events_per_index = max_events_per_index,
        });
        planned.output = output;
        planned.target_subsets.push_back(target_subset);
        planned.connections.push_back(connection);
        auto const index = static_cast<EventMaterializationIndex>(
            result.event_materializations.size());
        result.event_materializations.push_back(std::move(planned));
        result.ports[output].event_materialization = index;
        append_unique(result.event_target_storage[target_subset], output);
        return index;
    };

    for (BackgroundConnectionIndex connection_index = 0;
         connection_index < background.connections.size(); ++connection_index) {
        auto const& background_connection =
            background.connections[connection_index];
        auto& storage_connection = result.connections[connection_index];

        if (background_connection.kind == PortKind::sample) {
            auto const* connection = sample_connection_for(
                connections, background_connection);
            if (!connection) {
                return std::unexpected(
                    "GraphJit background sample storage planning lost its normalized connection");
            }
            for (auto const target_subset_index :
                 background_connection.target_subsets) {
                if (target_subset_index >= background.sample_target_subsets.size()) {
                    return std::unexpected(
                        "GraphJit background sample storage planning has an invalid target subset");
                }
                auto const& target_subset =
                    background.sample_target_subsets[target_subset_index];
                struct DirectChannel {
                    std::size_t target_channel = 0;
                    std::size_t source_index = 0;
                    PortSubsetIndex source_subset = 0;
                };
                std::vector<DirectChannel> direct_channels;
                bool direct = true;
                for (auto const target_channel_id : target_subset.channels) {
                    auto const source_index = direct_sample_source_index(
                        *connection, target_channel_id.channel);
                    if (!source_index
                        || *source_index
                            >= connection->source_channel_timings.size()) {
                        direct = false;
                        break;
                    }
                    auto const& source =
                        connection->source_channel_timings[*source_index];
                    auto const source_subset = sample_source_subset_for(
                        background, background_connection, source.source);
                    if (!source_subset) {
                        direct = false;
                        break;
                    }
                    direct_channels.push_back(DirectChannel{
                        .target_channel = target_channel_id.channel,
                        .source_index = *source_index,
                        .source_subset = *source_subset,
                    });
                }

                if (direct) {
                    for (auto const& channel : direct_channels) {
                        auto const& timing =
                            connection->source_channel_timings[channel.source_index];
                        auto const& source_subset =
                            background.sample_source_subsets[channel.source_subset];
                        auto const& ports =
                            result.sample_source_storage[channel.source_subset];
                        std::vector<PortStorageIndex> selected;
                        if (target_subset.access
                            == PlannedDestinationAccess::random_access
                            && source_subset.retention
                                == OutputRetention::ephemeral) {
                            for (auto const storage_kind : {
                                     PortStorageKind::tick_random_access,
                                     PortStorageKind::background}) {
                                if (auto const storage =
                                        storage_for_delivery(
                                            result,
                                            ports,
                                            source_subset,
                                            timing.delivery,
                                            storage_kind)) {
                                    append_unique(selected, *storage);
                                }
                            }
                        } else {
                            auto const context = target_subset.access
                                    == PlannedDestinationAccess::random_access
                                ? PortStorageKind::tick_random_access
                                : PortStorageKind::tick_sequential;
                            if (auto const storage =
                                    storage_for_delivery(
                                        result,
                                        ports,
                                        source_subset,
                                        timing.delivery,
                                        context)) {
                                selected.push_back(*storage);
                            }
                        }
                        if (target_subset.access
                                == PlannedDestinationAccess::sequential
                            && target_is_replay_input(
                                background, target_subset.port)) {
                            if (auto const replay_storage =
                                    storage_for_delivery(
                                        result,
                                        ports,
                                        source_subset,
                                        timing.delivery,
                                        PortStorageKind::
                                            background)) {
                                append_unique(
                                    selected, *replay_storage);
                            }
                        }
                        if (selected.empty()) {
                            return std::unexpected(
                                "GraphJit background sample direct source has no legal storage");
                        }
                        for (auto const storage : selected) {
                            auto const direct_index = result.direct_samples.size();
                            result.direct_samples.push_back(
                                DirectSampleConnectionPlan{
                                    .connection = connection_index,
                                    .target_subset = target_subset_index,
                                    .target_channel = channel.target_channel,
                                    .source_subset = channel.source_subset,
                                    .source_channel = timing.source.channel,
                                    .storage = storage,
                                    .delivery = timing.delivery,
                                    .read_latency = timing.read_latency,
                                    .target_history = connection->target_history,
                                });
                            storage_connection.direct_samples.push_back(
                                direct_index);
                            append_unique(
                                result.sample_target_storage[
                                    target_subset_index],
                                storage);
                        }
                    }
                    continue;
                }

                std::vector<std::size_t> source_indices;
                std::vector<SampleProjectionContributionPlan const*>
                    projections;
                if (connection->projection_contributions.empty()) {
                    source_indices.resize(
                        connection->source_channel_timings.size());
                    for (std::size_t source_index = 0;
                         source_index < source_indices.size(); ++source_index) {
                        source_indices[source_index] = source_index;
                    }
                } else {
                    auto const target_atom_has_channel =
                        [&](std::size_t target_channel) {
                            return target_channel
                                    < connection->target_channels.size()
                                && std::ranges::contains(
                                target_subset.channels,
                                connection->target_channels[target_channel]);
                        };
                    for (auto const& projection :
                         connection->projection_contributions) {
                        if (!std::ranges::any_of(
                                projection.target_channels,
                                target_atom_has_channel)) {
                            continue;
                        }
                        if (!std::ranges::all_of(
                                projection.target_channels,
                                target_atom_has_channel)) {
                            return std::unexpected(
                                "GraphJit background sample projection spans multiple target subsets");
                        }
                        projections.push_back(&projection);
                        for (auto const source_index :
                             projection.source_channel_indices) {
                            if (source_index
                                >= connection->source_channel_timings.size()) {
                                return std::unexpected(
                                    "GraphJit background sample projection has an invalid source index");
                            }
                            append_unique(source_indices, source_index);
                        }
                    }
                    if (projections.empty()) {
                        return std::unexpected(
                            "GraphJit background sample target subset has no projection contribution");
                    }
                }

                std::vector<PortStorageKind> storage_kinds;
                if (target_subset.access
                    == PlannedDestinationAccess::random_access) {
                    storage_kinds = {
                        PortStorageKind::tick_random_access,
                        PortStorageKind::background,
                    };
                } else {
                    auto const has_current = std::ranges::any_of(
                        source_indices,
                        [&](std::size_t source_index) {
                            return connection
                                       ->source_channel_timings[source_index]
                                       .delivery
                                    == PlannedDeliveryMechanism::tick_to_sequential;
                        });
                    storage_kinds.push_back(
                        has_current
                            ? PortStorageKind::current_tick
                            : PortStorageKind::tick_sequential);
                    if (target_is_replay_input(background, target_subset.port)) {
                        append_unique(
                            storage_kinds,
                            PortStorageKind::
                                background);
                    }
                }

                for (auto const storage_kind : storage_kinds) {
                    SampleMaterializationPlan materialization{
                        .storage = storage_kind,
                        .source_type = connection->source_type,
                        .target_layout = connection->target_layout,
                        .target_history = connection->target_history,
                    };
                    for (auto const channel : target_subset.channels) {
                        materialization.target_channels.push_back(channel.channel);
                    }
                    std::vector<std::optional<std::size_t>> local_source_index(
                        connection->source_channel_timings.size());
                    for (auto const source_index : source_indices) {
                        auto const& timing =
                            connection->source_channel_timings[source_index];
                        auto const source_subset = sample_source_subset_for(
                            background, background_connection, timing.source);
                        if (!source_subset
                            || !std::ranges::contains(
                                target_subset.source_subsets, *source_subset)) {
                            return std::unexpected(
                                "GraphJit background sample materialization source is outside its target subset");
                        }
                        auto const storage = storage_for_delivery(
                            result,
                            result.sample_source_storage[*source_subset],
                            background.sample_source_subsets[*source_subset],
                            timing.delivery,
                            storage_kind);
                        if (!storage) {
                            return std::unexpected(
                                "GraphJit background sample materialization has no legal source storage");
                        }
                        local_source_index[source_index] =
                            materialization.source_channels.size();
                        materialization.source_subsets.push_back(*source_subset);
                        materialization.inputs.push_back(
                            *storage);
                        materialization.source_channels.push_back(timing.source);
                        materialization.source_read_latencies.push_back(
                            timing.read_latency);
                    }
                    for (auto const* projection : projections) {
                        SampleProjectionPlan retained{
                            .source_type = projection->source_type,
                            .target_type = projection->target_type,
                            .target_channels = projection->target_channels,
                        };
                        retained.source_channel_indices.reserve(
                            projection->source_channel_indices.size());
                        for (auto const source_index :
                             projection->source_channel_indices) {
                            if (source_index >= local_source_index.size()
                                || !local_source_index[source_index]) {
                                return std::unexpected(
                                    "GraphJit background sample projection references a source outside its target subset");
                            }
                            retained.source_channel_indices.push_back(
                                *local_source_index[source_index]);
                        }
                        materialization.projections.push_back(
                            std::move(retained));
                    }
                    auto const materialization_index =
                        append_sample_materialization(
                            std::move(materialization),
                            target_subset_index,
                            connection_index);
                    append_unique(
                        storage_connection.sample_materializations,
                        materialization_index);
                }
            }
            continue;
        }

        auto const* connection = event_connection_for(
            connections, background_connection);
        if (!connection) {
            return std::unexpected(
                "GraphJit background event storage planning lost its normalized connection");
        }
        for (auto const target_subset_index : background_connection.target_subsets) {
            if (target_subset_index >= background.event_target_subsets.size()) {
                return std::unexpected(
                    "GraphJit background event storage planning has an invalid target subset");
            }
            auto const& target_subset =
                background.event_target_subsets[target_subset_index];
            std::vector<std::size_t> deliveries;
            for (std::size_t delivery = 0;
                 delivery < background_connection.event_deliveries.size(); ++delivery) {
                if (background_connection.event_deliveries[delivery].target
                    == target_subset.port) {
                    deliveries.push_back(delivery);
                }
            }
            auto const direct = deliveries.size() == 1
                && !background_connection.requires_conversion
                && background_connection.event_source_type
                    == background_connection.event_target_type;
            if (direct) {
                auto const& delivery =
                    background_connection.event_deliveries[deliveries.front()];
                auto const source_subset = event_source_subset_for(
                    background, background_connection, delivery.source);
                if (!source_subset) {
                    return std::unexpected(
                        "GraphJit background event direct source lost its source subset");
                }
                auto const& subset = background.event_source_subsets[*source_subset];
                std::vector<PortStorageIndex> selected;
                if (target_subset.access
                    == PlannedDestinationAccess::random_access
                    && subset.retention == OutputRetention::ephemeral) {
                    for (auto const storage_kind : {
                             PortStorageKind::tick_random_access,
                             PortStorageKind::background}) {
                        if (auto const storage = storage_for_delivery(
                                result,
                                result.event_source_storage[*source_subset],
                                subset,
                                delivery.mechanism,
                                storage_kind)) {
                            append_unique(selected, *storage);
                        }
                    }
                } else {
                    auto const context = target_subset.access
                            == PlannedDestinationAccess::random_access
                        ? PortStorageKind::tick_random_access
                        : PortStorageKind::tick_sequential;
                    if (auto const storage = storage_for_delivery(
                            result,
                            result.event_source_storage[*source_subset],
                            subset,
                            delivery.mechanism,
                            context)) {
                        selected.push_back(*storage);
                    }
                }
                if (target_subset.access
                        == PlannedDestinationAccess::sequential
                    && target_is_replay_input(
                        background,
                        NodeBundlePortId{
                            target_subset.port.bundle,
                            PortKind::event,
                            target_subset.port.port})) {
                    if (auto const replay_storage =
                            storage_for_delivery(
                                result,
                                result.event_source_storage[*source_subset],
                                subset,
                                delivery.mechanism,
                                PortStorageKind::
                                    background)) {
                        append_unique(selected, *replay_storage);
                    }
                }
                if (selected.empty()) {
                    return std::unexpected(
                        "GraphJit background event direct source has no legal storage");
                }
                auto const target_plan = std::ranges::find_if(
                    connection->target_plans,
                    [&](EventTargetPlan const& candidate) {
                        return candidate.target == target_subset.port;
                    });
                auto const target_history =
                    target_plan == connection->target_plans.end()
                    ? std::size_t{0}
                    : target_plan->history;
                for (auto const storage : selected) {
                    auto const direct_index = result.direct_events.size();
                    result.direct_events.push_back(
                        DirectEventConnectionPlan{
                            .connection = connection_index,
                            .target_subset = target_subset_index,
                            .source_subset = *source_subset,
                            .storage = storage,
                            .delivery = delivery.mechanism,
                            .target_history = target_history,
                        });
                    storage_connection.direct_events.push_back(direct_index);
                    append_unique(
                        result.event_target_storage[target_subset_index],
                        storage);
                }
                continue;
            }

            std::vector<PortStorageKind> storage_kinds;
            if (target_subset.access == PlannedDestinationAccess::random_access) {
                storage_kinds = {
                    PortStorageKind::tick_random_access,
                    PortStorageKind::background,
                };
            } else {
                auto const has_current = std::ranges::any_of(
                    deliveries,
                    [&](std::size_t delivery) {
                        return background_connection.event_deliveries[delivery].mechanism
                            == PlannedDeliveryMechanism::tick_to_sequential;
                    });
                storage_kinds.push_back(
                    has_current
                        ? PortStorageKind::current_tick
                        : PortStorageKind::tick_sequential);
                if (target_is_replay_input(
                        background,
                        NodeBundlePortId{
                            target_subset.port.bundle,
                            PortKind::event,
                            target_subset.port.port})) {
                    append_unique(
                        storage_kinds,
                        PortStorageKind::
                            background);
                }
            }

            for (auto const storage_kind : storage_kinds) {
                EventMaterializationPlan materialization{
                    .storage = storage_kind,
                    .source_type = background_connection.event_source_type,
                    .target_type = background_connection.event_target_type,
                    .conversion = background_connection.event_conversion,
                };
                double max_events_per_index = 0.0;
                for (auto const delivery_index : deliveries) {
                    auto const& delivery =
                        background_connection.event_deliveries[delivery_index];
                    auto const source_subset = event_source_subset_for(
                        background, background_connection, delivery.source);
                    if (!source_subset) {
                        return std::unexpected(
                            "GraphJit background event materialization lost its source subset");
                    }
                    auto const& subset = background.event_source_subsets[*source_subset];
                    auto const storage = storage_for_delivery(
                        result,
                        result.event_source_storage[*source_subset],
                        subset,
                        delivery.mechanism,
                        storage_kind);
                    if (!storage) {
                        return std::unexpected(
                            "GraphJit background event materialization has no legal source storage");
                    }
                    materialization.source_subsets.push_back(*source_subset);
                    materialization.inputs.push_back(
                        *storage);
                    if (!is_valid_event_buffer_rate(subset.max_events_per_index)
                        || !is_valid_event_buffer_rate(
                            max_events_per_index
                            + subset.max_events_per_index)) {
                        return std::unexpected(
                            "GraphJit background event materialization rate is not representable");
                    }
                    max_events_per_index += subset.max_events_per_index;
                }
                auto const target_plan = std::ranges::find_if(
                    connection->target_plans,
                    [&](EventTargetPlan const& candidate) {
                        return candidate.target == target_subset.port;
                    });
                if (target_plan != connection->target_plans.end()) {
                    materialization.target_history = target_plan->history;
                }
                auto const materialization_index =
                    append_event_materialization(
                        std::move(materialization),
                        target_subset_index,
                        connection_index,
                        max_events_per_index);
                append_unique(
                    storage_connection.event_materializations,
                    materialization_index);
            }
        }
    }

    for (auto& storage : result.ports) {
        std::ranges::sort(storage.sample_channels);
        std::ranges::sort(storage.source_subsets);
        std::ranges::sort(storage.target_subsets);
        std::ranges::sort(storage.connections);
    }
    return result;
}

std::expected<void, std::string> validate_background_runtime_plan(
    BackgroundEvaluationPlan const& plan)
{
    auto const& runtime = plan.runtime;
    if (runtime.port_bindings.size() != plan.ports.size()
        || runtime.node_operations.size() != plan.nodes.size()
        || runtime.node_replay_invocations.size() != plan.nodes.size()) {
        return std::unexpected(
            "GraphJit background runtime maps are not aligned with the evaluation plan");
    }

    auto binding_required = [&](BackgroundPortIndex port_index) {
        auto const& port = plan.ports[port_index];
        if (port.node >= plan.nodes.size()) return false;
        auto const& node = plan.nodes[port.node];
        return port.direction == PortDirection::input
            ? node.authored_tock_execution
                ? port.random_access_input
                : node.replays_tick && port.replay_sequential_input
            : node.authored_tock_execution
                ? port.authored_tock_output
                : node.replays_tick && port.replayed_tick_output;
    };
    std::vector<bool> seen_bindings(runtime.bindings.size(), false);
    for (BackgroundPortIndex port_index = 0;
         port_index < plan.ports.size(); ++port_index) {
        auto const slot = runtime.port_bindings[port_index];
        if (binding_required(port_index) != slot.has_value()) {
            return std::unexpected(
                "GraphJit background runtime binding map is incomplete");
        }
        if (!slot) continue;
        if (*slot >= runtime.bindings.size() || seen_bindings[*slot]) {
            return std::unexpected(
                "GraphJit background runtime binding slot is invalid or duplicated");
        }
        seen_bindings[*slot] = true;
        auto const& binding = runtime.bindings[*slot];
        auto const& port = plan.ports[port_index];
        if (binding.port != port_index
            || binding.node != port.node
            || binding.kind != port.kind
            || binding.direction != port.direction
            || binding.storage.empty()) {
            return std::unexpected(
                "GraphJit background runtime binding does not match its logical port");
        }
        for (auto const storage_index : binding.storage) {
            if (storage_index >= plan.storage.ports.size()
                || plan.storage.ports[storage_index].kind != binding.kind
                || (binding.direction == PortDirection::input
                    && plan.storage.ports[storage_index].storage
                        != PortStorageKind::background
                    && plan.storage.ports[storage_index].storage
                        != PortStorageKind::persisted_pages)
                || (binding.direction == PortDirection::output
                    && plan.storage.ports[storage_index].storage
                        == PortStorageKind::current_tick)) {
                return std::unexpected(
                    "GraphJit background runtime binding references incompatible storage");
            }
        }
    }
    if (!std::ranges::all_of(seen_bindings, [](bool seen) { return seen; })) {
        return std::unexpected(
            "GraphJit background runtime has an unreachable binding slot");
    }

    for (auto const& storage : plan.storage.ports) {
        if (std::ranges::any_of(
                storage.inputs,
                [&](PortStorageIndex input) {
                    return input >= plan.storage.ports.size();
                })) {
            return std::unexpected(
                "GraphJit background storage input index is out of range");
        }
        if (storage.storage != PortStorageKind::persisted_pages) continue;
        if (!storage.output_port || *storage.output_port >= plan.ports.size()) {
            return std::unexpected(
                "GraphJit persisted background storage has no output identity");
        }
        auto const& port = plan.ports[*storage.output_port];
        if (port.direction != PortDirection::output) {
            return std::unexpected(
                "GraphJit persisted background storage identity is not an output");
        }
        // stable_identity is preferred across generations; output_port is the
        // explicit generation-local identity for anonymous concrete outputs.
    }
    for (auto const& materialization : plan.storage.sample_materializations) {
        if (materialization.output >= plan.storage.ports.size()
            || std::ranges::any_of(
                materialization.inputs,
                [&](PortStorageIndex input) {
                    return input >= plan.storage.ports.size();
                })) {
            return std::unexpected(
                "GraphJit sample materialization references storage out of range");
        }
    }
    for (auto const& materialization : plan.storage.event_materializations) {
        if (materialization.output >= plan.storage.ports.size()
            || std::ranges::any_of(
                materialization.inputs,
                [&](PortStorageIndex input) {
                    return input >= plan.storage.ports.size();
                })) {
            return std::unexpected(
                "GraphJit event materialization references storage out of range");
        }
    }
    for (auto const& direct : plan.storage.direct_samples) {
        if (direct.storage >= plan.storage.ports.size()
            || direct.connection >= plan.connections.size()) {
            return std::unexpected(
                "GraphJit direct sample operation references an invalid plan index");
        }
    }
    for (auto const& direct : plan.storage.direct_events) {
        if (direct.storage >= plan.storage.ports.size()
            || direct.connection >= plan.connections.size()) {
            return std::unexpected(
                "GraphJit direct event operation references an invalid plan index");
        }
    }

    std::vector<std::optional<std::size_t>> execution_position(
        plan.nodes.size());
    for (std::size_t position = 0;
         position < plan.background_evaluation_order.size(); ++position) {
        auto const node = plan.background_evaluation_order[position];
        if (node >= plan.nodes.size() || execution_position[node]) {
            return std::unexpected(
                "GraphJit background evaluation order contains an invalid node");
        }
        execution_position[node] = position;
    }
    auto operation_connections = [&](BackgroundRuntimeOperationPlan const& operation)
        -> std::span<BackgroundConnectionIndex const> {
        return operation.connections;
    };
    std::vector<std::size_t> placement_count(runtime.operations.size(), 0);
    for (BackgroundNodeIndex node = 0;
         node < runtime.node_operations.size(); ++node) {
        auto validate_placements = [&](std::span<BackgroundRuntimeOperationIndex const> placed,
                                       bool before)
            -> std::expected<void, std::string> {
            if (!execution_position[node] && !placed.empty()) {
                return std::unexpected(
                    "GraphJit runtime operation is placed around a non-executable node");
            }
            for (auto const operation_index : placed) {
                if (operation_index >= runtime.operations.size()) {
                    return std::unexpected(
                        "GraphJit runtime operation placement is out of range");
                }
                ++placement_count[operation_index];
                auto const position = *execution_position[node];
                for (auto const connection_index :
                     operation_connections(runtime.operations[operation_index])) {
                    if (connection_index >= plan.connections.size()) {
                        return std::unexpected(
                            "GraphJit runtime operation references a missing connection");
                    }
                    auto const& connection = plan.connections[connection_index];
                    for (auto const port_index : connection.source_coverage_ports) {
                        if (port_index >= plan.ports.size()) {
                            return std::unexpected(
                                "GraphJit runtime operation source port is out of range");
                        }
                        auto const source = plan.ports[port_index].node;
                        if (source >= execution_position.size()
                            || !execution_position[source]) continue;
                        if ((before && *execution_position[source] >= position)
                            || (!before && *execution_position[source] > position)) {
                            return std::unexpected(
                                "GraphJit runtime operation is placed before its producer");
                        }
                    }
                    for (auto const port_index : connection.target_coverage_ports) {
                        if (port_index >= plan.ports.size()) {
                            return std::unexpected(
                                "GraphJit runtime operation target port is out of range");
                        }
                        auto const target = plan.ports[port_index].node;
                        if (target >= execution_position.size()
                            || !execution_position[target]) continue;
                        if ((before && *execution_position[target] < position)
                            || (!before && *execution_position[target] <= position)) {
                            return std::unexpected(
                                "GraphJit runtime operation is placed after its consumer");
                        }
                    }
                }
            }
            return {};
        };
        if (auto valid = validate_placements(
                runtime.node_operations[node].before, true); !valid) {
            return valid;
        }
        if (auto valid = validate_placements(
                runtime.node_operations[node].after, false); !valid) {
            return valid;
        }
    }
    if (std::ranges::any_of(
            placement_count, [](std::size_t count) { return count != 1; })) {
        return std::unexpected(
            "GraphJit runtime operation does not have exactly one placement");
    }
    for (auto const& operation : runtime.operations) {
        auto valid_index = [&] {
            switch (operation.kind) {
            case BackgroundRuntimeOperationKind::direct_sample:
                return operation.operation < plan.storage.direct_samples.size()
                    && operation.connections
                        == std::vector<BackgroundConnectionIndex>{
                            plan.storage.direct_samples[operation.operation]
                                .connection};
            case BackgroundRuntimeOperationKind::direct_event:
                return operation.operation < plan.storage.direct_events.size()
                    && operation.connections
                        == std::vector<BackgroundConnectionIndex>{
                            plan.storage.direct_events[operation.operation]
                                .connection};
            case BackgroundRuntimeOperationKind::sample_materialization:
                return operation.operation
                        < plan.storage.sample_materializations.size()
                    && operation.connections
                        == plan.storage.sample_materializations[
                            operation.operation].connections;
            case BackgroundRuntimeOperationKind::event_materialization:
                return operation.operation
                        < plan.storage.event_materializations.size()
                    && operation.connections
                        == plan.storage.event_materializations[
                            operation.operation].connections;
            }
            return false;
        }();
        if (!valid_index) {
            return std::unexpected(
                "GraphJit runtime operation references a missing storage operation");
        }
    }

    std::vector<bool> seen_replay(runtime.replay_invocations.size(), false);
    for (BackgroundNodeIndex node = 0; node < plan.nodes.size(); ++node) {
        auto const slot = runtime.node_replay_invocations[node];
        if (plan.nodes[node].replays_tick != slot.has_value()) {
            return std::unexpected(
                "GraphJit replay invocation map is incomplete");
        }
        if (!slot) continue;
        if (*slot >= runtime.replay_invocations.size() || seen_replay[*slot]) {
            return std::unexpected(
                "GraphJit replay invocation slot is invalid or duplicated");
        }
        seen_replay[*slot] = true;
        auto const& replay = runtime.replay_invocations[*slot];
        if (replay.node != node
            || replay.maximum_block_size == 0
            || replay.maximum_block_size
                != plan.nodes[node].replay_maximum_block_size) {
            return std::unexpected(
                "GraphJit replay invocation has an invalid block constraint");
        }
        auto validate_replay_bindings = [&](std::span<BackgroundBindingSlot const> slots,
                                            PortDirection direction) {
            return std::ranges::all_of(slots, [&](BackgroundBindingSlot binding) {
                return binding < runtime.bindings.size()
                    && runtime.bindings[binding].node == node
                    && runtime.bindings[binding].direction == direction;
            });
        };
        if (!validate_replay_bindings(
                replay.input_bindings, PortDirection::input)
            || !validate_replay_bindings(
                replay.output_bindings, PortDirection::output)) {
            return std::unexpected(
                "GraphJit replay invocation references an incompatible binding slot");
        }
    }
    if (!std::ranges::all_of(seen_replay, [](bool seen) { return seen; })) {
        return std::unexpected(
            "GraphJit background runtime has an unreachable replay slot");
    }
    return {};
}

std::expected<void, std::string> finalize_background_runtime_plan(
    BackgroundEvaluationPlan& plan)
{
    auto& runtime = plan.runtime;
    runtime = {};
    runtime.port_bindings.resize(plan.ports.size());
    runtime.node_operations.resize(plan.nodes.size());
    runtime.node_replay_invocations.resize(plan.nodes.size());

    auto append_unique_storage = [](std::vector<PortStorageIndex>& target,
                                    std::span<PortStorageIndex const> source,
                                    PortDirection direction,
                                    BackgroundStoragePlan const& storage) {
        for (auto const index : source) {
            if (index >= storage.ports.size()) continue;
            auto const kind = storage.ports[index].storage;
            auto const compatible = direction == PortDirection::input
                ? kind == PortStorageKind::background
                    || kind == PortStorageKind::persisted_pages
                : kind != PortStorageKind::current_tick;
            if (compatible && !std::ranges::contains(target, index)) {
                target.push_back(index);
            }
        }
    };
    auto binding_required = [&](BackgroundPortPlan const& port) {
        if (port.node >= plan.nodes.size()) return false;
        auto const& node = plan.nodes[port.node];
        return port.direction == PortDirection::input
            ? node.authored_tock_execution
                ? port.random_access_input
                : node.replays_tick && port.replay_sequential_input
            : node.authored_tock_execution
                ? port.authored_tock_output
                : node.replays_tick && port.replayed_tick_output;
    };
    for (BackgroundPortIndex port_index = 0;
         port_index < plan.ports.size(); ++port_index) {
        auto const& port = plan.ports[port_index];
        if (!binding_required(port)) continue;
        BackgroundRuntimeBindingPlan binding{
            .node = port.node,
            .port = port_index,
            .kind = port.kind,
            .direction = port.direction,
        };
        if (port.kind == PortKind::sample) {
            if (port.direction == PortDirection::output) {
                for (PortSubsetIndex subset = 0;
                     subset < plan.sample_source_subsets.size(); ++subset) {
                    if (plan.sample_source_subsets[subset].port
                        != port.configured_port) continue;
                    append_unique_storage(
                        binding.storage,
                        plan.storage.sample_source_storage[subset],
                        port.direction,
                        plan.storage);
                }
            } else {
                for (PortSubsetIndex subset = 0;
                     subset < plan.sample_target_subsets.size(); ++subset) {
                    if (plan.sample_target_subsets[subset].port
                        != port.configured_port) continue;
                    append_unique_storage(
                        binding.storage,
                        plan.storage.sample_target_storage[subset],
                        port.direction,
                        plan.storage);
                }
            }
        } else if (port.direction == PortDirection::output) {
            for (PortSubsetIndex subset = 0;
                 subset < plan.event_source_subsets.size(); ++subset) {
                auto const& source = plan.event_source_subsets[subset];
                if (source.port.bundle != port.configured_port.node_bundle_handle
                    || source.port.port != port.configured_port.port_index) continue;
                append_unique_storage(
                    binding.storage,
                    plan.storage.event_source_storage[subset],
                    port.direction,
                    plan.storage);
            }
        } else {
            for (PortSubsetIndex subset = 0;
                 subset < plan.event_target_subsets.size(); ++subset) {
                auto const& target = plan.event_target_subsets[subset];
                if (target.port.bundle != port.configured_port.node_bundle_handle
                    || target.port.port != port.configured_port.port_index) continue;
                append_unique_storage(
                    binding.storage,
                    plan.storage.event_target_storage[subset],
                    port.direction,
                    plan.storage);
            }
        }
        std::ranges::sort(binding.storage);
        auto const slot = runtime.bindings.size();
        runtime.port_bindings[port_index] = slot;
        runtime.bindings.push_back(std::move(binding));
    }

    std::vector<std::optional<std::size_t>> execution_position(
        plan.nodes.size());
    for (std::size_t position = 0;
         position < plan.background_evaluation_order.size(); ++position) {
        auto const node = plan.background_evaluation_order[position];
        if (node < execution_position.size()) execution_position[node] = position;
    }
    auto source_and_target_positions = [&](std::span<BackgroundConnectionIndex const> connections) {
        std::optional<std::pair<std::size_t, BackgroundNodeIndex>> latest_source;
        std::optional<std::pair<std::size_t, BackgroundNodeIndex>> earliest_target;
        for (auto const connection_index : connections) {
            if (connection_index >= plan.connections.size()) continue;
            auto const& connection = plan.connections[connection_index];
            for (auto const port_index : connection.source_coverage_ports) {
                if (port_index >= plan.ports.size()) continue;
                auto const node = plan.ports[port_index].node;
                if (node >= execution_position.size()
                    || !execution_position[node]) continue;
                auto const candidate = std::pair{*execution_position[node], node};
                if (!latest_source || candidate.first > latest_source->first) {
                    latest_source = candidate;
                }
            }
            for (auto const port_index : connection.target_coverage_ports) {
                if (port_index >= plan.ports.size()) continue;
                auto const node = plan.ports[port_index].node;
                if (node >= execution_position.size()
                    || !execution_position[node]) continue;
                auto const candidate = std::pair{*execution_position[node], node};
                if (!earliest_target || candidate.first < earliest_target->first) {
                    earliest_target = candidate;
                }
            }
        }
        return std::pair{latest_source, earliest_target};
    };
    auto append_operation = [&](BackgroundRuntimeOperationKind kind,
                                std::size_t operation,
                                PortStorageKind storage_kind,
                                std::vector<BackgroundConnectionIndex> connections) {
        if (storage_kind == PortStorageKind::current_tick) return;
        auto const [latest_source, earliest_target] =
            source_and_target_positions(connections);
        auto const place_before = storage_kind == PortStorageKind::background
                || storage_kind == PortStorageKind::persisted_pages
            ? earliest_target
            : std::optional<std::pair<std::size_t, BackgroundNodeIndex>>{};
        if (!place_before && !latest_source) return;
        auto const index = runtime.operations.size();
        runtime.operations.push_back(BackgroundRuntimeOperationPlan{
            .kind = kind,
            .operation = operation,
            .connections = std::move(connections),
        });
        if (place_before) {
            runtime.node_operations[place_before->second].before.push_back(index);
        } else {
            runtime.node_operations[latest_source->second].after.push_back(index);
        }
    };
    for (std::size_t index = 0;
         index < plan.storage.direct_samples.size(); ++index) {
        auto const& direct = plan.storage.direct_samples[index];
        if (direct.storage >= plan.storage.ports.size()) continue;
        append_operation(
            BackgroundRuntimeOperationKind::direct_sample,
            index,
            plan.storage.ports[direct.storage].storage,
            {direct.connection});
    }
    for (std::size_t index = 0;
         index < plan.storage.direct_events.size(); ++index) {
        auto const& direct = plan.storage.direct_events[index];
        if (direct.storage >= plan.storage.ports.size()) continue;
        append_operation(
            BackgroundRuntimeOperationKind::direct_event,
            index,
            plan.storage.ports[direct.storage].storage,
            {direct.connection});
    }
    for (std::size_t index = 0;
         index < plan.storage.sample_materializations.size(); ++index) {
        auto const& materialization = plan.storage.sample_materializations[index];
        append_operation(
            BackgroundRuntimeOperationKind::sample_materialization,
            index,
            materialization.storage,
            materialization.connections);
    }
    for (std::size_t index = 0;
         index < plan.storage.event_materializations.size(); ++index) {
        auto const& materialization = plan.storage.event_materializations[index];
        append_operation(
            BackgroundRuntimeOperationKind::event_materialization,
            index,
            materialization.storage,
            materialization.connections);
    }

    for (BackgroundNodeIndex node = 0; node < plan.nodes.size(); ++node) {
        if (!plan.nodes[node].replays_tick) continue;
        BackgroundReplayInvocationPlan replay{
            .node = node,
            .maximum_block_size = plan.nodes[node].replay_maximum_block_size,
        };
        for (auto const port_index : plan.nodes[node].inputs) {
            if (port_index < runtime.port_bindings.size()
                && runtime.port_bindings[port_index]) {
                replay.input_bindings.push_back(
                    *runtime.port_bindings[port_index]);
            }
        }
        for (auto const port_index : plan.nodes[node].outputs) {
            if (port_index < runtime.port_bindings.size()
                && runtime.port_bindings[port_index]) {
                replay.output_bindings.push_back(
                    *runtime.port_bindings[port_index]);
            }
        }
        auto const slot = runtime.replay_invocations.size();
        runtime.node_replay_invocations[node] = slot;
        runtime.replay_invocations.push_back(std::move(replay));
    }
    return validate_background_runtime_plan(plan);
}

std::expected<void, std::string> validate_tick_runtime_plan(
    BackgroundEvaluationPlan const& plan)
{
    auto const& runtime = plan.tick_runtime;
    if (runtime.nodes.size() != plan.nodes.size()) {
        return std::unexpected(
            "GraphJit Tick runtime node map is not aligned with the evaluation plan");
    }

    auto validate_random_access_binding = [&](
        TickRandomAccessBindingPlan const& binding,
        PortKind kind,
        BackgroundNodeIndex node)
        -> std::expected<void, std::string> {
        if (binding.port >= plan.ports.size()) {
            return std::unexpected(
                "GraphJit Tick runtime binding references a missing port");
        }
        auto const& port = plan.ports[binding.port];
        if (port.node != node || port.kind != kind
            || port.direction != PortDirection::input
            || !port.random_access_input || binding.storage.empty()) {
            return std::unexpected(
                "GraphJit Tick runtime binding does not match its Random Access input");
        }
        for (auto const storage : binding.storage) {
            if (storage >= plan.storage.ports.size()
                || plan.storage.ports[storage].kind != kind
                || (plan.storage.ports[storage].storage
                        != PortStorageKind::persisted_pages
                    && plan.storage.ports[storage].storage
                        != PortStorageKind::tick_random_access)) {
                return std::unexpected(
                    "GraphJit Tick runtime binding references incompatible storage");
            }
        }
        return {};
    };
    auto validate_sequential_binding = [&](
        TickSequentialBindingPlan const& binding,
        PortKind kind,
        BackgroundNodeIndex node)
        -> std::expected<void, std::string> {
        if (binding.port >= plan.ports.size()) {
            return std::unexpected(
                "GraphJit Tick Sequential binding references a missing port");
        }
        auto const& port = plan.ports[binding.port];
        if (port.node != node || port.kind != kind
            || port.direction != PortDirection::input
            || !port.tick_sequential_input || binding.storage.empty()) {
            return std::unexpected(
                "GraphJit Tick Sequential binding does not match its input");
        }
        for (auto const storage : binding.storage) {
            if (storage >= plan.storage.ports.size()
                || plan.storage.ports[storage].kind != kind
                || (plan.storage.ports[storage].storage
                        != PortStorageKind::persisted_pages
                    && plan.storage.ports[storage].storage
                        != PortStorageKind::tick_sequential
                    && plan.storage.ports[storage].storage
                        != PortStorageKind::tick_random_access)) {
                return std::unexpected(
                    "GraphJit Tick Sequential binding references incompatible storage");
            }
        }
        return {};
    };

    std::size_t next_sequential_sample = 0;
    std::size_t next_sequential_event = 0;
    std::size_t next_sample = 0;
    std::size_t next_event = 0;
    for (BackgroundNodeIndex node = 0; node < runtime.nodes.size(); ++node) {
        auto const& invocation = runtime.nodes[node];
        if (invocation.sequential_sample_begin
                > runtime.sequential_sample_inputs.size()
            || invocation.sequential_event_begin
                > runtime.sequential_event_inputs.size()
            || invocation.sequential_sample_begin != next_sequential_sample
            || invocation.sequential_event_begin != next_sequential_event
            || invocation.sequential_sample_count
                > runtime.sequential_sample_inputs.size()
                    - invocation.sequential_sample_begin
            || invocation.sequential_event_count
                > runtime.sequential_event_inputs.size()
                    - invocation.sequential_event_begin
            || invocation.random_access_sample_begin
                > runtime.random_access_sample_inputs.size()
            || invocation.random_access_event_begin
                > runtime.random_access_event_inputs.size()
            || invocation.random_access_sample_begin != next_sample
            || invocation.random_access_event_begin != next_event
            || invocation.random_access_sample_count
                > runtime.random_access_sample_inputs.size()
                    - invocation.random_access_sample_begin
            || invocation.random_access_event_count
                > runtime.random_access_event_inputs.size()
                    - invocation.random_access_event_begin) {
            return std::unexpected(
                "GraphJit Tick runtime binding ranges are not contiguous");
        }
        for (std::size_t slot = next_sequential_sample;
             slot < next_sequential_sample
                    + invocation.sequential_sample_count;
             ++slot) {
            if (auto valid = validate_sequential_binding(
                    runtime.sequential_sample_inputs[slot],
                    PortKind::sample,
                    node);
                !valid) {
                return valid;
            }
        }
        for (std::size_t slot = next_sequential_event;
             slot < next_sequential_event
                    + invocation.sequential_event_count;
             ++slot) {
            if (auto valid = validate_sequential_binding(
                    runtime.sequential_event_inputs[slot],
                    PortKind::event,
                    node);
                !valid) {
                return valid;
            }
        }
        for (std::size_t slot = next_sample;
             slot < next_sample + invocation.random_access_sample_count;
             ++slot) {
            if (auto valid = validate_random_access_binding(
                    runtime.random_access_sample_inputs[slot],
                    PortKind::sample,
                    node);
                !valid) {
                return valid;
            }
        }
        for (std::size_t slot = next_event;
             slot < next_event + invocation.random_access_event_count;
             ++slot) {
            if (auto valid = validate_random_access_binding(
                    runtime.random_access_event_inputs[slot],
                    PortKind::event,
                    node);
                !valid) {
                return valid;
            }
        }
        next_sequential_sample += invocation.sequential_sample_count;
        next_sequential_event += invocation.sequential_event_count;
        next_sample += invocation.random_access_sample_count;
        next_event += invocation.random_access_event_count;
    }
    if (next_sequential_sample != runtime.sequential_sample_inputs.size()
        || next_sequential_event != runtime.sequential_event_inputs.size()
        || next_sample != runtime.random_access_sample_inputs.size()
        || next_event != runtime.random_access_event_inputs.size()) {
        return std::unexpected(
            "GraphJit Tick runtime contains unreachable binding slots");
    }
    return {};
}

std::expected<void, std::string> finalize_tick_runtime_plan(
    BackgroundEvaluationPlan& plan)
{
    auto& runtime = plan.tick_runtime;
    runtime = {};
    runtime.nodes.resize(plan.nodes.size());

    auto append_unique_storage = [&](std::vector<PortStorageIndex>& result,
                                     std::span<PortStorageIndex const> candidates,
                                     bool sequential) {
        for (auto const storage : candidates) {
            if (storage >= plan.storage.ports.size()) continue;
            auto const kind = plan.storage.ports[storage].storage;
            auto const compatible = kind == PortStorageKind::persisted_pages
                || kind == PortStorageKind::tick_random_access
                || (sequential && kind == PortStorageKind::tick_sequential);
            if (compatible
                && !std::ranges::contains(result, storage)) {
                result.push_back(storage);
            }
        }
    };
    auto storage_for_port = [&](BackgroundPortPlan const& port,
                                bool sequential) {
        std::vector<PortStorageIndex> result;
        if (port.kind == PortKind::sample) {
            for (PortSubsetIndex subset = 0;
                 subset < plan.sample_target_subsets.size(); ++subset) {
                if (plan.sample_target_subsets[subset].port
                    != port.configured_port) {
                    continue;
                }
                append_unique_storage(
                    result,
                    plan.storage.sample_target_storage[subset],
                    sequential);
            }
        } else {
            for (PortSubsetIndex subset = 0;
                 subset < plan.event_target_subsets.size(); ++subset) {
                auto const& target = plan.event_target_subsets[subset];
                if (target.port.bundle != port.configured_port.node_bundle_handle
                    || target.port.port != port.configured_port.port_index) {
                    continue;
                }
                append_unique_storage(
                    result,
                    plan.storage.event_target_storage[subset],
                    sequential);
            }
        }
        std::ranges::sort(result, [&](auto left, auto right) {
            return plan.storage.ports[left].storage
                < plan.storage.ports[right].storage;
        });
        return result;
    };
    auto has_current_tick_storage = [&](BackgroundPortPlan const& port) {
        auto contains_current = [&](std::span<PortStorageIndex const> candidates) {
            return std::ranges::any_of(candidates, [&](auto const storage) {
                return storage < plan.storage.ports.size()
                    && plan.storage.ports[storage].storage
                        == PortStorageKind::current_tick;
            });
        };
        if (port.kind == PortKind::sample) {
            for (PortSubsetIndex subset = 0;
                 subset < plan.sample_target_subsets.size(); ++subset) {
                if (plan.sample_target_subsets[subset].port
                        == port.configured_port
                    && contains_current(
                        plan.storage.sample_target_storage[subset])) {
                    return true;
                }
            }
        } else {
            for (PortSubsetIndex subset = 0;
                 subset < plan.event_target_subsets.size(); ++subset) {
                auto const& target = plan.event_target_subsets[subset];
                if (target.port.bundle
                            == port.configured_port.node_bundle_handle
                    && target.port.port == port.configured_port.port_index
                    && contains_current(
                        plan.storage.event_target_storage[subset])) {
                    return true;
                }
            }
        }
        return false;
    };

    for (BackgroundNodeIndex node = 0; node < plan.nodes.size(); ++node) {
        auto& invocation = runtime.nodes[node];
        invocation.sequential_sample_begin =
            runtime.sequential_sample_inputs.size();
        invocation.sequential_event_begin =
            runtime.sequential_event_inputs.size();
        invocation.random_access_sample_begin =
            runtime.random_access_sample_inputs.size();
        invocation.random_access_event_begin =
            runtime.random_access_event_inputs.size();
        for (auto const port_index : plan.nodes[node].inputs) {
            if (port_index >= plan.ports.size()) {
                return std::unexpected(
                    "GraphJit Tick runtime node references a missing input port");
            }
            auto const& port = plan.ports[port_index];
            if (port.tick_sequential_input) {
                auto storage = storage_for_port(port, true);
                if (storage.empty()) {
                    if (has_current_tick_storage(port)) continue;
                    return std::unexpected(
                        "GraphJit Tick Sequential input has no compatible storage");
                }
                TickSequentialBindingPlan binding{
                    .port = port_index,
                    .storage = std::move(storage),
                };
                if (port.kind == PortKind::sample) {
                    runtime.sequential_sample_inputs.push_back(
                        std::move(binding));
                    ++invocation.sequential_sample_count;
                } else {
                    runtime.sequential_event_inputs.push_back(
                        std::move(binding));
                    ++invocation.sequential_event_count;
                }
            }
            if (port.random_access_input) {
                auto storage = storage_for_port(port, false);
                if (storage.empty()) {
                    return std::unexpected(
                        "GraphJit Tick Random Access input has no compatible storage");
                }
                TickRandomAccessBindingPlan binding{
                    .port = port_index,
                    .storage = std::move(storage),
                };
                if (port.kind == PortKind::sample) {
                    runtime.random_access_sample_inputs.push_back(
                        std::move(binding));
                    ++invocation.random_access_sample_count;
                } else {
                    runtime.random_access_event_inputs.push_back(
                        std::move(binding));
                    ++invocation.random_access_event_count;
                }
            }
        }
    }
    return validate_tick_runtime_plan(plan);
}

} // namespace iv::graph_jit::detail
