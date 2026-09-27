#include <intravenous/runtime/tick_invocation_frame.h>

#include <algorithm>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace iv {
namespace {

Coverage const empty_tick_coverage{};

[[nodiscard]] std::size_t coverage_sample_count(
    Coverage const& coverage) noexcept
{
    std::size_t result = 0;
    for (auto const region : coverage.regions()) {
        result += static_cast<std::size_t>(region.end - region.begin);
    }
    return result;
}

[[nodiscard]] std::optional<std::size_t> packed_frame_offset(
    Coverage const& domain,
    SampleIndex index) noexcept
{
    std::size_t offset = 0;
    for (auto const region : domain.regions()) {
        if (region.contains(index)) {
            return offset + static_cast<std::size_t>(index - region.begin);
        }
        if (index < region.begin) break;
        offset += static_cast<std::size_t>(region.end - region.begin);
    }
    return std::nullopt;
}

bool target_subset_matches(
    graph_jit::BackgroundEvaluationPlan const& plan,
    graph_jit::PortSubsetIndex subset,
    NodeBundlePortId port)
{
    return subset < plan.sample_target_subsets.size()
        && plan.sample_target_subsets[subset].port == port;
}

bool target_subset_matches_event(
    graph_jit::BackgroundEvaluationPlan const& plan,
    graph_jit::PortSubsetIndex subset,
    NodeBundlePortId port)
{
    if (subset >= plan.event_target_subsets.size()) return false;
    auto const& target = plan.event_target_subsets[subset].port;
    return target.bundle == port.node_bundle_handle
        && target.port == port.port_index;
}

} // namespace

class TickInvocationWorkspace::Impl {
public:
    struct SampleSlot {
        PersistedPageStore::Snapshot const* snapshot = nullptr;
        std::optional<PersistedOutputId> output{};
        ChannelLayout layout{};
        Coverage const* coverage = &empty_tick_coverage;

        [[nodiscard]] Sample read(
            SampleIndex index, std::size_t channel) const noexcept
        {
            if (!snapshot || !output || snapshot->page_width() == 0) return {};
            auto const width = snapshot->page_width();
            auto const page_index = static_cast<std::uint64_t>(index / width);
            auto const* page = snapshot->find_sample_page(*output, page_index);
            if (!page || page->layout != layout
                || !page->domain.contains(index)
                || channel >= channel_count(page->layout)) {
                return {};
            }

            std::size_t frame = 0;
            std::size_t frames = width;
            if (page->packing == PersistedSamplePacking::dense) {
                frame = static_cast<std::size_t>(index % width);
            } else {
                auto const packed = packed_frame_offset(page->domain, index);
                if (!packed) return {};
                frame = *packed;
                frames = coverage_sample_count(page->domain);
            }
            auto const channels = channel_count(page->layout);
            auto const offset = page->layout.sample_layout
                    == SampleStreamLayout::planar
                ? channel * frames + frame
                : frame * channels + channel;
            return offset < page->values.size() ? page->values[offset] : Sample{};
        }
    };

    struct EventSlot {
        PersistedPageStore::Snapshot const* snapshot = nullptr;
        std::optional<PersistedOutputId> output{};
        EventTypeId type = EventTypeId::empty;
        Coverage const* coverage = &empty_tick_coverage;

        void for_each(
            SampleIndex begin,
            SampleIndex end,
            void* visitor_data,
            RandomAccessEventInputPort::VisitEvent visitor) const
        {
            if (!snapshot || !output || snapshot->page_width() == 0
                || begin >= end) {
                return;
            }
            auto const width = snapshot->page_width();
            auto const first_page = begin / width;
            auto const last_page = (end - 1) / width;
            for (auto page_index = first_page;; ++page_index) {
                auto const* page = snapshot->find_event_page(
                    *output, static_cast<std::uint64_t>(page_index));
                if (page && page->type == type) {
                    auto const page_begin = page_index * width;
                    for (auto const& stored : page->events) {
                        auto const absolute = page_begin
                            + static_cast<SampleIndex>(stored.time);
                        if (absolute < begin || absolute >= end
                            || absolute
                                > std::numeric_limits<EventTime>::max()) {
                            continue;
                        }
                        auto event = stored;
                        event.time = static_cast<EventTime>(absolute);
                        visitor(visitor_data, event);
                    }
                }
                if (page_index == last_page) break;
            }
        }
    };

    std::vector<SampleSlot> sample_slots{};
    std::vector<EventSlot> event_slots{};
    std::vector<RandomAccessSampleInputPort> sample_views{};
    std::vector<RandomAccessEventInputPort> event_views{};

    Impl(
        graph_jit::BackgroundEvaluationPlan const& plan,
        std::uint64_t generation)
    {
        auto const& runtime = plan.tick_runtime;
        sample_slots.resize(runtime.random_access_sample_inputs.size());
        event_slots.resize(runtime.random_access_event_inputs.size());
        sample_views.resize(sample_slots.size());
        event_views.resize(event_slots.size());

        for (std::size_t slot = 0; slot < sample_slots.size(); ++slot) {
            auto const& binding = runtime.random_access_sample_inputs[slot];
            if (binding.port >= plan.ports.size()) {
                throw std::invalid_argument(
                    "Tick sample binding references a missing logical port");
            }
            auto const& port = plan.ports[binding.port];
            auto& selected = sample_slots[slot];
            selected.layout = port.sample_layout;

            std::optional<graph_jit::PortStorageIndex> persisted;
            for (auto const storage : binding.storage) {
                if (storage < plan.storage.ports.size()
                    && plan.storage.ports[storage].storage
                        == graph_jit::PortStorageKind::persisted_pages) {
                    if (persisted) {
                        persisted.reset();
                        break;
                    }
                    persisted = storage;
                }
            }
            if (persisted && binding.storage.size() == 1) {
                auto const& storage = plan.storage.ports[*persisted];
                auto const channels = channel_count(port.sample_layout);
                bool identity = storage.sample_layout == port.sample_layout;
                for (std::size_t channel = 0;
                     identity && channel < channels; ++channel) {
                    auto const direct_count = std::ranges::count_if(
                        plan.storage.direct_samples,
                        [&](auto const& direct) {
                            return direct.target_channel == channel
                                && target_subset_matches(
                                    plan,
                                    direct.target_subset,
                                    port.configured_port);
                        });
                    identity = direct_count == 1 && std::ranges::any_of(
                        plan.storage.direct_samples,
                        [&](auto const& direct) {
                            return direct.storage == *persisted
                                && direct.target_channel == channel
                                && direct.source_channel == channel
                                && direct.read_latency == 0
                                && target_subset_matches(
                                    plan,
                                    direct.target_subset,
                                    port.configured_port);
                        });
                }
                if (identity) {
                    selected.output = persisted_output_id(
                        plan, *persisted, generation);
                }
            }
            sample_views[slot] = RandomAccessSampleInputPort{
                .data = &selected,
                .coverage_value = &empty_tick_coverage,
                .read_sample = +[](void const* opaque,
                                   SampleIndex index,
                                   std::size_t channel) noexcept {
                    return static_cast<SampleSlot const*>(opaque)->read(
                        index, channel);
                },
            };
        }

        for (std::size_t slot = 0; slot < event_slots.size(); ++slot) {
            auto const& binding = runtime.random_access_event_inputs[slot];
            if (binding.port >= plan.ports.size()) {
                throw std::invalid_argument(
                    "Tick event binding references a missing logical port");
            }
            auto const& port = plan.ports[binding.port];
            auto& selected = event_slots[slot];
            selected.type = port.event_type;

            std::optional<graph_jit::PortStorageIndex> persisted;
            for (auto const storage : binding.storage) {
                if (storage < plan.storage.ports.size()
                    && plan.storage.ports[storage].storage
                        == graph_jit::PortStorageKind::persisted_pages) {
                    if (persisted) {
                        persisted.reset();
                        break;
                    }
                    persisted = storage;
                }
            }
            auto const direct = persisted && binding.storage.size() == 1
                && plan.storage.ports[*persisted].event_type == port.event_type
                && std::ranges::count_if(
                    plan.storage.direct_events,
                    [&](auto const& candidate) {
                        return target_subset_matches_event(
                            plan,
                            candidate.target_subset,
                            port.configured_port);
                    }) == 1
                && std::ranges::any_of(
                    plan.storage.direct_events,
                    [&](auto const& candidate) {
                        return candidate.storage == *persisted
                            && target_subset_matches_event(
                                plan,
                                candidate.target_subset,
                                port.configured_port);
                    });
            if (direct) {
                selected.output = persisted_output_id(
                    plan, *persisted, generation);
            }
            event_views[slot] = RandomAccessEventInputPort{
                .data = &selected,
                .coverage_value = &empty_tick_coverage,
                .for_each_event = +[](
                    void const* opaque,
                    SampleIndex begin,
                    SampleIndex end,
                    void* visitor_data,
                    RandomAccessEventInputPort::VisitEvent visitor) {
                    static_cast<EventSlot const*>(opaque)->for_each(
                        begin, end, visitor_data, visitor);
                },
            };
        }
    }

    graph_jit::TickInvocationCall bind(
        PersistedPageStore::Snapshot const& published) noexcept
    {
        for (std::size_t slot = 0; slot < sample_slots.size(); ++slot) {
            auto& selected = sample_slots[slot];
            selected.snapshot = &published;
            selected.coverage = selected.output
                ? published.find_sample_coverage(
                    *selected.output, selected.layout)
                : nullptr;
            if (!selected.coverage) selected.coverage = &empty_tick_coverage;
            sample_views[slot].coverage_value = selected.coverage;
        }
        for (std::size_t slot = 0; slot < event_slots.size(); ++slot) {
            auto& selected = event_slots[slot];
            selected.snapshot = &published;
            selected.coverage = selected.output
                ? published.find_event_coverage(
                    *selected.output, selected.type)
                : nullptr;
            if (!selected.coverage) selected.coverage = &empty_tick_coverage;
            event_views[slot].coverage_value = selected.coverage;
        }
        return {
            .random_access_sample_inputs = sample_views,
            .random_access_event_inputs = event_views,
        };
    }
};

TickInvocationWorkspace::TickInvocationWorkspace(
    graph_jit::BackgroundEvaluationPlan const& plan,
    std::uint64_t generation)
    : impl_(std::make_unique<Impl>(plan, generation))
{}

TickInvocationWorkspace::~TickInvocationWorkspace() = default;

std::size_t TickInvocationWorkspace::random_access_sample_count() const noexcept
{
    return impl_->sample_views.size();
}

std::size_t TickInvocationWorkspace::random_access_event_count() const noexcept
{
    return impl_->event_views.size();
}

graph_jit::TickInvocationCall TickInvocationWorkspace::bind(
    PersistedPageStore::Snapshot const& published) noexcept
{
    return impl_->bind(published);
}

TickInvocationFrame::TickInvocationFrame(
    PersistedPageStore::ReaderSlot& page_reader,
    TickInvocationWorkspace& workspace) noexcept
    : published_pages_(page_reader.pin())
    , call_(workspace.bind(published_pages_.snapshot()))
{}

} // namespace iv
