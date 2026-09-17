#include <intravenous/graph_jit/sample_physical_plan.h>

#include <intravenous/sample.h>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace iv::graph_jit::detail {
namespace {

std::size_t align_up(std::size_t value, std::size_t alignment)
{
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        throw std::invalid_argument(
            "GraphJit sample physical alignment must be a non-zero power of two");
    }
    if (value > std::numeric_limits<std::size_t>::max() - (alignment - 1)) {
        throw std::overflow_error("GraphJit sample physical layout overflows size_t");
    }
    return (value + alignment - 1) & ~(alignment - 1);
}

bool live_intervals_overlap(
    ConnectionLiveIntervalPlan const& a,
    ConnectionLiveIntervalPlan const& b) noexcept
{
    // Transient slots must never be reused across a value that survives a root
    // invocation. Such groups are rejected by the point-7 capability gate, but
    // keeping the allocator conservative makes this invariant local.
    if (a.crosses_kernel_invocations || b.crosses_kernel_invocations) return true;
    return !(a.end < b.begin || b.end < a.begin);
}

bool slot_is_available(
    SampleTransientSlotPlan const& slot,
    std::vector<SampleRepresentationPlan> const& representations,
    ConnectionLiveIntervalPlan const& requested)
{
    for (auto const representation_index : slot.representations) {
        if (representation_index >= representations.size()) return false;
        if (live_intervals_overlap(
                representations[representation_index].live_interval,
                requested)) {
            return false;
        }
    }
    return true;
}

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

        std::size_t slot_index = no_sample_transient_slot;
        for (std::size_t candidate = 0;
             candidate < plan.transient_slots.size(); ++candidate) {
            if (!slot_is_available(
                    plan.transient_slots[candidate],
                    plan.representations,
                    group.live_interval)) {
                continue;
            }
            slot_index = candidate;
            break;
        }
        if (slot_index == no_sample_transient_slot) {
            slot_index = plan.transient_slots.size();
            plan.transient_slots.push_back(SampleTransientSlotPlan{});
        }

        auto& slot = plan.transient_slots[slot_index];
        slot.size_bytes = std::max(slot.size_bytes, *bytes);
        slot.alignment = std::max(slot.alignment, alignof(Sample));
        slot.representations.push_back(representation_index);
        plan.representations[representation_index].transient_slot = slot_index;
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

    return plan;
}

std::expected<void, std::string> declare_sample_physical_storage(
    NodeLayoutBuilder& builder,
    SamplePhysicalPlan& plan)
{
    try {
        if (plan.transient_slots.empty()) return {};

        std::size_t cursor = 0;
        std::size_t region_alignment = 1;
        for (auto& slot : plan.transient_slots) {
            if (slot.size_bytes == 0 || slot.alignment == 0
                || (slot.alignment & (slot.alignment - 1)) != 0) {
                return std::unexpected(
                    "GraphJit sample transient slot has invalid size/alignment");
            }
            cursor = align_up(cursor, slot.alignment);
            slot.region_relative_offset = cursor;
            if (slot.size_bytes > std::numeric_limits<std::size_t>::max() - cursor) {
                return std::unexpected(
                    "GraphJit sample transient storage layout overflows size_t");
            }
            cursor += slot.size_bytes;
            region_alignment = std::max(region_alignment, slot.alignment);
        }
        plan.transient_region = builder.declare_raw_region(cursor, region_alignment);
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
    if (plan.transient_slots.empty()) {
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
    for (auto& slot : plan.transient_slots) {
        if (slot.region_relative_offset > region.size
            || slot.size_bytes > region.size - slot.region_relative_offset) {
            return std::unexpected(
                "GraphJit sample transient slot lies outside its raw region");
        }
        slot.storage_offset = region.storage_offset + slot.region_relative_offset;
    }
    return {};
}

} // namespace iv::graph_jit::detail
