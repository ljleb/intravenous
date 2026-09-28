#include <intravenous/runtime/tick_invocation_frame.h>

#include <algorithm>
#include <bit>
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

template<class Binding>
std::optional<PersistedOutputId> direct_persisted_sample_output(
    graph_jit::BackgroundEvaluationPlan const& plan,
    Binding const& binding,
    graph_jit::BackgroundPortPlan const& port,
    std::uint64_t generation)
{
    std::optional<graph_jit::PortStorageIndex> persisted;
    for (auto const storage : binding.storage) {
        if (storage < plan.storage.ports.size()
            && plan.storage.ports[storage].storage
                == graph_jit::PortStorageKind::persisted_pages) {
            if (persisted) return std::nullopt;
            persisted = storage;
        }
    }
    if (!persisted || binding.storage.size() != 1) return std::nullopt;

    auto const& storage = plan.storage.ports[*persisted];
    auto const channels = channel_count(port.sample_layout);
    bool identity = storage.sample_layout == port.sample_layout;
    for (std::size_t channel = 0; identity && channel < channels; ++channel) {
        auto const direct_count = std::ranges::count_if(
            plan.storage.direct_samples,
            [&](auto const& direct) {
                return direct.target_channel == channel
                    && target_subset_matches(
                        plan, direct.target_subset, port.configured_port);
            });
        identity = direct_count == 1 && std::ranges::any_of(
            plan.storage.direct_samples,
            [&](auto const& direct) {
                return direct.storage == *persisted
                    && direct.target_channel == channel
                    && direct.source_channel == channel
                    && direct.read_latency == 0
                    && target_subset_matches(
                        plan, direct.target_subset, port.configured_port);
            });
    }
    return identity
        ? std::optional<PersistedOutputId>{
            persisted_output_id(plan, *persisted, generation)}
        : std::nullopt;
}

} // namespace

class TickInvocationWorkspace::Impl {
public:
    struct SequentialSampleSlot {
        graph_jit::BackgroundPortIndex port = 0;
        std::optional<PersistedOutputId> output{};
        bool accepts_materialization = false;
        ChannelLayout layout{};
        std::size_t history = 0;
        std::size_t capacity = 0;
        Sample neutral{};
        std::vector<Sample> values{};
        ReflectedSampleInputPortBinding binding{};

        void initialize_storage()
        {
            auto const channels = channel_count(layout);
            if (channels == 0 || channels > maximum_supported_channel_count
                || capacity == 0 || !std::has_single_bit(capacity)) {
                throw std::invalid_argument(
                    "Tick Sequential sample binding has an invalid layout");
            }
            if (capacity > std::numeric_limits<std::size_t>::max() / channels) {
                throw std::length_error(
                    "Tick Sequential sample playback storage is too large");
            }
            values.assign(capacity * channels, neutral);
            binding = {
                .storage = {
                    .frame_capacity = capacity,
                    .storage_latency = 0,
                    .channel_layout = layout,
                },
                .history = history,
                .read_latency = 0,
            };
            for (std::size_t channel = 0; channel < channels; ++channel) {
                auto const planar = layout.sample_layout
                    == SampleStreamLayout::planar;
                binding.storage.channels[channel] = {
                    .storage = reinterpret_cast<std::byte*>(
                        values.data() + (planar ? channel * capacity : channel)),
                    .frame_capacity = capacity,
                    .frame_stride = planar ? 1 : channels,
                    .frame_delay = 0,
                };
            }
        }

        [[nodiscard]] Sample read(
            PersistedPageStore::Snapshot const& published,
            TickMaterializedSampleInput const* materialized,
            SampleIndex index,
            std::size_t channel) const noexcept
        {
            if (materialized && materialized->coverage.contains(index)) {
                return materialized->at(index, channel);
            }
            if (!output || published.page_width() == 0) return neutral;
            auto const width = published.page_width();
            auto const page_index = static_cast<std::uint64_t>(index / width);
            auto const* page = published.find_sample_page(*output, page_index);
            if (!page || page->layout != layout
                || !page->domain.contains(index)
                || channel >= channel_count(page->layout)) {
                return neutral;
            }

            std::size_t frame = 0;
            std::size_t frames = width;
            if (page->packing == PersistedSamplePacking::dense) {
                frame = static_cast<std::size_t>(index % width);
            } else {
                auto const packed = packed_frame_offset(page->domain, index);
                if (!packed) return neutral;
                frame = *packed;
                frames = coverage_sample_count(page->domain);
            }
            auto const channels = channel_count(page->layout);
            auto const offset = page->layout.sample_layout
                    == SampleStreamLayout::planar
                ? channel * frames + frame
                : frame * channels + channel;
            return offset < page->values.size() ? page->values[offset] : neutral;
        }

        void populate(
            PersistedPageStore::Snapshot const& published,
            TickMaterializedSampleInput const* materialized,
            SampleIndex sample_index,
            std::size_t block_size) noexcept
        {
            std::ranges::fill(values, neutral);
            auto const channels = channel_count(layout);
            auto const begin = sample_index - static_cast<SampleIndex>(history);
            auto const count = history + block_size;
            auto const mask = capacity - 1;
            for (std::size_t offset = 0; offset < count; ++offset) {
                auto const index = begin + static_cast<SampleIndex>(offset);
                auto const frame = static_cast<std::size_t>(index & mask);
                for (std::size_t channel = 0; channel < channels; ++channel) {
                    auto const value = read(
                        published, materialized, index, channel);
                    if (layout.sample_layout == SampleStreamLayout::planar) {
                        values[channel * capacity + frame] = value;
                    } else {
                        values[frame * channels + channel] = value;
                    }
                }
            }
        }
    };

    struct SampleSlot {
        PersistedPageStore::Snapshot const* snapshot = nullptr;
        TickMaterializedSampleInput const* materialized = nullptr;
        std::optional<PersistedOutputId> output{};
        graph_jit::BackgroundPortIndex port = 0;
        bool accepts_materialization = false;
        ChannelLayout layout{};
        Coverage const* coverage = &empty_tick_coverage;

        [[nodiscard]] Sample read(
            SampleIndex index, std::size_t channel) const noexcept
        {
            if (materialized && materialized->coverage.contains(index)
                && materialized->has_channel(channel)) {
                return materialized->at(index, channel);
            }
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
        TickMaterializedEventInput const* materialized = nullptr;
        std::optional<PersistedOutputId> output{};
        graph_jit::BackgroundPortIndex port = 0;
        bool accepts_materialization = false;
        EventTypeId type = EventTypeId::empty;
        Coverage const* coverage = &empty_tick_coverage;

        void for_each(
            SampleIndex begin,
            SampleIndex end,
            void* visitor_data,
            RandomAccessEventInputPort::VisitEvent visitor) const
        {
            if (materialized) {
                materialized->for_each(
                    begin, end, visitor_data, visitor);
                return;
            }
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

    std::vector<SequentialSampleSlot> sequential_sample_slots{};
    std::vector<ReflectedSampleInputPortBinding> sequential_sample_views{};
    std::vector<SampleSlot> sample_slots{};
    std::vector<EventSlot> event_slots{};
    std::vector<RandomAccessSampleInputPort> sample_views{};
    std::vector<RandomAccessEventInputPort> event_views{};
    std::uint64_t generation = 0;
    std::size_t maximum_block_size = 0;

    Impl(
        graph_jit::BackgroundEvaluationPlan const& plan,
        std::uint64_t selected_generation,
        std::size_t selected_maximum_block_size)
        : generation(selected_generation)
        , maximum_block_size(selected_maximum_block_size)
    {
        if (maximum_block_size == 0) {
            throw std::invalid_argument(
                "Tick invocation maximum block size must be non-zero");
        }
        auto const& runtime = plan.tick_runtime;
        sequential_sample_slots.resize(runtime.sequential_sample_inputs.size());
        sequential_sample_views.resize(sequential_sample_slots.size());
        sample_slots.resize(runtime.random_access_sample_inputs.size());
        event_slots.resize(runtime.random_access_event_inputs.size());
        sample_views.resize(sample_slots.size());
        event_views.resize(event_slots.size());

        for (std::size_t slot = 0;
             slot < sequential_sample_slots.size(); ++slot) {
            auto const& planned = runtime.sequential_sample_inputs[slot];
            if (planned.port >= plan.ports.size()) {
                throw std::invalid_argument(
                    "Tick Sequential sample binding references a missing "
                    "logical port");
            }
            auto const& port = plan.ports[planned.port];
            if (port.kind != PortKind::sample
                || port.direction != graph_jit::PortDirection::input) {
                throw std::invalid_argument(
                    "Tick Sequential sample binding references a non-sample "
                    "input port");
            }
            if (port.sequential_history
                > std::numeric_limits<std::size_t>::max()
                    - maximum_block_size) {
                throw std::length_error(
                    "Tick Sequential sample playback window is too large");
            }
            auto const required = port.sequential_history + maximum_block_size;
            auto const largest_power_of_two = std::size_t{1}
                << (std::numeric_limits<std::size_t>::digits - 1);
            if (required > largest_power_of_two) {
                throw std::length_error(
                    "Tick Sequential sample playback capacity is too large");
            }

            auto& selected = sequential_sample_slots[slot];
            selected.port = planned.port;
            selected.layout = port.sample_layout;
            selected.history = port.sequential_history;
            selected.capacity = std::bit_ceil(required);
            selected.neutral = port.sample_neutral_value;
            selected.accepts_materialization = std::ranges::any_of(
                planned.storage, [&](auto const storage) {
                    return storage < plan.storage.ports.size()
                        && plan.storage.ports[storage].storage
                            == graph_jit::PortStorageKind::tick_sequential;
                });
            selected.output = direct_persisted_sample_output(
                plan, planned, port, selected_generation);
            selected.initialize_storage();
            sequential_sample_views[slot] = selected.binding;
        }

        for (std::size_t slot = 0; slot < sample_slots.size(); ++slot) {
            auto const& binding = runtime.random_access_sample_inputs[slot];
            if (binding.port >= plan.ports.size()) {
                throw std::invalid_argument(
                    "Tick sample binding references a missing logical port");
            }
            auto const& port = plan.ports[binding.port];
            auto& selected = sample_slots[slot];
            selected.port = binding.port;
            selected.layout = port.sample_layout;
            selected.accepts_materialization = std::ranges::any_of(
                binding.storage, [&](auto const storage) {
                    return storage < plan.storage.ports.size()
                        && plan.storage.ports[storage].storage
                            == graph_jit::PortStorageKind::tick_random_access;
                });

            selected.output = direct_persisted_sample_output(
                plan, binding, port, selected_generation);
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
            selected.port = binding.port;
            selected.type = port.event_type;
            selected.accepts_materialization = std::ranges::any_of(
                binding.storage, [&](auto const storage) {
                    return storage < plan.storage.ports.size()
                        && plan.storage.ports[storage].storage
                            == graph_jit::PortStorageKind::tick_random_access;
                });

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
                    plan, *persisted, selected_generation);
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
        PersistedPageStore::Snapshot const& published,
        TickMaterializationSnapshot const& materialized,
        SampleIndex sample_index,
        std::size_t block_size) noexcept
    {
        auto const materialization_matches = materialized.generation() == generation
            && materialized.pages() == published.version();
        auto const selected_block_size = std::min(block_size, maximum_block_size);
        for (auto& selected : sequential_sample_slots) {
            auto const* materialized_sample = materialization_matches
                    && selected.accepts_materialization
                ? materialized.find_sample(selected.port)
                : nullptr;
            auto const materialized_sample_is_complete = materialized_sample
                && materialized_sample->layout == selected.layout
                && [&] {
                    for (std::size_t channel = 0;
                         channel < channel_count(selected.layout); ++channel) {
                        if (!materialized_sample->has_channel(channel)) {
                            return false;
                        }
                    }
                    return true;
                }();
            selected.populate(
                published,
                materialized_sample_is_complete ? materialized_sample : nullptr,
                sample_index,
                selected_block_size);
        }
        for (std::size_t slot = 0; slot < sample_slots.size(); ++slot) {
            auto& selected = sample_slots[slot];
            selected.snapshot = &published;
            selected.materialized = materialization_matches
                    && selected.accepts_materialization
                ? materialized.find_sample(selected.port)
                : nullptr;
            auto const materialized_sample_is_complete = selected.materialized
                && selected.materialized->layout == selected.layout
                && [&] {
                    for (std::size_t channel = 0;
                         channel < channel_count(selected.layout); ++channel) {
                        if (!selected.materialized->has_channel(channel)) {
                            return false;
                        }
                    }
                    return true;
                }();
            selected.coverage = materialized_sample_is_complete
                ? &selected.materialized->coverage
                : selected.output
                    ? published.find_sample_coverage(
                        *selected.output, selected.layout)
                    : nullptr;
            if (!materialized_sample_is_complete) {
                selected.materialized = nullptr;
            }
            if (!selected.coverage) selected.coverage = &empty_tick_coverage;
            sample_views[slot].coverage_value = selected.coverage;
        }
        for (std::size_t slot = 0; slot < event_slots.size(); ++slot) {
            auto& selected = event_slots[slot];
            selected.snapshot = &published;
            selected.materialized = materialization_matches
                    && selected.accepts_materialization
                ? materialized.find_event(selected.port)
                : nullptr;
            selected.coverage = selected.materialized
                && selected.materialized->type == selected.type
                ? &selected.materialized->coverage
                : selected.output
                    ? published.find_event_coverage(
                        *selected.output, selected.type)
                    : nullptr;
            if (selected.materialized
                && selected.materialized->type != selected.type) {
                selected.materialized = nullptr;
            }
            if (!selected.coverage) selected.coverage = &empty_tick_coverage;
            event_views[slot].coverage_value = selected.coverage;
        }
        return {
            .sequential_sample_inputs = sequential_sample_views,
            .random_access_sample_inputs = sample_views,
            .random_access_event_inputs = event_views,
        };
    }
};

TickInvocationWorkspace::TickInvocationWorkspace(
    graph_jit::BackgroundEvaluationPlan const& plan,
    std::uint64_t generation,
    std::size_t maximum_block_size)
    : impl_(std::make_unique<Impl>(plan, generation, maximum_block_size))
{}

TickInvocationWorkspace::~TickInvocationWorkspace() = default;

std::size_t TickInvocationWorkspace::sequential_sample_count() const noexcept
{
    return impl_->sequential_sample_views.size();
}

std::size_t TickInvocationWorkspace::random_access_sample_count() const noexcept
{
    return impl_->sample_views.size();
}

std::size_t TickInvocationWorkspace::random_access_event_count() const noexcept
{
    return impl_->event_views.size();
}

graph_jit::TickInvocationCall TickInvocationWorkspace::bind(
    PersistedPageStore::Snapshot const& published,
    TickMaterializationSnapshot const& materialized,
    SampleIndex sample_index,
    std::size_t block_size) noexcept
{
    return impl_->bind(published, materialized, sample_index, block_size);
}

TickInvocationFrame::TickInvocationFrame(
    PersistedPageStore::ReaderSlot& page_reader,
    TickMaterializationStore::ReaderSlot& materialization_reader,
    TickInvocationWorkspace& workspace,
    SampleIndex sample_index,
    std::size_t block_size) noexcept
    : published_pages_(page_reader.pin())
    , materialized_storage_(materialization_reader.pin())
    , call_(workspace.bind(
        published_pages_.snapshot(),
        materialized_storage_.snapshot(),
        sample_index,
        block_size))
{}

} // namespace iv
