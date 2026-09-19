#include <intravenous/graph_jit/sample_physical_plan.h>

#include <intravenous/graph_jit/transient_arena_plan.h>
#include <intravenous/ports.h>
#include <intravenous/sample.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace iv::graph_jit::detail {
namespace {

void initialize_sample_raw_region(
    std::span<std::byte> storage,
    std::span<std::byte const> payload)
{
    if (payload.size() != sizeof(Sample)
        || storage.size() % sizeof(Sample) != 0) {
        throw std::logic_error(
            "GraphJit sample raw-region initializer received invalid storage");
    }
    Sample value{};
    std::memcpy(&value, payload.data(), sizeof(value));
    auto samples = std::span<Sample>{
        reinterpret_cast<Sample*>(storage.data()),
        storage.size() / sizeof(Sample)};
    std::ranges::fill(samples, value);
}

std::vector<std::byte> sample_initialize_payload(Sample value)
{
    static_assert(std::is_trivially_copyable_v<Sample>);
    std::vector<std::byte> payload(sizeof(value));
    std::memcpy(payload.data(), &value, sizeof(value));
    return payload;
}

std::expected<std::size_t, std::string> sample_bytes(
    ChannelLayout layout,
    std::size_t frames,
    bool require_power_of_two)
{
    if (!is_valid_channel_type(layout.channel_type)
        || !is_valid_sample_stream_layout(layout.sample_layout)
        || frames == 0
        || (require_power_of_two && !is_power_of_2(frames))) {
        return std::unexpected(
            "GraphJit sample physical plan has an invalid bounded representation");
    }
    auto const channels = channel_count(layout);
    if (channels == 0
        || frames > std::numeric_limits<std::size_t>::max() / channels) {
        return std::unexpected(
            "GraphJit sample physical sample count overflows size_t");
    }
    auto const count = sample_storage_size(layout, frames);
    if (count > std::numeric_limits<std::size_t>::max() / sizeof(Sample)) {
        return std::unexpected(
            "GraphJit sample physical storage size overflows size_t");
    }
    return count * sizeof(Sample);
}

std::expected<std::size_t, std::string> working_ring_capacity(
    std::size_t kernel_block_size,
    std::size_t retained_frames)
{
    if (retained_frames
        > std::numeric_limits<std::size_t>::max() - kernel_block_size) {
        return std::unexpected(
            "GraphJit sample retained extent overflows ring capacity");
    }
    auto const minimum = kernel_block_size + retained_frames;
    auto const capacity = next_power_of_2(std::max<std::size_t>(1, minimum));
    if (capacity < minimum || !is_power_of_2(capacity)) {
        return std::unexpected(
            "GraphJit sample retained extent exceeds representable ring capacity");
    }
    return capacity;
}

std::string persistent_identity(
    SampleProducerGroupPlan const& group,
    SamplePersistentStorageKind kind,
    std::size_t retained_frames,
    std::size_t frame_capacity)
{
    std::ostringstream out;
    out << "graphjit.sample:" << static_cast<unsigned>(group.source_type) << ':';
    for (auto const source : group.source_channels) {
        out << source.bundle << '.' << source.port << '.' << source.channel << ',';
    }
    if (group.canonical_source_layout) {
        out << ":layout="
            << static_cast<unsigned>(group.canonical_source_layout->channel_type)
            << '.'
            << static_cast<unsigned>(group.canonical_source_layout->sample_layout);
    }
    out << ":kind=" << static_cast<unsigned>(kind)
        << ":retained=" << retained_frames
        << ":capacity=" << frame_capacity;
    return std::move(out).str();
}

std::string feedback_identity(
    SampleProducerGroupPlan const& group,
    SampleConnectionPlan const& connection,
    Sample initial_value,
    std::size_t frame_capacity)
{
    std::ostringstream out;
    out << "graphjit.sample.feedback:source=";
    if (group.source_port) {
        out << group.source_port->node_bundle_handle << '.'
            << group.source_port->port_ordinal;
    } else {
        for (auto const source : group.source_channels) {
            out << source.bundle << '.' << source.port << '.' << source.channel << ',';
        }
    }
    out << ":target=" << connection.target_port.node_bundle_handle << '.'
        << connection.target_port.port_ordinal
        << ":source_layout=";
    if (group.canonical_source_layout) {
        out << static_cast<unsigned>(group.canonical_source_layout->channel_type)
            << '.'
            << static_cast<unsigned>(group.canonical_source_layout->sample_layout);
    } else {
        out << static_cast<unsigned>(connection.source_type);
    }
    out
        << ":latency=" << connection.detach->loop_extra_latency
        << ":initial_bits="
        << std::bit_cast<std::uint32_t>(static_cast<float>(initial_value))
        << ":capacity=" << frame_capacity;
    return std::move(out).str();
}

std::string composition_feedback_identity(
    SampleConnectionPlan const& connection,
    Sample initial_value,
    std::size_t frame_capacity)
{
    std::ostringstream out;
    out << "graphjit.sample.composed_feedback:target="
        << connection.target_port.node_bundle_handle << '.'
        << connection.target_port.port_ordinal
        << ":layout="
        << static_cast<unsigned>(connection.target_layout.channel_type) << '.'
        << static_cast<unsigned>(connection.target_layout.sample_layout)
        << ":contributions=";
    for (auto const& contribution : connection.projection_contributions) {
        out << '[' << static_cast<unsigned>(contribution.source_type) << '>'
            << static_cast<unsigned>(contribution.target_type) << ':';
        for (auto const source_index : contribution.source_channel_indices) {
            if (source_index >= connection.source_channel_timings.size()) {
                out << "invalid,";
                continue;
            }
            auto const& channel = connection.source_channel_timings[source_index];
            out << channel.source.bundle << '.' << channel.source.port << '.'
                << channel.source.channel << '@' << channel.read_latency << ',';
        }
        out << "->";
        for (auto const target_channel : contribution.target_channels)
            out << target_channel << ',';
        out << ']';
    }
    out << ":latency=" << connection.detach->loop_extra_latency
        << ":history=" << connection.target_history
        << ":initial_bits="
        << std::bit_cast<std::uint32_t>(static_cast<float>(initial_value))
        << ":capacity=" << frame_capacity;
    return std::move(out).str();
}

std::string composition_feedback_alignment_identity(
    SampleConnectionPlan const& connection,
    std::size_t contribution_index,
    ChannelLayout source_layout,
    std::size_t minimum_latency,
    std::size_t maximum_latency,
    std::size_t frame_capacity,
    Sample initial_value)
{
    std::ostringstream out;
    out << "graphjit.sample.composed_feedback_alignment:target="
        << connection.target_port.node_bundle_handle << '.'
        << connection.target_port.port_ordinal
        << ":contribution=" << contribution_index
        << ":layout=" << static_cast<unsigned>(source_layout.channel_type) << '.'
        << static_cast<unsigned>(source_layout.sample_layout)
        << ":min_latency=" << minimum_latency
        << ":max_latency=" << maximum_latency
        << ":capacity=" << frame_capacity
        << ":initial_bits="
        << std::bit_cast<std::uint32_t>(static_cast<float>(initial_value));
    return std::move(out).str();
}

} // namespace

std::expected<SamplePhysicalPlan, std::string> build_sample_physical_plan(
    ConnectionAnalysisPlan const& connections,
    std::size_t kernel_block_size)
{
    if (kernel_block_size == 0 || !is_power_of_2(kernel_block_size)) {
        return std::unexpected(
            "GraphJit sample physical plan requires a non-zero power-of-two kernel block size");
    }

    SamplePhysicalPlan plan;
    plan.producer_groups.resize(connections.sample_producer_groups.size());
    plan.connection_representations.resize(connections.sample_connections.size());

    std::vector<TransientArenaAllocationRequest> transient_requests;
    std::vector<std::size_t> transient_representations;
    std::vector<std::optional<std::size_t>> transient_request_for_representation;

    auto target_position = [&](SampleConnectionPlan const& connection,
                               ConnectionLiveIntervalPlan const& fallback) {
        auto const bundle = connection.target_port.node_bundle_handle;
        if (bundle == connections.boundary_bundle && !connections.nodes.empty()) {
            return connections.nodes.size();
        }
        if (bundle < connections.schedule.bundle_execution_position.size()
            && connections.schedule.bundle_execution_position[bundle]) {
            return *connections.schedule.bundle_execution_position[bundle];
        }
        return fallback.end;
    };

    auto source_position = [&](NodeBundlePortId source_port,
                               ConnectionLiveIntervalPlan const& fallback) {
        auto const bundle = source_port.node_bundle_handle;
        if (bundle == connections.boundary_bundle) return std::size_t{0};
        if (bundle < connections.schedule.bundle_execution_position.size()
            && connections.schedule.bundle_execution_position[bundle]) {
            return *connections.schedule.bundle_execution_position[bundle];
        }
        return fallback.begin;
    };

    auto composition_position = [&](SampleConnectionPlan const& connection,
                                    ConnectionLiveIntervalPlan const& fallback) {
        auto position = fallback.begin;
        for (auto const& channel : connection.source_channel_timings) {
            position = std::max(
                position,
                source_position(
                    NodeBundlePortId{
                        channel.source.bundle,
                        PortKind::sample,
                        channel.source.port},
                    fallback));
        }
        return position;
    };

    auto append_representation = [&](SampleRepresentationPlan representation) {
        auto const index = plan.representations.size();
        plan.representations.push_back(std::move(representation));
        transient_request_for_representation.emplace_back(std::nullopt);
        return index;
    };

    auto append_transient_representation = [&](SampleRepresentationPlan representation)
        -> std::expected<std::size_t, std::string> {
        auto bytes = sample_bytes(
            representation.channel_layout, representation.frame_capacity, true);
        if (!bytes) return std::unexpected(std::move(bytes.error()));
        auto const index = append_representation(std::move(representation));
        auto const request_index = transient_requests.size();
        transient_requests.push_back(TransientArenaAllocationRequest{
            .size_bytes = *bytes,
            .alignment = alignof(Sample),
            .live_interval = plan.representations.back().live_interval,
        });
        transient_representations.push_back(index);
        transient_request_for_representation[index] = request_index;
        return index;
    };

    auto update_transient_request = [&](std::size_t representation_index)
        -> std::expected<void, std::string> {
        if (representation_index >= plan.representations.size()
            || representation_index >= transient_request_for_representation.size()
            || !transient_request_for_representation[representation_index]) {
            return std::unexpected(
                "GraphJit sample representation lost its transient request");
        }
        auto bytes = sample_bytes(
            plan.representations[representation_index].channel_layout,
            plan.representations[representation_index].frame_capacity,
            true);
        if (!bytes) return std::unexpected(std::move(bytes.error()));
        auto& request = transient_requests[
            *transient_request_for_representation[representation_index]];
        request.size_bytes = *bytes;
        request.live_interval = plan.representations[representation_index].live_interval;
        return {};
    };

    auto append_persistent_allocation = [&] (
        std::size_t representation_index,
        SampleProducerGroupPlan const& group,
        SamplePersistentStorageKind kind,
        ChannelLayout layout,
        std::size_t retained_frames,
        std::size_t frame_capacity,
        std::string migration_identity = {},
        std::optional<Sample> initialize_value = std::nullopt)
        -> std::expected<std::size_t, std::string> {
        auto const storage_frames = kind == SamplePersistentStorageKind::compact_carry
            ? retained_frames
            : frame_capacity;
        auto bytes = sample_bytes(layout, storage_frames, false);
        if (!bytes) return std::unexpected(std::move(bytes.error()));
        auto const allocation_index = plan.persistent_allocations.size();
        plan.persistent_allocations.push_back(SamplePersistentAllocationPlan{
            .representation_index = representation_index,
            .kind = kind,
            .channel_layout = layout,
            .retained_frames = retained_frames,
            .frame_capacity = frame_capacity,
            .size_bytes = *bytes,
            .alignment = alignof(Sample),
            .migration_identity = migration_identity.empty()
                ? persistent_identity(
                    group, kind, retained_frames, frame_capacity)
                : std::move(migration_identity),
            .initialize_value = initialize_value,
        });
        return allocation_index;
    };

    auto append_synthetic_persistent_allocation = [&] (
        std::size_t representation_index,
        ChannelLayout layout,
        std::size_t retained_frames,
        std::size_t frame_capacity,
        std::string migration_identity,
        std::optional<Sample> initialize_value)
        -> std::expected<std::size_t, std::string> {
        if (migration_identity.empty()) {
            return std::unexpected(
                "GraphJit synthetic sample persistent allocation requires a migration identity");
        }
        auto bytes = sample_bytes(layout, frame_capacity, false);
        if (!bytes) return std::unexpected(std::move(bytes.error()));
        auto const allocation_index = plan.persistent_allocations.size();
        plan.persistent_allocations.push_back(SamplePersistentAllocationPlan{
            .representation_index = representation_index,
            .kind = SamplePersistentStorageKind::ring,
            .channel_layout = layout,
            .retained_frames = retained_frames,
            .frame_capacity = frame_capacity,
            .size_bytes = *bytes,
            .alignment = alignof(Sample),
            .migration_identity = std::move(migration_identity),
            .initialize_value = initialize_value,
        });
        return allocation_index;
    };

    for (std::size_t group_index = 0;
         group_index < connections.sample_producer_groups.size(); ++group_index) {
        auto const& group = connections.sample_producer_groups[group_index];
        if (!group.has_realtime_connections) continue;
        if (!group.implementation) {
            return std::unexpected(
                "GraphJit sample physical plan lost a realtime implementation choice");
        }
        if (!group.canonical_source_layout) {
            return std::unexpected(
                "GraphJit sample physical plan requires a canonical realtime source layout");
        }

        switch (*group.implementation) {
        case SampleConnectionImplementationKind::direct:
        case SampleConnectionImplementationKind::transient_materialization:
        case SampleConnectionImplementationKind::compact_persistent_carry:
        case SampleConnectionImplementationKind::persistent_ring:
            break;
        case SampleConnectionImplementationKind::feedback_ring:
        case SampleConnectionImplementationKind::external_boundary:
            return std::unexpected(
                "GraphJit point-9 sample physical plan does not yet realize feedback or external storage");
        }

        auto const has_feedback_branch = std::ranges::any_of(
            group.connection_indices,
            [&](std::size_t connection_index) {
                return connection_index < connections.sample_connections.size()
                    && connections.sample_connections[connection_index].detach.has_value();
            });
        if (group.live_interval.crosses_kernel_invocations
            && !has_feedback_branch
            && (*group.implementation == SampleConnectionImplementationKind::direct
                || *group.implementation
                    == SampleConnectionImplementationKind::transient_materialization)) {
            return std::unexpected(
                "GraphJit transient sample storage cannot satisfy cross-kernel retained storage");
        }

        auto producer_position = group.source_port
            ? source_position(*group.source_port, group.live_interval)
            : group.live_interval.begin;
        for (auto const connection_index : group.connection_indices) {
            if (connection_index >= connections.sample_connections.size()) {
                return std::unexpected(
                    "GraphJit sample producer group references an invalid connection");
            }
            auto const& connection = connections.sample_connections[connection_index];
            if (connection.access != PlannedConnectionAccess::realtime_to_realtime) {
                continue;
            }
            auto const belongs_to_group = !group.source_port
                || std::ranges::any_of(
                    connection.source_channel_timings,
                    [&](SampleSourceChannelTimingPlan const& channel) {
                        return channel.source.bundle
                                == group.source_port->node_bundle_handle
                            && channel.source.port
                                == group.source_port->port_ordinal;
                    });
            if (!belongs_to_group) {
                return std::unexpected(
                    "GraphJit sample producer group contains a foreign source port");
            }
        }

        ConnectionLiveIntervalPlan canonical_live{
            .begin = producer_position,
            .end = group.connection_indices.empty()
                ? group.live_interval.end
                : producer_position,
            .crosses_kernel_invocations = false,
        };
        for (auto const connection_index : group.connection_indices) {
            auto const& connection = connections.sample_connections[connection_index];
            if (connection.access != PlannedConnectionAccess::realtime_to_realtime) {
                continue;
            }
            auto const canonical_branch = !group.source_port
                || (connection.canonical_source_port
                    && *connection.canonical_source_port == *group.source_port);
            if (canonical_branch && !connection.requires_conversion) {
                canonical_live.end = std::max(
                    canonical_live.end, target_position(connection, group.live_interval));
            } else if (!canonical_branch) {
                canonical_live.end = std::max(
                    canonical_live.end,
                    composition_position(connection, group.live_interval));
            }
        }

        std::size_t canonical_capacity = kernel_block_size;
        if (group.requirements.retained_frames != 0) {
            auto capacity = working_ring_capacity(
                kernel_block_size, group.requirements.retained_frames);
            if (!capacity) return std::unexpected(std::move(capacity.error()));
            canonical_capacity = *capacity;
        }

        // Zero-initialized detached branches may share the producer's home
        // representation when the producer has no independent retained state.
        // Zero is also the fresh NodeStorage value seen by ordinary producer
        // history, so this does not leak branch-local detach initialization back
        // into OutputPort semantics. Different feedback delays remain InputPort
        // read latencies over the same absolute producer timeline. Non-zero detach
        // initialization stays branch-local and therefore keeps an explicit copy.
        bool has_feedback_home = false;
        std::size_t home_feedback_retained_frames = 0;
        std::size_t home_feedback_capacity = 0;
        if (group.requirements.retained_frames == 0
            && (*group.implementation == SampleConnectionImplementationKind::direct
                || *group.implementation
                    == SampleConnectionImplementationKind::transient_materialization)) {
            for (auto const connection_index : group.connection_indices) {
                auto const& connection = connections.sample_connections[connection_index];
                if (connection.access != PlannedConnectionAccess::realtime_to_realtime
                    || !connection.detach
                    || !connection.detach_initial_value
                    || std::bit_cast<std::uint32_t>(
                        static_cast<float>(*connection.detach_initial_value))
                        != std::bit_cast<std::uint32_t>(0.0f)
                    || !group.source_port
                    || !connection.canonical_source_port
                    || *connection.canonical_source_port != *group.source_port
                    || !connection.canonical_source_layout
                    || *connection.canonical_source_layout
                        != *group.canonical_source_layout) {
                    continue;
                }
                auto const latency = connection.detach->loop_extra_latency;
                if (connection.read_latency
                        > std::numeric_limits<std::size_t>::max() - latency
                    || connection.target_history
                        > std::numeric_limits<std::size_t>::max()
                            - latency - connection.read_latency) {
                    return std::unexpected(
                        "GraphJit sample feedback retained extent overflows size_t");
                }
                has_feedback_home = true;
                home_feedback_retained_frames = std::max(
                    home_feedback_retained_frames,
                    latency + connection.read_latency + connection.target_history);
            }
            if (has_feedback_home) {
                auto capacity = working_ring_capacity(
                    kernel_block_size, home_feedback_retained_frames);
                if (!capacity) {
                    return std::unexpected(std::move(capacity.error()));
                }
                home_feedback_capacity = *capacity;
            }
        }

        std::size_t canonical = no_sample_representation;
        if (has_feedback_home) {
            auto const index = append_representation(SampleRepresentationPlan{
                .producer_group_index = group_index,
                .canonical_producer_representation = true,
                .implementation = SampleConnectionImplementationKind::feedback_ring,
                .channel_layout = *group.canonical_source_layout,
                .frame_capacity = home_feedback_capacity,
                .live_interval = group.live_interval,
            });
            auto persistent = append_persistent_allocation(
                index,
                group,
                SamplePersistentStorageKind::ring,
                *group.canonical_source_layout,
                home_feedback_retained_frames,
                home_feedback_capacity,
                persistent_identity(
                    group,
                    SamplePersistentStorageKind::ring,
                    home_feedback_retained_frames,
                    home_feedback_capacity),
                Sample{});
            if (!persistent) return std::unexpected(std::move(persistent.error()));
            plan.representations[index].persistent_allocation = *persistent;
            canonical = index;
        } else if (*group.implementation == SampleConnectionImplementationKind::persistent_ring) {
            auto const index = append_representation(SampleRepresentationPlan{
                .producer_group_index = group_index,
                .canonical_producer_representation = true,
                .implementation = *group.implementation,
                .channel_layout = *group.canonical_source_layout,
                .frame_capacity = canonical_capacity,
                .live_interval = group.live_interval,
            });
            auto persistent = append_persistent_allocation(
                index,
                group,
                SamplePersistentStorageKind::ring,
                *group.canonical_source_layout,
                group.requirements.retained_frames,
                canonical_capacity);
            if (!persistent) return std::unexpected(std::move(persistent.error()));
            plan.representations[index].persistent_allocation = *persistent;
            canonical = index;
        } else {
            auto transient_canonical = append_transient_representation(SampleRepresentationPlan{
                .producer_group_index = group_index,
                .canonical_producer_representation = true,
                .implementation = *group.implementation,
                .channel_layout = *group.canonical_source_layout,
                .frame_capacity = canonical_capacity,
                .live_interval = canonical_live,
            });
            if (!transient_canonical) {
                return std::unexpected(std::move(transient_canonical.error()));
            }
            canonical = *transient_canonical;
            if (*group.implementation
                == SampleConnectionImplementationKind::compact_persistent_carry) {
                if (group.requirements.retained_frames == 0) {
                    return std::unexpected(
                        "GraphJit compact sample carry has no retained frames");
                }
                auto persistent = append_persistent_allocation(
                    canonical,
                    group,
                    SamplePersistentStorageKind::compact_carry,
                    *group.canonical_source_layout,
                    group.requirements.retained_frames,
                    canonical_capacity);
                if (!persistent) {
                    return std::unexpected(std::move(persistent.error()));
                }
                plan.representations[canonical].persistent_allocation = *persistent;
                plan.carry_operations.push_back(SampleCarryOperationPlan{
                    .representation_index = canonical,
                    .persistent_allocation = *persistent,
                    .producer_execution_position = producer_position,
                    .retained_frames = group.requirements.retained_frames,
                });
            }
        }

        if (canonical == no_sample_representation) {
            return std::unexpected(
                "GraphJit sample producer lost its canonical representation");
        }
        plan.producer_groups[group_index] = SampleProducerPhysicalPlan{
            .canonical_representation = canonical,
        };

        struct DerivedKey {
            ChannelLayout target_layout{};
            bool operator==(DerivedKey const&) const = default;
        };
        struct DerivedBranch {
            DerivedKey key{};
            std::size_t representation = no_sample_representation;
            std::size_t materialization = 0;
        };
        std::vector<DerivedBranch> derived;

        for (auto const connection_index : group.connection_indices) {
            auto const& connection = connections.sample_connections[connection_index];
            if (connection.access != PlannedConnectionAccess::realtime_to_realtime) {
                continue;
            }
            if (plan.connection_representations[connection_index]) {
                return std::unexpected(
                    "GraphJit sample connection belongs to multiple producer groups");
            }
            if (connection.detach) continue;
            if (group.source_port
                && (!connection.canonical_source_port
                    || *connection.canonical_source_port != *group.source_port)) {
                continue;
            }
            if (connection.canonical_source_layout
                && *connection.canonical_source_layout
                    != *group.canonical_source_layout) {
                return std::unexpected(
                    "GraphJit sample connection disagrees with its producer canonical layout");
            }
            if (!connection.requires_conversion) {
                if (connection.target_layout != *group.canonical_source_layout) {
                    return std::unexpected(
                        "GraphJit identity sample branch changed channel layout without a conversion");
                }
                plan.connection_representations[connection_index] = canonical;
                continue;
            }

            try {
                (void)ChannelConversionRegistry::plan(
                    *group.canonical_source_layout, connection.target_layout);
            } catch (std::exception const& e) {
                return std::unexpected(
                    "GraphJit sample physical plan could not resolve a channel conversion: "
                    + std::string(e.what()));
            }

            DerivedKey const key{.target_layout = connection.target_layout};
            auto branch = std::ranges::find_if(
                derived,
                [&](DerivedBranch const& candidate) { return candidate.key == key; });
            if (connection.target_history
                > std::numeric_limits<std::size_t>::max()
                    - connection.read_latency) {
                return std::unexpected(
                    "GraphJit converted sample retention overflows size_t");
            }
            auto const retained_before = connection.target_history
                + connection.read_latency;
            auto derived_capacity = working_ring_capacity(
                kernel_block_size, retained_before);
            if (!derived_capacity) {
                return std::unexpected(std::move(derived_capacity.error()));
            }
            if (branch == derived.end()) {
                auto const end = target_position(connection, group.live_interval);
                auto representation = append_transient_representation(
                    SampleRepresentationPlan{
                        .producer_group_index = group_index,
                        .canonical_producer_representation = false,
                        .implementation = SampleConnectionImplementationKind::transient_materialization,
                        .channel_layout = connection.target_layout,
                        .frame_capacity = *derived_capacity,
                        .live_interval = ConnectionLiveIntervalPlan{
                            .begin = canonical_live.begin,
                            .end = std::max(canonical_live.begin, end),
                            .crosses_kernel_invocations = false,
                        },
                    });
                if (!representation) {
                    return std::unexpected(std::move(representation.error()));
                }
                auto const materialization_index = plan.materializations.size();
                plan.materializations.push_back(SampleMaterializationPlan{
                    .source_representation = canonical,
                    .target_representation = *representation,
                    .after_execution_position = canonical_live.begin,
                    .source_layout = *group.canonical_source_layout,
                    .target_layout = connection.target_layout,
                    .retained_before = retained_before,
                    .latest_read_latency = connection.read_latency,
                });
                derived.push_back(DerivedBranch{
                    .key = key,
                    .representation = *representation,
                    .materialization = materialization_index,
                });
                branch = std::prev(derived.end());
            } else {
                auto& representation = plan.representations[branch->representation];
                representation.live_interval.end = std::max(
                    representation.live_interval.end,
                    target_position(connection, group.live_interval));
                auto& materialization = plan.materializations[branch->materialization];
                // The shared derived representation must cover the union of all
                // consumer windows, not merely the branch with the largest
                // latency. A lower-latency sibling extends the window toward
                // newer frames while history/latency can extend it backward.
                materialization.retained_before = std::max(
                    materialization.retained_before, retained_before);
                materialization.latest_read_latency = std::min(
                    materialization.latest_read_latency, connection.read_latency);
                auto widened_capacity = working_ring_capacity(
                    kernel_block_size, materialization.retained_before);
                if (!widened_capacity) {
                    return std::unexpected(std::move(widened_capacity.error()));
                }
                representation.frame_capacity = std::max(
                    representation.frame_capacity, *widened_capacity);
                auto updated = update_transient_request(branch->representation);
                if (!updated) return std::unexpected(std::move(updated.error()));
            }
            plan.connection_representations[connection_index] = branch->representation;
        }

        // Detach transport is branch-local. Keep the producer's canonical
        // representation ordinary, then allocate one persistent absolute-indexed
        // ring in the canonical source layout. Converted consumers derive
        // transient target-layout windows from that ring immediately before they
        // execute, so persistent feedback state stays producer-format and stable.
        for (auto const connection_index : group.connection_indices) {
            auto const& connection = connections.sample_connections[connection_index];
            if (connection.access != PlannedConnectionAccess::realtime_to_realtime
                || !connection.detach) {
                continue;
            }
            if (plan.connection_representations[connection_index]) {
                return std::unexpected(
                    "GraphJit detached sample connection already owns a physical representation");
            }
            // Non-canonical projected/permuted feedback is realized once,
            // after every contributing producer group has been planned, by the
            // composition pass below. Do not allocate one feedback branch per
            // contributing producer here.
            if (!connection.canonical_source_port) continue;
            if (!group.source_port
                || *connection.canonical_source_port != *group.source_port
                || !connection.canonical_source_layout
                || *connection.canonical_source_layout != *group.canonical_source_layout) {
                return std::unexpected(
                    "GraphJit canonical sample feedback lost its producer identity");
            }
            if (!connection.requires_conversion
                && connection.target_layout != *group.canonical_source_layout) {
                return std::unexpected(
                    "GraphJit sample feedback changed channel layout without a conversion");
            }
            if (connection.requires_conversion) {
                try {
                    (void)ChannelConversionRegistry::plan(
                        *group.canonical_source_layout, connection.target_layout);
                } catch (std::exception const& e) {
                    return std::unexpected(
                        "GraphJit sample feedback conversion is unsupported: "
                        + std::string(e.what()));
                }
            }
            if (!connection.detach_initial_value) {
                return std::unexpected(
                    "GraphJit sample feedback lost its resolved initial value");
            }

            auto const latency = connection.detach->loop_extra_latency;
            if (connection.read_latency
                    > std::numeric_limits<std::size_t>::max() - latency
                || connection.target_history
                    > std::numeric_limits<std::size_t>::max()
                        - latency - connection.read_latency) {
                return std::unexpected(
                    "GraphJit sample feedback retained extent overflows size_t");
            }
            auto const retained_frames =
                latency + connection.read_latency + connection.target_history;
            auto ring_capacity = working_ring_capacity(
                kernel_block_size, retained_frames);
            if (!ring_capacity) {
                return std::unexpected(std::move(ring_capacity.error()));
            }
            auto const writes_directly_to_feedback = has_feedback_home
                && std::bit_cast<std::uint32_t>(
                    static_cast<float>(*connection.detach_initial_value))
                    == std::bit_cast<std::uint32_t>(0.0f);
            std::size_t ring_representation = canonical;
            if (writes_directly_to_feedback) {
                auto const& home = plan.representations[canonical];
                if (!home.canonical_producer_representation
                    || home.implementation
                        != SampleConnectionImplementationKind::feedback_ring
                    || home.channel_layout != *group.canonical_source_layout
                    || home.frame_capacity < *ring_capacity
                    || home.persistent_allocation
                        == no_sample_persistent_allocation) {
                    return std::unexpected(
                        "GraphJit sample feedback producer-home representation is inconsistent");
                }
            } else {
                ring_representation = append_representation(
                    SampleRepresentationPlan{
                        .producer_group_index = group_index,
                        .canonical_producer_representation = false,
                        .implementation = SampleConnectionImplementationKind::feedback_ring,
                        .channel_layout = *group.canonical_source_layout,
                        .frame_capacity = *ring_capacity,
                        .live_interval = ConnectionLiveIntervalPlan{
                            .begin = producer_position,
                            .end = target_position(connection, group.live_interval),
                            .crosses_kernel_invocations = true,
                        },
                    });
                auto persistent = append_persistent_allocation(
                    ring_representation,
                    group,
                    SamplePersistentStorageKind::ring,
                    *group.canonical_source_layout,
                    retained_frames,
                    *ring_capacity,
                    feedback_identity(
                        group,
                        connection,
                        *connection.detach_initial_value,
                        *ring_capacity),
                    *connection.detach_initial_value);
                if (!persistent) {
                    return std::unexpected(std::move(persistent.error()));
                }
                plan.representations[ring_representation].persistent_allocation =
                    *persistent;
            }

            if (connection.requires_conversion) {
                auto const consumer_position =
                    target_position(connection, group.live_interval);
                auto derived = append_transient_representation(
                    SampleRepresentationPlan{
                        .producer_group_index = group_index,
                        .canonical_producer_representation = false,
                        .implementation = SampleConnectionImplementationKind::transient_materialization,
                        .channel_layout = connection.target_layout,
                        .frame_capacity = *ring_capacity,
                        .live_interval = ConnectionLiveIntervalPlan{
                            .begin = consumer_position,
                            .end = consumer_position,
                            .crosses_kernel_invocations = false,
                        },
                    });
                if (!derived) {
                    return std::unexpected(std::move(derived.error()));
                }
                plan.materializations.push_back(SampleMaterializationPlan{
                    .source_representation = ring_representation,
                    .target_representation = *derived,
                    .after_execution_position = consumer_position,
                    .before_execution_position = consumer_position,
                    .source_layout = *group.canonical_source_layout,
                    .target_layout = connection.target_layout,
                    .retained_before = retained_frames,
                    .latest_read_latency = latency + connection.read_latency,
                });
                plan.connection_representations[connection_index] = *derived;
            } else {
                plan.connection_representations[connection_index] =
                    ring_representation;
            }
            plan.feedback_timelines.push_back(SampleFeedbackTimelinePlan{
                .connection_index = connection_index,
                .timeline_representation = ring_representation,
                .channel_layout = *group.canonical_source_layout,
                .retained_frames = retained_frames,
                .loop_extra_latency = latency,
                .initial_value = *connection.detach_initial_value,
                .writer = SampleFeedbackTimelineWriterPlan{
                    .kind = writes_directly_to_feedback
                        ? SampleFeedbackTimelineWriterKind::producer_home
                        : SampleFeedbackTimelineWriterKind::copy,
                    .after_execution_position = producer_position,
                    .source_representation = writes_directly_to_feedback
                        ? no_sample_representation
                        : canonical,
                },
            });
        }
    }

    // Connections that do not resolve to one canonical whole output port are
    // physical channel compositions. Each actual producer owns its own
    // canonical representation and retention; this synthetic transient value
    // gathers those channels only after all contributing producers have run.
    for (std::size_t connection_index = 0;
         connection_index < connections.sample_connections.size();
         ++connection_index) {
        auto const& connection = connections.sample_connections[connection_index];
        if (connection.access != PlannedConnectionAccess::realtime_to_realtime
            || connection.canonical_source_port
            || connection.source_channel_timings.empty()) {
            continue;
        }
        if (plan.connection_representations[connection_index]) {
            return std::unexpected(
                "GraphJit composed sample connection already owns a physical representation");
        }
        if (connection.external_boundary) {
            return std::unexpected(
                "GraphJit sample composition does not yet realize external storage");
        }
        if (connection.source_channel_timings.size()
                != connection.source_channels.size()
            || connection.projection_contributions.empty()) {
            return std::unexpected(
                "GraphJit sample composition lost normalized projection metadata");
        }
        for (std::size_t channel = 0;
             channel < connection.target_channels.size(); ++channel) {
            auto const& target = connection.target_channels[channel];
            if (target.bundle != connection.target_port.node_bundle_handle
                || target.port != connection.target_port.port_ordinal
                || target.channel != channel) {
                return std::unexpected(
                    "GraphJit normalized sample composition lost canonical target-port coverage");
            }
        }

        auto const begin = composition_position(
            connection,
            ConnectionLiveIntervalPlan{
                .begin = 0,
                .end = target_position(
                    connection, ConnectionLiveIntervalPlan{}),
            });
        auto const end = target_position(
            connection,
            ConnectionLiveIntervalPlan{.begin = begin, .end = begin});

        std::size_t target_representation = no_sample_representation;
        bool const detached = connection.detach.has_value();
        std::size_t feedback_retained_frames = 0;
        if (detached) {
            if (!connection.detach_initial_value) {
                return std::unexpected(
                    "GraphJit composed sample feedback lost its resolved initial value");
            }
            auto max_read_latency = std::size_t{0};
            for (auto const& channel : connection.source_channel_timings) {
                max_read_latency = std::max(
                    max_read_latency, channel.read_latency);
            }
            auto const detach_latency = connection.detach->loop_extra_latency;
            if (connection.target_history
                    > std::numeric_limits<std::size_t>::max() - detach_latency
                || max_read_latency
                    > std::numeric_limits<std::size_t>::max()
                        - detach_latency - connection.target_history) {
                return std::unexpected(
                    "GraphJit composed sample feedback retained extent overflows size_t");
            }
            feedback_retained_frames = detach_latency
                + connection.target_history + max_read_latency;
            auto capacity = working_ring_capacity(
                kernel_block_size, feedback_retained_frames);
            if (!capacity) {
                return std::unexpected(std::move(capacity.error()));
            }

            target_representation = append_representation(
                SampleRepresentationPlan{
                    .producer_group_index = no_sample_producer_group,
                    .canonical_producer_representation = false,
                    .implementation = SampleConnectionImplementationKind::feedback_ring,
                    .channel_layout = connection.target_layout,
                    .frame_capacity = *capacity,
                    .live_interval = ConnectionLiveIntervalPlan{
                        .begin = std::min(begin, end),
                        .end = std::max(begin, end),
                        .crosses_kernel_invocations = true,
                    },
                });
            auto persistent = append_synthetic_persistent_allocation(
                target_representation,
                connection.target_layout,
                feedback_retained_frames,
                *capacity,
                composition_feedback_identity(
                    connection,
                    *connection.detach_initial_value,
                    *capacity),
                *connection.detach_initial_value);
            if (!persistent) {
                return std::unexpected(std::move(persistent.error()));
            }
            plan.representations[target_representation].persistent_allocation =
                *persistent;
        } else {
            auto capacity = working_ring_capacity(
                kernel_block_size, connection.target_history);
            if (!capacity) {
                return std::unexpected(std::move(capacity.error()));
            }
            auto transient = append_transient_representation(
                SampleRepresentationPlan{
                    .producer_group_index = no_sample_producer_group,
                    .canonical_producer_representation = false,
                    .implementation = SampleConnectionImplementationKind::transient_materialization,
                    .channel_layout = connection.target_layout,
                    .frame_capacity = *capacity,
                    .live_interval = ConnectionLiveIntervalPlan{
                        .begin = begin,
                        .end = std::max(begin, end),
                        .crosses_kernel_invocations = false,
                    },
                });
            if (!transient) {
                return std::unexpected(std::move(transient.error()));
            }
            target_representation = *transient;
        }

        std::vector<SampleCompositionContributionPlan> composition_contributions;
        composition_contributions.reserve(
            connection.projection_contributions.size());
        std::vector<bool> populated_targets(channel_count(connection.target_layout), false);

        for (std::size_t contribution_index = 0;
             contribution_index < connection.projection_contributions.size();
             ++contribution_index) {
            auto const& semantic =
                connection.projection_contributions[contribution_index];
            auto const source_layout = ChannelLayout{
                .channel_type = semantic.source_type,
                .sample_layout = SampleStreamLayout::planar,
            };
            auto const converted_layout = ChannelLayout{
                .channel_type = semantic.target_type,
                .sample_layout = SampleStreamLayout::planar,
            };
            if (semantic.source_channel_indices.size()
                    != channel_count(source_layout)
                || semantic.target_channels.size()
                    != channel_count(converted_layout)) {
                return std::unexpected(
                    "GraphJit sample composition contribution has inconsistent semantic channel counts");
            }
            try {
                (void)ChannelConversionRegistry::plan(
                    source_layout, converted_layout);
            } catch (std::exception const& e) {
                return std::unexpected(
                    "GraphJit sample composition conversion is unsupported: "
                    + std::string(e.what()));
            }

            SampleCompositionContributionPlan contribution{
                .source_layout = source_layout,
                .converted_layout = converted_layout,
            };
            contribution.sources.reserve(semantic.source_channel_indices.size());
            contribution.target_channels.reserve(semantic.target_channels.size());

            for (auto const timing_index : semantic.source_channel_indices) {
                if (timing_index >= connection.source_channel_timings.size()) {
                    return std::unexpected(
                        "GraphJit sample composition contribution lost a source timing");
                }
                auto const& channel = connection.source_channel_timings[timing_index];
                NodeBundlePortId const source_port{
                    channel.source.bundle,
                    PortKind::sample,
                    channel.source.port,
                };
                auto const group = std::ranges::find_if(
                    connections.sample_producer_groups,
                    [&](SampleProducerGroupPlan const& candidate) {
                        return candidate.source_port
                            && *candidate.source_port == source_port;
                    });
                if (group == connections.sample_producer_groups.end()) {
                    return std::unexpected(
                        "GraphJit sample composition lost a source producer group");
                }
                auto const group_index = static_cast<std::size_t>(
                    std::distance(connections.sample_producer_groups.begin(), group));
                if (group_index >= plan.producer_groups.size()
                    || !plan.producer_groups[group_index]) {
                    return std::unexpected(
                        "GraphJit sample composition lost a source physical representation");
                }
                auto const source_representation =
                    plan.producer_groups[group_index]->canonical_representation;
                if (source_representation >= plan.representations.size()
                    || channel.source.channel
                        >= channel_count(plan.representations[source_representation].channel_layout)) {
                    return std::unexpected(
                        "GraphJit sample composition source channel is outside its producer representation");
                }
                contribution.sources.push_back(SampleCompositionInputPlan{
                    .source_representation = source_representation,
                    .source_channel = channel.source.channel,
                    .read_latency = channel.read_latency,
                });
            }

            for (auto const target_channel : semantic.target_channels) {
                if (target_channel >= populated_targets.size()
                    || populated_targets[target_channel]) {
                    return std::unexpected(
                        "GraphJit sample composition contribution has an invalid target projection");
                }
                populated_targets[target_channel] = true;
                contribution.target_channels.push_back(target_channel);
            }

            if (detached
                && source_layout.channel_type == ChannelTypeId::stereo
                && converted_layout.channel_type == ChannelTypeId::mono) {
                auto const [minimum_it, maximum_it] = std::ranges::minmax_element(
                    contribution.sources,
                    {},
                    &SampleCompositionInputPlan::read_latency);
                auto const minimum_latency = minimum_it->read_latency;
                auto const maximum_latency = maximum_it->read_latency;
                if (minimum_latency != maximum_latency) {
                    auto const alignment_history =
                        maximum_latency - minimum_latency;
                    auto alignment_capacity = working_ring_capacity(
                        kernel_block_size, alignment_history);
                    if (!alignment_capacity) {
                        return std::unexpected(
                            std::move(alignment_capacity.error()));
                    }
                    auto const alignment_representation = append_representation(
                        SampleRepresentationPlan{
                            .producer_group_index = no_sample_producer_group,
                            .canonical_producer_representation = false,
                            .implementation = SampleConnectionImplementationKind::persistent_ring,
                            .channel_layout = source_layout,
                            .frame_capacity = *alignment_capacity,
                            .live_interval = ConnectionLiveIntervalPlan{
                                .begin = std::min(begin, end),
                                .end = std::max(begin, end),
                                .crosses_kernel_invocations = true,
                            },
                        });
                    auto const identity =
                        composition_feedback_alignment_identity(
                            connection,
                            contribution_index,
                            source_layout,
                            minimum_latency,
                            maximum_latency,
                            *alignment_capacity,
                            *connection.detach_initial_value);
                    auto persistent = append_synthetic_persistent_allocation(
                        alignment_representation,
                        source_layout,
                        alignment_history,
                        *alignment_capacity,
                        identity + ":samples",
                        *connection.detach_initial_value);
                    if (!persistent) {
                        return std::unexpected(std::move(persistent.error()));
                    }
                    plan.representations[alignment_representation]
                        .persistent_allocation = *persistent;

                    contribution.feedback_alignment_representation =
                        alignment_representation;
                    contribution.feedback_alignment_write_latency =
                        minimum_latency;
                }
            }
            composition_contributions.push_back(std::move(contribution));
        }
        if (!std::ranges::all_of(
                populated_targets, [](bool value) { return value; })) {
            return std::unexpected(
                "GraphJit sample composition does not populate every target channel");
        }

        if (detached) {
            plan.feedback_timelines.push_back(SampleFeedbackTimelinePlan{
                .connection_index = connection_index,
                .timeline_representation = target_representation,
                .channel_layout = connection.target_layout,
                .retained_frames = feedback_retained_frames,
                .loop_extra_latency = connection.detach->loop_extra_latency,
                .initial_value = *connection.detach_initial_value,
                .writer = SampleFeedbackTimelineWriterPlan{
                    .kind = SampleFeedbackTimelineWriterKind::composition,
                    .after_execution_position = begin,
                    .composition_contributions = std::move(composition_contributions),
                },
            });
        } else {
            plan.compositions.push_back(SampleCompositionPlan{
                .connection_index = connection_index,
                .contributions = std::move(composition_contributions),
                .target_representation = target_representation,
                .after_execution_position = begin,
                .target_layout = connection.target_layout,
                .target_history = connection.target_history,
            });
        }
        plan.connection_representations[connection_index] = target_representation;
    }

    auto arena = plan_transient_arena(transient_requests);
    if (!arena) return std::unexpected(std::move(arena.error()));
    if (arena->allocations.size() != transient_representations.size()) {
        return std::unexpected(
            "GraphJit transient arena lost sample representation allocations");
    }

    plan.transient_arena_size = arena->size_bytes;
    plan.transient_arena_alignment = arena->alignment;
    plan.transient_allocations.reserve(arena->allocations.size());
    for (std::size_t request_index = 0;
         request_index < arena->allocations.size(); ++request_index) {
        auto const representation_index = transient_representations[request_index];
        auto const& allocation = arena->allocations[request_index];
        auto const allocation_index = plan.transient_allocations.size();
        plan.transient_allocations.push_back(SampleTransientAllocationPlan{
            .representation_index = representation_index,
            .size_bytes = allocation.size_bytes,
            .alignment = allocation.alignment,
            .region_relative_offset = allocation.offset,
        });
        plan.representations[representation_index].transient_allocation =
            allocation_index;
    }

    return plan;
}

std::expected<void, std::string> declare_sample_physical_storage(
    NodeLayoutBuilder& builder,
    SamplePhysicalPlan& plan)
{
    try {
        if (plan.transient_allocations.empty()) {
            if (plan.transient_arena_size != 0) {
                return std::unexpected(
                    "GraphJit empty transient sample plan has a non-zero arena size");
            }
        } else {
            if (plan.transient_arena_size == 0
                || plan.transient_arena_alignment == 0
                || (plan.transient_arena_alignment
                        & (plan.transient_arena_alignment - 1)) != 0) {
                return std::unexpected(
                    "GraphJit sample transient arena has invalid size/alignment");
            }
            for (auto const& allocation : plan.transient_allocations) {
                if (allocation.representation_index >= plan.representations.size()
                    || allocation.size_bytes == 0 || allocation.alignment == 0
                    || (allocation.alignment & (allocation.alignment - 1)) != 0
                    || allocation.region_relative_offset % allocation.alignment != 0
                    || allocation.alignment > plan.transient_arena_alignment
                    || allocation.region_relative_offset > plan.transient_arena_size
                    || allocation.size_bytes
                        > plan.transient_arena_size - allocation.region_relative_offset) {
                    return std::unexpected(
                        "GraphJit sample transient allocation lies outside its arena");
                }
            }
            plan.transient_region = builder.declare_raw_region(
                plan.transient_arena_size, plan.transient_arena_alignment);
        }

        for (auto& allocation : plan.persistent_allocations) {
            if (allocation.representation_index >= plan.representations.size()
                || allocation.size_bytes == 0 || allocation.alignment == 0
                || (allocation.alignment & (allocation.alignment - 1)) != 0
                || allocation.migration_identity.empty()) {
                return std::unexpected(
                    "GraphJit sample persistent allocation is invalid");
            }
            allocation.region = builder.declare_raw_region(
                allocation.size_bytes,
                allocation.alignment,
                allocation.migration_identity,
                allocation.initialize_value ? initialize_sample_raw_region : nullptr,
                allocation.initialize_value
                    ? sample_initialize_payload(*allocation.initialize_value)
                    : std::vector<std::byte>{});
        }
        return {};
    } catch (std::exception const& e) {
        return std::unexpected(
            "GraphJit sample physical storage declaration failed: "
            + std::string(e.what()));
    }
}

std::expected<void, std::string> finalize_sample_physical_storage(
    NodeLayout const& layout,
    SamplePhysicalPlan& plan)
{
    if (plan.transient_allocations.empty()) {
        if (plan.transient_region.valid()) {
            return std::unexpected(
                "GraphJit empty sample transient plan unexpectedly owns a raw region");
        }
    } else {
        if (!plan.transient_region.valid()
            || plan.transient_region.index >= layout.regions.size()) {
            return std::unexpected(
                "GraphJit sample transient region was lost during NodeLayout finalization");
        }
        auto const& region = layout.regions[plan.transient_region.index];
        if (region.kind != NodeLayout::Region::Kind::raw) {
            return std::unexpected(
                "GraphJit sample transient storage was not finalized as raw storage");
        }
        if (region.size != plan.transient_arena_size
            || !region.migration_identity.empty()) {
            return std::unexpected(
                "GraphJit finalized sample transient arena changed semantics");
        }
        for (auto& allocation : plan.transient_allocations) {
            if (allocation.region_relative_offset > region.size
                || allocation.size_bytes > region.size - allocation.region_relative_offset) {
                return std::unexpected(
                    "GraphJit sample transient allocation lies outside its raw region");
            }
            allocation.storage_offset =
                region.storage_offset + allocation.region_relative_offset;
            if (allocation.storage_offset % allocation.alignment != 0) {
                return std::unexpected(
                    "GraphJit finalized sample transient allocation lost alignment");
            }
        }
    }

    for (auto& allocation : plan.persistent_allocations) {
        if (!allocation.region.valid()
            || allocation.region.index >= layout.regions.size()) {
            return std::unexpected(
                "GraphJit sample persistent region was lost during NodeLayout finalization");
        }
        auto const& region = layout.regions[allocation.region.index];
        if (region.kind != NodeLayout::Region::Kind::raw
            || region.size != allocation.size_bytes
            || region.alignment != allocation.alignment
            || region.migration_identity != allocation.migration_identity
            || static_cast<bool>(region.raw_initialize_fn)
                != allocation.initialize_value.has_value()
            || region.raw_initialize_payload.size()
                != (allocation.initialize_value ? sizeof(Sample) : 0u)) {
            return std::unexpected(
                "GraphJit finalized sample persistent region changed semantics");
        }
        allocation.storage_offset = region.storage_offset;
        if (allocation.storage_offset % allocation.alignment != 0) {
            return std::unexpected(
                "GraphJit finalized sample persistent allocation lost alignment");
        }
    }
    return {};
}

} // namespace iv::graph_jit::detail
