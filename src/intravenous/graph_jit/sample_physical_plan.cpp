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
                "GraphJit point-7 sample physical plan does not yet realize cross-kernel retained storage");
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
                "GraphJit point-7 sample physical plan does not yet realize retained, feedback, or external storage");
        }

        // Point 7 realizes only canonical realtime producer representations.
        // Compiled-access branches intentionally have no realtime physical
        // representation here, and layout-converted realtime branches require
        // a derived representation that point 8 will add explicitly.
        for (auto const connection_index : group.connection_indices) {
            if (connection_index >= connections.sample_connections.size()) {
                return std::unexpected(
                    "GraphJit sample producer group references an invalid connection");
            }
            auto const& connection = connections.sample_connections[connection_index];
            if (connection.access != PlannedConnectionAccess::realtime_to_realtime) {
                continue;
            }
            if (connection.requires_conversion) {
                return std::unexpected(
                    "GraphJit point-7 sample physical plan does not yet realize derived converted sample representations");
            }
        }

        auto bytes = sample_bytes(*group.canonical_source_layout, kernel_block_size);
        if (!bytes) return std::unexpected(std::move(bytes.error()));

        auto const representation_index = plan.representations.size();
        plan.representations.push_back(SampleRepresentationPlan{
            .producer_group_index = group_index,
            .canonical_producer_representation = true,
            .implementation = *group.implementation,
            .channel_layout = *group.canonical_source_layout,
            .frame_capacity = kernel_block_size,
            .live_interval = group.live_interval,
        });
        transient_requests.push_back(TransientArenaAllocationRequest{
            .size_bytes = *bytes,
            .alignment = alignof(Sample),
            .live_interval = group.live_interval,
        });
        transient_representations.push_back(representation_index);
        plan.producer_groups[group_index] = SampleProducerPhysicalPlan{
            .canonical_representation = representation_index,
        };

        for (auto const connection_index : group.connection_indices) {
            auto const& connection = connections.sample_connections[connection_index];
            if (connection.access != PlannedConnectionAccess::realtime_to_realtime) {
                continue;
            }
            if (plan.connection_representations[connection_index]) {
                return std::unexpected(
                    "GraphJit sample connection belongs to multiple producer groups");
            }
            // Point 7 has no derived branches yet. Every realized realtime
            // connection is an identity view of its producer's canonical
            // representation. Compiled-access branches remain unresolved here.
            plan.connection_representations[connection_index] =
                representation_index;
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
