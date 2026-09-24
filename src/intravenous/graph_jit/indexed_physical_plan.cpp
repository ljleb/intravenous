#include <intravenous/graph_jit/indexed_physical_plan.h>

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

std::optional<IndexedEndpointOrdinal> output_endpoint_for(
    IndexedPlan const& indexed,
    NodeBundlePortId port)
{
    auto const found = std::ranges::find_if(
        indexed.endpoints,
        [&](IndexedEndpointPlan const& endpoint) {
            return endpoint.direction == IndexedEndpointDirection::output
                && endpoint.configured_port == port;
        });
    if (found == indexed.endpoints.end()) return std::nullopt;
    return static_cast<IndexedEndpointOrdinal>(
        std::distance(indexed.endpoints.begin(), found));
}

std::vector<IndexedConnectionOrdinal> connections_for_source_atom(
    IndexedPlan const& indexed,
    PortKind kind,
    EndpointAtomOrdinal atom)
{
    std::vector<IndexedConnectionOrdinal> result;
    for (IndexedConnectionOrdinal connection = 0;
         connection < indexed.connections.size(); ++connection) {
        auto const& candidate = indexed.connections[connection];
        if (candidate.kind == kind
            && std::ranges::contains(candidate.source_atoms, atom)) {
            result.push_back(connection);
        }
    }
    return result;
}

std::optional<EndpointAtomOrdinal> sample_source_atom_for(
    IndexedPlan const& indexed,
    IndexedConnectionPlan const& connection,
    SampleOutputChannelId channel)
{
    for (auto const atom : connection.source_atoms) {
        if (atom < indexed.sample_source_atoms.size()
            && std::ranges::contains(
                indexed.sample_source_atoms[atom].channels, channel)) {
            return atom;
        }
    }
    return std::nullopt;
}

std::optional<EndpointAtomOrdinal> event_source_atom_for(
    IndexedPlan const& indexed,
    IndexedConnectionPlan const& connection,
    EventOutputPortId port)
{
    for (auto const atom : connection.source_atoms) {
        if (atom < indexed.event_source_atoms.size()
            && indexed.event_source_atoms[atom].port == port) {
            return atom;
        }
    }
    return std::nullopt;
}

SampleConnectionPlan const* sample_connection_for(
    ConnectionAnalysisPlan const& connections,
    IndexedConnectionPlan const& indexed)
{
    auto const found = std::ranges::find_if(
        connections.sample_connections,
        [&](SampleConnectionPlan const& connection) {
            return connection.configured_connection_index
                    == indexed.configured_connection_index
                && connection.source_type == indexed.sample_source_type
                && connection.target_type == indexed.sample_target_type
                && connection.source_channels == indexed.sample_source_channels
                && connection.target_channels == indexed.sample_target_channels;
        });
    return found == connections.sample_connections.end() ? nullptr : &*found;
}

EventConnectionPlan const* event_connection_for(
    ConnectionAnalysisPlan const& connections,
    IndexedConnectionPlan const& indexed)
{
    auto const found = std::ranges::find_if(
        connections.event_connections,
        [&](EventConnectionPlan const& connection) {
            if (connection.configured_connection_index
                    != indexed.configured_connection_index
                || connection.source_type != indexed.event_source_type
                || connection.target_type != indexed.event_target_type
                || connection.deliveries.size()
                    != indexed.event_deliveries.size()) {
                return false;
            }
            for (std::size_t delivery = 0;
                 delivery < connection.deliveries.size(); ++delivery) {
                auto const& planned = connection.deliveries[delivery];
                if (planned.source_index >= connection.source_plans.size()
                    || planned.target_index >= connection.target_plans.size()) {
                    return false;
                }
                auto const& retained = indexed.event_deliveries[delivery];
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

std::optional<IndexedRepresentationOrdinal> representation_with_residence(
    IndexedPhysicalPlan const& physical,
    std::span<IndexedRepresentationOrdinal const> representations,
    IndexedRepresentationResidence residence)
{
    auto const found = std::ranges::find_if(
        representations,
        [&](IndexedRepresentationOrdinal representation) {
            return representation < physical.representations.size()
                && physical.representations[representation].residence
                    == residence;
        });
    return found == representations.end()
        ? std::nullopt
        : std::optional<IndexedRepresentationOrdinal>{*found};
}

template<class Atom>
std::optional<IndexedRepresentationOrdinal> representation_for_context(
    IndexedPhysicalPlan const& physical,
    std::span<IndexedRepresentationOrdinal const> representations,
    Atom const& atom,
    PlannedDeliveryMechanism delivery,
    IndexedRepresentationResidence output_residence)
{
    // A synthesized replay of an ordinary Sequential edge reads an isolated
    // transaction-local replay result, never the live current-Tick buffer.
    if (output_residence
        == IndexedRepresentationResidence::transaction_local_addressable) {
        if (atom.retention == OutputRetention::persisted) {
            return representation_with_residence(
                physical,
                representations,
                IndexedRepresentationResidence::canonical_persisted_pages);
        }
        return representation_with_residence(
            physical,
            representations,
            IndexedRepresentationResidence::transaction_local_addressable);
    }
    if (atom.retention == OutputRetention::persisted) {
        if (delivery == PlannedDeliveryMechanism::tick_to_sequential) {
            return representation_with_residence(
                physical,
                representations,
                IndexedRepresentationResidence::current_tick);
        }
        return representation_with_residence(
            physical,
            representations,
            IndexedRepresentationResidence::canonical_persisted_pages);
    }
    if (delivery == PlannedDeliveryMechanism::tick_to_sequential) {
        return representation_with_residence(
            physical,
            representations,
            IndexedRepresentationResidence::current_tick);
    }
    if (output_residence
        == IndexedRepresentationResidence::prepared_addressable_window) {
        return representation_with_residence(
            physical,
            representations,
            IndexedRepresentationResidence::prepared_addressable_window);
    }
    if (auto const addressable = representation_with_residence(
            physical,
            representations,
            IndexedRepresentationResidence::prepared_addressable_window)) {
        return addressable;
    }
    return representation_with_residence(
        physical,
        representations,
        IndexedRepresentationResidence::prepared_sequential_window);
}

bool target_is_replay_input(
    IndexedPlan const& indexed,
    NodeBundlePortId port)
{
    return std::ranges::any_of(
        indexed.endpoints,
        [&](IndexedEndpointPlan const& endpoint) {
            return endpoint.direction == IndexedEndpointDirection::input
                && endpoint.configured_port == port
                && endpoint.replay_sequential_input;
        });
}

bool equal_sample_materialization_operation(
    IndexedSampleMaterializationPlan const& left,
    IndexedSampleMaterializationPlan const& right)
{
    return left.source_atoms == right.source_atoms
        && left.input_representations == right.input_representations
        && left.source_channels == right.source_channels
        && left.source_read_latencies == right.source_read_latencies
        && left.source_type == right.source_type
        && left.target_layout == right.target_layout
        && left.target_channels == right.target_channels
        && left.projections == right.projections
        && left.target_history == right.target_history;
}

bool equal_sample_materialization_key(
    IndexedSampleMaterializationPlan const& left,
    IndexedSampleMaterializationPlan const& right)
{
    return left.residence == right.residence
        && equal_sample_materialization_operation(left, right);
}

bool equal_event_materialization_operation(
    IndexedEventMaterializationPlan const& left,
    IndexedEventMaterializationPlan const& right)
{
    return left.source_atoms == right.source_atoms
        && left.input_representations == right.input_representations
        && left.source_type == right.source_type
        && left.target_type == right.target_type
        && left.conversion == right.conversion
        && left.target_history == right.target_history;
}

bool equal_event_materialization_key(
    IndexedEventMaterializationPlan const& left,
    IndexedEventMaterializationPlan const& right)
{
    return left.residence == right.residence
        && equal_event_materialization_operation(left, right);
}

} // namespace

std::expected<IndexedPhysicalPlan, std::string> build_indexed_physical_plan(
    ConnectionAnalysisPlan const& connections)
{
    auto const& indexed = connections.indexed;
    IndexedPhysicalPlan result;
    result.sample_source_representations.resize(
        indexed.sample_source_atoms.size());
    result.sample_target_representations.resize(
        indexed.sample_target_atoms.size());
    result.event_source_representations.resize(
        indexed.event_source_atoms.size());
    result.event_target_representations.resize(
        indexed.event_target_atoms.size());
    result.connections.resize(indexed.connections.size());

    auto append_representation = [&](IndexedRepresentationPlan representation) {
        auto const ordinal = result.representations.size();
        result.representations.push_back(std::move(representation));
        return static_cast<IndexedRepresentationOrdinal>(ordinal);
    };

    auto append_source_representation = [&] (
        PortKind kind,
        EndpointAtomOrdinal atom,
        NodeBundlePortId port,
        IndexedRepresentationResidence residence,
        ChannelLayout sample_layout,
        std::span<std::size_t const> sample_channels,
        EventTypeId event_type,
        double max_events_per_index,
        bool coalesce_port) {
        auto existing = result.representations.end();
        if (coalesce_port) {
            existing = std::ranges::find_if(
                result.representations,
                [&](IndexedRepresentationPlan const& candidate) {
                    return candidate.kind == kind
                        && candidate.origin == IndexedRepresentationOrigin::source
                        && candidate.residence == residence
                        && candidate.source_port
                        && *candidate.source_port == port;
                });
        }
        IndexedRepresentationOrdinal representation;
        if (existing == result.representations.end()) {
            representation = append_representation(IndexedRepresentationPlan{
                .kind = kind,
                .residence = residence,
                .origin = IndexedRepresentationOrigin::source,
                .source_atoms = {atom},
                .connections = connections_for_source_atom(indexed, kind, atom),
                .source_port = port,
                .output_endpoint = output_endpoint_for(indexed, port),
                .sample_layout = sample_layout,
                .sample_channels = {
                    sample_channels.begin(), sample_channels.end()},
                .event_type = event_type,
                .max_events_per_index = max_events_per_index,
            });
        } else {
            representation = static_cast<IndexedRepresentationOrdinal>(
                std::distance(result.representations.begin(), existing));
            append_unique(existing->source_atoms, atom);
            for (auto const connection :
                 connections_for_source_atom(indexed, kind, atom)) {
                append_unique(existing->connections, connection);
            }
            for (auto const channel : sample_channels) {
                append_unique(existing->sample_channels, channel);
            }
        }
        auto& bindings = kind == PortKind::sample
            ? result.sample_source_representations[atom]
            : result.event_source_representations[atom];
        append_unique(bindings, representation);
    };

    for (EndpointAtomOrdinal atom = 0;
         atom < indexed.sample_source_atoms.size(); ++atom) {
        auto const& source = indexed.sample_source_atoms[atom];
        std::vector<std::size_t> channels;
        channels.reserve(source.channels.size());
        for (auto const channel : source.channels) channels.push_back(channel.channel);
        auto const port_is_indexed = std::ranges::any_of(
            indexed.connections,
            [&](IndexedConnectionPlan const& connection) {
                return connection.kind == PortKind::sample
                    && std::ranges::contains(connection.source_atoms, atom);
            });
        auto append = [&](IndexedRepresentationResidence residence,
                          bool coalesce_port = false) {
            append_source_representation(
                PortKind::sample,
                atom,
                source.port,
                residence,
                source.source_layout,
                channels,
                EventTypeId::empty,
                0.0,
                coalesce_port);
        };
        if (source.capabilities.current_tick_readable && port_is_indexed) {
            append(IndexedRepresentationResidence::current_tick, true);
        }
        if (source.capabilities.canonical_persisted_pages) {
            append(
                IndexedRepresentationResidence::canonical_persisted_pages,
                true);
        }
        if (source.capabilities.prepared_addressable_window) {
            append(IndexedRepresentationResidence::prepared_addressable_window);
        } else if (source.capabilities.prepared_sequential_window) {
            append(IndexedRepresentationResidence::prepared_sequential_window);
        }
        if (source.capabilities.transaction_local_addressable) {
            append(
                IndexedRepresentationResidence::transaction_local_addressable);
        }
    }

    for (EndpointAtomOrdinal atom = 0;
         atom < indexed.event_source_atoms.size(); ++atom) {
        auto const& source = indexed.event_source_atoms[atom];
        NodeBundlePortId const port{
            source.port.bundle, PortKind::event, source.port.port};
        auto const port_is_indexed = std::ranges::any_of(
            indexed.connections,
            [&](IndexedConnectionPlan const& connection) {
                return connection.kind == PortKind::event
                    && std::ranges::contains(connection.source_atoms, atom);
            });
        auto append = [&](IndexedRepresentationResidence residence,
                          bool coalesce_port = false) {
            append_source_representation(
                PortKind::event,
                atom,
                port,
                residence,
                {},
                {},
                source.type,
                source.max_events_per_index,
                coalesce_port);
        };
        if (source.capabilities.current_tick_readable && port_is_indexed) {
            append(IndexedRepresentationResidence::current_tick, true);
        }
        if (source.capabilities.canonical_persisted_pages) {
            append(
                IndexedRepresentationResidence::canonical_persisted_pages,
                true);
        }
        if (source.capabilities.prepared_addressable_window) {
            append(IndexedRepresentationResidence::prepared_addressable_window);
        } else if (source.capabilities.prepared_sequential_window) {
            append(IndexedRepresentationResidence::prepared_sequential_window);
        }
        if (source.capabilities.transaction_local_addressable) {
            append(
                IndexedRepresentationResidence::transaction_local_addressable);
        }
    }

    auto append_sample_materialization = [&] (
        IndexedSampleMaterializationPlan planned,
        EndpointAtomOrdinal target_atom,
        IndexedConnectionOrdinal connection)
        -> IndexedSampleMaterializationOrdinal {
        auto found = std::ranges::find_if(
            result.sample_materializations,
            [&](IndexedSampleMaterializationPlan const& existing) {
                return equal_sample_materialization_key(existing, planned);
            });
        if (found == result.sample_materializations.end()
            && (planned.residence
                    == IndexedRepresentationResidence::prepared_sequential_window
                || planned.residence
                    == IndexedRepresentationResidence::prepared_addressable_window)) {
            auto const subsuming_residence = planned.residence
                    == IndexedRepresentationResidence::prepared_sequential_window
                ? IndexedRepresentationResidence::prepared_addressable_window
                : IndexedRepresentationResidence::prepared_sequential_window;
            found = std::ranges::find_if(
                result.sample_materializations,
                [&](IndexedSampleMaterializationPlan const& existing) {
                    return existing.residence == subsuming_residence
                        && equal_sample_materialization_operation(
                            existing, planned);
                });
            if (found != result.sample_materializations.end()
                && planned.residence
                    == IndexedRepresentationResidence::prepared_addressable_window) {
                // Addressable prepared storage supplies sequential slices too.
                // Promote an earlier sequential-only template so planning is
                // independent of configured connection order.
                found->residence =
                    IndexedRepresentationResidence::prepared_addressable_window;
                result.representations[found->output_representation].residence =
                    IndexedRepresentationResidence::prepared_addressable_window;
            }
        }
        if (found != result.sample_materializations.end()) {
            auto const ordinal = static_cast<IndexedSampleMaterializationOrdinal>(
                std::distance(result.sample_materializations.begin(), found));
            append_unique(found->target_atoms, target_atom);
            append_unique(found->connections, connection);
            auto& representation =
                result.representations[found->output_representation];
            append_unique(representation.target_atoms, target_atom);
            append_unique(representation.connections, connection);
            append_unique(
                result.sample_target_representations[target_atom],
                found->output_representation);
            return ordinal;
        }

        auto const output = append_representation(IndexedRepresentationPlan{
            .kind = PortKind::sample,
            .residence = planned.residence,
            .origin = IndexedRepresentationOrigin::derived,
            .source_atoms = planned.source_atoms,
            .target_atoms = {target_atom},
            .connections = {connection},
            .input_representations = planned.input_representations,
            .sample_layout = planned.target_layout,
            .sample_channels = planned.target_channels,
        });
        planned.output_representation = output;
        planned.target_atoms.push_back(target_atom);
        planned.connections.push_back(connection);
        auto const ordinal = static_cast<IndexedSampleMaterializationOrdinal>(
            result.sample_materializations.size());
        result.sample_materializations.push_back(std::move(planned));
        result.representations[output].sample_materialization = ordinal;
        append_unique(result.sample_target_representations[target_atom], output);
        return ordinal;
    };

    auto append_event_materialization = [&] (
        IndexedEventMaterializationPlan planned,
        EndpointAtomOrdinal target_atom,
        IndexedConnectionOrdinal connection,
        double max_events_per_index)
        -> IndexedEventMaterializationOrdinal {
        auto found = std::ranges::find_if(
            result.event_materializations,
            [&](IndexedEventMaterializationPlan const& existing) {
                return equal_event_materialization_key(existing, planned);
            });
        if (found == result.event_materializations.end()
            && (planned.residence
                    == IndexedRepresentationResidence::prepared_sequential_window
                || planned.residence
                    == IndexedRepresentationResidence::prepared_addressable_window)) {
            auto const subsuming_residence = planned.residence
                    == IndexedRepresentationResidence::prepared_sequential_window
                ? IndexedRepresentationResidence::prepared_addressable_window
                : IndexedRepresentationResidence::prepared_sequential_window;
            found = std::ranges::find_if(
                result.event_materializations,
                [&](IndexedEventMaterializationPlan const& existing) {
                    return existing.residence == subsuming_residence
                        && equal_event_materialization_operation(existing, planned);
                });
            if (found != result.event_materializations.end()
                && planned.residence
                    == IndexedRepresentationResidence::prepared_addressable_window) {
                found->residence =
                    IndexedRepresentationResidence::prepared_addressable_window;
                result.representations[found->output_representation].residence =
                    IndexedRepresentationResidence::prepared_addressable_window;
            }
        }
        if (found != result.event_materializations.end()) {
            auto const ordinal = static_cast<IndexedEventMaterializationOrdinal>(
                std::distance(result.event_materializations.begin(), found));
            append_unique(found->target_atoms, target_atom);
            append_unique(found->connections, connection);
            auto& representation =
                result.representations[found->output_representation];
            append_unique(representation.target_atoms, target_atom);
            append_unique(representation.connections, connection);
            append_unique(
                result.event_target_representations[target_atom],
                found->output_representation);
            return ordinal;
        }

        auto const output = append_representation(IndexedRepresentationPlan{
            .kind = PortKind::event,
            .residence = planned.residence,
            .origin = IndexedRepresentationOrigin::derived,
            .source_atoms = planned.source_atoms,
            .target_atoms = {target_atom},
            .connections = {connection},
            .input_representations = planned.input_representations,
            .event_type = planned.target_type,
            .max_events_per_index = max_events_per_index,
        });
        planned.output_representation = output;
        planned.target_atoms.push_back(target_atom);
        planned.connections.push_back(connection);
        auto const ordinal = static_cast<IndexedEventMaterializationOrdinal>(
            result.event_materializations.size());
        result.event_materializations.push_back(std::move(planned));
        result.representations[output].event_materialization = ordinal;
        append_unique(result.event_target_representations[target_atom], output);
        return ordinal;
    };

    for (IndexedConnectionOrdinal connection_ordinal = 0;
         connection_ordinal < indexed.connections.size(); ++connection_ordinal) {
        auto const& indexed_connection =
            indexed.connections[connection_ordinal];
        auto& physical_connection = result.connections[connection_ordinal];

        if (indexed_connection.kind == PortKind::sample) {
            auto const* connection = sample_connection_for(
                connections, indexed_connection);
            if (!connection) {
                return std::unexpected(
                    "GraphJit indexed sample physical planning lost its normalized connection");
            }
            for (auto const target_atom_ordinal :
                 indexed_connection.target_atoms) {
                if (target_atom_ordinal >= indexed.sample_target_atoms.size()) {
                    return std::unexpected(
                        "GraphJit indexed sample physical planning has an invalid target atom");
                }
                auto const& target_atom =
                    indexed.sample_target_atoms[target_atom_ordinal];
                struct DirectChannel {
                    std::size_t target_channel = 0;
                    std::size_t source_index = 0;
                    EndpointAtomOrdinal source_atom = 0;
                };
                std::vector<DirectChannel> direct_channels;
                bool direct = true;
                for (auto const target_channel_id : target_atom.channels) {
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
                    auto const source_atom = sample_source_atom_for(
                        indexed, indexed_connection, source.source);
                    if (!source_atom) {
                        direct = false;
                        break;
                    }
                    direct_channels.push_back(DirectChannel{
                        .target_channel = target_channel_id.channel,
                        .source_index = *source_index,
                        .source_atom = *source_atom,
                    });
                }

                if (direct) {
                    for (auto const& channel : direct_channels) {
                        auto const& timing =
                            connection->source_channel_timings[channel.source_index];
                        auto const& source_atom =
                            indexed.sample_source_atoms[channel.source_atom];
                        auto const& representations =
                            result.sample_source_representations[channel.source_atom];
                        std::vector<IndexedRepresentationOrdinal> selected;
                        if (target_atom.access
                            == PlannedDestinationAccess::random_access
                            && source_atom.retention
                                == OutputRetention::ephemeral) {
                            for (auto const residence : {
                                     IndexedRepresentationResidence::prepared_addressable_window,
                                     IndexedRepresentationResidence::transaction_local_addressable}) {
                                if (auto const representation =
                                        representation_for_context(
                                            result,
                                            representations,
                                            source_atom,
                                            timing.delivery,
                                            residence)) {
                                    append_unique(selected, *representation);
                                }
                            }
                        } else {
                            auto const context = target_atom.access
                                    == PlannedDestinationAccess::random_access
                                ? IndexedRepresentationResidence::prepared_addressable_window
                                : IndexedRepresentationResidence::prepared_sequential_window;
                            if (auto const representation =
                                    representation_for_context(
                                        result,
                                        representations,
                                        source_atom,
                                        timing.delivery,
                                        context)) {
                                selected.push_back(*representation);
                            }
                        }
                        if (target_atom.access
                                == PlannedDestinationAccess::sequential
                            && target_is_replay_input(
                                indexed, target_atom.port)) {
                            if (auto const replay_representation =
                                    representation_for_context(
                                        result,
                                        representations,
                                        source_atom,
                                        timing.delivery,
                                        IndexedRepresentationResidence::
                                            transaction_local_addressable)) {
                                append_unique(
                                    selected, *replay_representation);
                            }
                        }
                        if (selected.empty()) {
                            return std::unexpected(
                                "GraphJit indexed sample direct binding has no legal source representation");
                        }
                        for (auto const representation : selected) {
                            auto const binding = result.sample_direct_bindings.size();
                            result.sample_direct_bindings.push_back(
                                IndexedSampleDirectBindingPlan{
                                    .connection = connection_ordinal,
                                    .target_atom = target_atom_ordinal,
                                    .target_channel = channel.target_channel,
                                    .source_atom = channel.source_atom,
                                    .source_channel = timing.source.channel,
                                    .representation = representation,
                                    .delivery = timing.delivery,
                                    .read_latency = timing.read_latency,
                                    .target_history = connection->target_history,
                                });
                            physical_connection.sample_direct_bindings.push_back(
                                binding);
                            append_unique(
                                result.sample_target_representations[
                                    target_atom_ordinal],
                                representation);
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
                                target_atom.channels,
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
                                "GraphJit indexed sample projection spans multiple target atoms");
                        }
                        projections.push_back(&projection);
                        for (auto const source_index :
                             projection.source_channel_indices) {
                            if (source_index
                                >= connection->source_channel_timings.size()) {
                                return std::unexpected(
                                    "GraphJit indexed sample projection has an invalid source index");
                            }
                            append_unique(source_indices, source_index);
                        }
                    }
                    if (projections.empty()) {
                        return std::unexpected(
                            "GraphJit indexed sample target atom has no projection contribution");
                    }
                }

                std::vector<IndexedRepresentationResidence> residences;
                if (target_atom.access
                    == PlannedDestinationAccess::random_access) {
                    residences = {
                        IndexedRepresentationResidence::prepared_addressable_window,
                        IndexedRepresentationResidence::transaction_local_addressable,
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
                    residences.push_back(
                        has_current
                            ? IndexedRepresentationResidence::current_tick
                            : IndexedRepresentationResidence::prepared_sequential_window);
                    if (target_is_replay_input(indexed, target_atom.port)) {
                        append_unique(
                            residences,
                            IndexedRepresentationResidence::
                                transaction_local_addressable);
                    }
                }

                for (auto const residence : residences) {
                    IndexedSampleMaterializationPlan materialization{
                        .residence = residence,
                        .source_type = connection->source_type,
                        .target_layout = connection->target_layout,
                        .target_history = connection->target_history,
                    };
                    for (auto const channel : target_atom.channels) {
                        materialization.target_channels.push_back(channel.channel);
                    }
                    std::vector<std::optional<std::size_t>> local_source_index(
                        connection->source_channel_timings.size());
                    for (auto const source_index : source_indices) {
                        auto const& timing =
                            connection->source_channel_timings[source_index];
                        auto const source_atom = sample_source_atom_for(
                            indexed, indexed_connection, timing.source);
                        if (!source_atom
                            || !std::ranges::contains(
                                target_atom.source_atoms, *source_atom)) {
                            return std::unexpected(
                                "GraphJit indexed sample materialization source is outside its target atom");
                        }
                        auto const representation = representation_for_context(
                            result,
                            result.sample_source_representations[*source_atom],
                            indexed.sample_source_atoms[*source_atom],
                            timing.delivery,
                            residence);
                        if (!representation) {
                            return std::unexpected(
                                "GraphJit indexed sample materialization has no legal source representation");
                        }
                        local_source_index[source_index] =
                            materialization.source_channels.size();
                        materialization.source_atoms.push_back(*source_atom);
                        materialization.input_representations.push_back(
                            *representation);
                        materialization.source_channels.push_back(timing.source);
                        materialization.source_read_latencies.push_back(
                            timing.read_latency);
                    }
                    for (auto const* projection : projections) {
                        IndexedSampleProjectionPlan retained{
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
                                    "GraphJit indexed sample projection references a source outside its target atom");
                            }
                            retained.source_channel_indices.push_back(
                                *local_source_index[source_index]);
                        }
                        materialization.projections.push_back(
                            std::move(retained));
                    }
                    auto const materialization_ordinal =
                        append_sample_materialization(
                            std::move(materialization),
                            target_atom_ordinal,
                            connection_ordinal);
                    append_unique(
                        physical_connection.sample_materializations,
                        materialization_ordinal);
                }
            }
            continue;
        }

        auto const* connection = event_connection_for(
            connections, indexed_connection);
        if (!connection) {
            return std::unexpected(
                "GraphJit indexed event physical planning lost its normalized connection");
        }
        for (auto const target_atom_ordinal : indexed_connection.target_atoms) {
            if (target_atom_ordinal >= indexed.event_target_atoms.size()) {
                return std::unexpected(
                    "GraphJit indexed event physical planning has an invalid target atom");
            }
            auto const& target_atom =
                indexed.event_target_atoms[target_atom_ordinal];
            std::vector<std::size_t> deliveries;
            for (std::size_t delivery = 0;
                 delivery < indexed_connection.event_deliveries.size(); ++delivery) {
                if (indexed_connection.event_deliveries[delivery].target
                    == target_atom.port) {
                    deliveries.push_back(delivery);
                }
            }
            auto const direct = deliveries.size() == 1
                && !indexed_connection.requires_conversion
                && indexed_connection.event_source_type
                    == indexed_connection.event_target_type;
            if (direct) {
                auto const& delivery =
                    indexed_connection.event_deliveries[deliveries.front()];
                auto const source_atom = event_source_atom_for(
                    indexed, indexed_connection, delivery.source);
                if (!source_atom) {
                    return std::unexpected(
                        "GraphJit indexed event direct binding lost its source atom");
                }
                auto const& atom = indexed.event_source_atoms[*source_atom];
                std::vector<IndexedRepresentationOrdinal> selected;
                if (target_atom.access
                    == PlannedDestinationAccess::random_access
                    && atom.retention == OutputRetention::ephemeral) {
                    for (auto const residence : {
                             IndexedRepresentationResidence::prepared_addressable_window,
                             IndexedRepresentationResidence::transaction_local_addressable}) {
                        if (auto const representation = representation_for_context(
                                result,
                                result.event_source_representations[*source_atom],
                                atom,
                                delivery.mechanism,
                                residence)) {
                            append_unique(selected, *representation);
                        }
                    }
                } else {
                    auto const context = target_atom.access
                            == PlannedDestinationAccess::random_access
                        ? IndexedRepresentationResidence::prepared_addressable_window
                        : IndexedRepresentationResidence::prepared_sequential_window;
                    if (auto const representation = representation_for_context(
                            result,
                            result.event_source_representations[*source_atom],
                            atom,
                            delivery.mechanism,
                            context)) {
                        selected.push_back(*representation);
                    }
                }
                if (target_atom.access
                        == PlannedDestinationAccess::sequential
                    && target_is_replay_input(
                        indexed,
                        NodeBundlePortId{
                            target_atom.port.bundle,
                            PortKind::event,
                            target_atom.port.port})) {
                    if (auto const replay_representation =
                            representation_for_context(
                                result,
                                result.event_source_representations[*source_atom],
                                atom,
                                delivery.mechanism,
                                IndexedRepresentationResidence::
                                    transaction_local_addressable)) {
                        append_unique(selected, *replay_representation);
                    }
                }
                if (selected.empty()) {
                    return std::unexpected(
                        "GraphJit indexed event direct binding has no legal source representation");
                }
                auto const target_plan = std::ranges::find_if(
                    connection->target_plans,
                    [&](EventTargetPlan const& candidate) {
                        return candidate.target == target_atom.port;
                    });
                auto const target_history =
                    target_plan == connection->target_plans.end()
                    ? std::size_t{0}
                    : target_plan->history;
                for (auto const representation : selected) {
                    auto const binding = result.event_direct_bindings.size();
                    result.event_direct_bindings.push_back(
                        IndexedEventDirectBindingPlan{
                            .connection = connection_ordinal,
                            .target_atom = target_atom_ordinal,
                            .source_atom = *source_atom,
                            .representation = representation,
                            .delivery = delivery.mechanism,
                            .target_history = target_history,
                        });
                    physical_connection.event_direct_bindings.push_back(binding);
                    append_unique(
                        result.event_target_representations[target_atom_ordinal],
                        representation);
                }
                continue;
            }

            std::vector<IndexedRepresentationResidence> residences;
            if (target_atom.access == PlannedDestinationAccess::random_access) {
                residences = {
                    IndexedRepresentationResidence::prepared_addressable_window,
                    IndexedRepresentationResidence::transaction_local_addressable,
                };
            } else {
                auto const has_current = std::ranges::any_of(
                    deliveries,
                    [&](std::size_t delivery) {
                        return indexed_connection.event_deliveries[delivery].mechanism
                            == PlannedDeliveryMechanism::tick_to_sequential;
                    });
                residences.push_back(
                    has_current
                        ? IndexedRepresentationResidence::current_tick
                        : IndexedRepresentationResidence::prepared_sequential_window);
                if (target_is_replay_input(
                        indexed,
                        NodeBundlePortId{
                            target_atom.port.bundle,
                            PortKind::event,
                            target_atom.port.port})) {
                    append_unique(
                        residences,
                        IndexedRepresentationResidence::
                            transaction_local_addressable);
                }
            }

            for (auto const residence : residences) {
                IndexedEventMaterializationPlan materialization{
                    .residence = residence,
                    .source_type = indexed_connection.event_source_type,
                    .target_type = indexed_connection.event_target_type,
                    .conversion = indexed_connection.event_conversion,
                };
                double max_events_per_index = 0.0;
                for (auto const delivery_index : deliveries) {
                    auto const& delivery =
                        indexed_connection.event_deliveries[delivery_index];
                    auto const source_atom = event_source_atom_for(
                        indexed, indexed_connection, delivery.source);
                    if (!source_atom) {
                        return std::unexpected(
                            "GraphJit indexed event materialization lost its source atom");
                    }
                    auto const& atom = indexed.event_source_atoms[*source_atom];
                    auto const representation = representation_for_context(
                        result,
                        result.event_source_representations[*source_atom],
                        atom,
                        delivery.mechanism,
                        residence);
                    if (!representation) {
                        return std::unexpected(
                            "GraphJit indexed event materialization has no legal source representation");
                    }
                    materialization.source_atoms.push_back(*source_atom);
                    materialization.input_representations.push_back(
                        *representation);
                    if (!is_valid_event_buffer_rate(atom.max_events_per_index)
                        || !is_valid_event_buffer_rate(
                            max_events_per_index
                            + atom.max_events_per_index)) {
                        return std::unexpected(
                            "GraphJit indexed event materialization rate is not representable");
                    }
                    max_events_per_index += atom.max_events_per_index;
                }
                auto const target_plan = std::ranges::find_if(
                    connection->target_plans,
                    [&](EventTargetPlan const& candidate) {
                        return candidate.target == target_atom.port;
                    });
                if (target_plan != connection->target_plans.end()) {
                    materialization.target_history = target_plan->history;
                }
                auto const materialization_ordinal =
                    append_event_materialization(
                        std::move(materialization),
                        target_atom_ordinal,
                        connection_ordinal,
                        max_events_per_index);
                append_unique(
                    physical_connection.event_materializations,
                    materialization_ordinal);
            }
        }
    }

    for (auto& representation : result.representations) {
        std::ranges::sort(representation.sample_channels);
        std::ranges::sort(representation.source_atoms);
        std::ranges::sort(representation.target_atoms);
        std::ranges::sort(representation.connections);
    }
    return result;
}

} // namespace iv::graph_jit::detail
