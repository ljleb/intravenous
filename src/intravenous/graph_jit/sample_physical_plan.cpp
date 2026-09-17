#include <intravenous/graph_jit/sample_physical_plan.h>

#include <intravenous/graph_jit/transient_arena_plan.h>
#include <intravenous/sample.h>

#include <algorithm>
#include <exception>
#include <limits>
#include <utility>

namespace iv::graph_jit::detail {
namespace {

std::expected<std::size_t, std::string> sample_bytes(
    ChannelLayout layout,
    std::size_t frames)
{
    if (!is_valid_channel_type(layout.channel_type)
        || !is_valid_sample_stream_layout(layout.sample_layout)
        || frames == 0 || !is_power_of_2(frames)) {
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

    auto append_transient_representation = [&](SampleRepresentationPlan representation)
        -> std::expected<std::size_t, std::string> {
        auto bytes = sample_bytes(representation.channel_layout, representation.frame_capacity);
        if (!bytes) return std::unexpected(std::move(bytes.error()));
        auto const index = plan.representations.size();
        plan.representations.push_back(std::move(representation));
        transient_requests.push_back(TransientArenaAllocationRequest{
            .size_bytes = *bytes,
            .alignment = alignof(Sample),
            .live_interval = plan.representations.back().live_interval,
        });
        transient_representations.push_back(index);
        return index;
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
        if (group.live_interval.crosses_kernel_invocations) {
            return std::unexpected(
                "GraphJit point-8 sample physical plan does not yet realize cross-kernel retained storage");
        }

        switch (*group.implementation) {
        case SampleConnectionImplementationKind::direct:
        case SampleConnectionImplementationKind::transient_materialization:
            break;
        case SampleConnectionImplementationKind::compact_persistent_carry:
        case SampleConnectionImplementationKind::persistent_ring:
        case SampleConnectionImplementationKind::feedback_ring:
        case SampleConnectionImplementationKind::external_boundary:
            return std::unexpected(
                "GraphJit point-8 sample physical plan does not yet realize retained, feedback, or external storage");
        }

        // The canonical producer representation must overlap every identity
        // consumer and every immediate post-producer materialization. Converted
        // consumers do not extend its lifetime beyond the producer itself.
        auto producer_position = group.live_interval.begin;
        std::optional<NodeBundleHandle> producer_bundle;
        for (auto const connection_index : group.connection_indices) {
            if (connection_index >= connections.sample_connections.size()) {
                return std::unexpected(
                    "GraphJit sample producer group references an invalid connection");
            }
            auto const& connection = connections.sample_connections[connection_index];
            if (connection.access != PlannedConnectionAccess::realtime_to_realtime
                || !connection.canonical_source_port) {
                continue;
            }
            auto const bundle = connection.canonical_source_port->node_bundle_handle;
            if (producer_bundle && *producer_bundle != bundle) {
                return std::unexpected(
                    "GraphJit canonical sample producer group spans multiple source bundles");
            }
            producer_bundle = bundle;
            if (bundle < connections.schedule.bundle_execution_position.size()
                && connections.schedule.bundle_execution_position[bundle]) {
                producer_position =
                    *connections.schedule.bundle_execution_position[bundle];
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
            if (connection_index >= connections.sample_connections.size()) {
                return std::unexpected(
                    "GraphJit sample producer group references an invalid connection");
            }
            auto const& connection = connections.sample_connections[connection_index];
            if (connection.access != PlannedConnectionAccess::realtime_to_realtime) {
                continue;
            }
            if (!connection.requires_conversion) {
                canonical_live.end = std::max(
                    canonical_live.end, target_position(connection, group.live_interval));
            }
        }

        auto canonical = append_transient_representation(SampleRepresentationPlan{
            .producer_group_index = group_index,
            .canonical_producer_representation = true,
            .implementation = *group.implementation,
            .channel_layout = *group.canonical_source_layout,
            .frame_capacity = kernel_block_size,
            .live_interval = canonical_live,
        });
        if (!canonical) return std::unexpected(std::move(canonical.error()));
        plan.producer_groups[group_index] = SampleProducerPhysicalPlan{
            .canonical_representation = *canonical,
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
                plan.connection_representations[connection_index] = *canonical;
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
            if (branch == derived.end()) {
                auto const end = target_position(connection, group.live_interval);
                auto representation = append_transient_representation(
                    SampleRepresentationPlan{
                        .producer_group_index = group_index,
                        .canonical_producer_representation = false,
                        .implementation = SampleConnectionImplementationKind::transient_materialization,
                        .channel_layout = connection.target_layout,
                        .frame_capacity = kernel_block_size,
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
                    .source_representation = *canonical,
                    .target_representation = *representation,
                    .after_execution_position = canonical_live.begin,
                    .source_layout = *group.canonical_source_layout,
                    .target_layout = connection.target_layout,
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
                auto const allocation_request_index = static_cast<std::size_t>(
                    std::distance(
                        transient_representations.begin(),
                        std::ranges::find(
                            transient_representations,
                            branch->representation)));
                if (allocation_request_index >= transient_requests.size()) {
                    return std::unexpected(
                        "GraphJit derived sample representation lost its transient request");
                }
                transient_requests[allocation_request_index].live_interval =
                    representation.live_interval;
            }
            plan.connection_representations[connection_index] = branch->representation;
        }
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
            return {};
        }
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
                "GraphJit empty sample physical plan unexpectedly owns a raw region");
        }
        return {};
    }
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
    if (region.size != plan.transient_arena_size) {
        return std::unexpected(
            "GraphJit finalized sample transient arena changed size");
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
    return {};
}

} // namespace iv::graph_jit::detail
