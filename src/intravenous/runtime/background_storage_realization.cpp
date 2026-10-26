#include <intravenous/runtime/background_storage_realization.h>

#include <intravenous/compat.h>
#include <intravenous/graph_jit/event_conversion_runtime.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <numeric>
#include <new>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace iv {
namespace {

IndexCoverage const empty_storage_coverage{};

struct AlignedBufferDelete {
    std::size_t alignment = alignof(std::max_align_t);

    void operator()(std::byte* storage) const noexcept
    {
        if (storage) {
            ::operator delete(storage, std::align_val_t{alignment});
        }
    }
};

class AlignedByteBuffer {
    std::unique_ptr<std::byte, AlignedBufferDelete> storage_{};
    std::size_t size_ = 0;

public:
    AlignedByteBuffer(std::size_t size, std::size_t alignment)
        : storage_(
            static_cast<std::byte*>(
                ::operator new(size, std::align_val_t{alignment})),
            AlignedBufferDelete{alignment})
        , size_(size)
    {
        std::fill_n(storage_.get(), size_, std::byte{});
    }

    [[nodiscard]] std::byte* data() noexcept { return storage_.get(); }
    [[nodiscard]] std::byte const* data() const noexcept
    {
        return storage_.get();
    }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
};

[[nodiscard]] IndexCoverage region_coverage(
    std::span<GlobalIndexRange const> regions)
{
    IndexCoverage result;
    for (auto const region : regions) result.include(region);
    return result;
}

[[nodiscard]] std::size_t checked_product(
    std::size_t left,
    std::size_t right)
{
    if (right != 0 && left > std::numeric_limits<std::size_t>::max() / right) {
        throw std::length_error("background sample storage is too large");
    }
    return left * right;
}

[[nodiscard]] bool contains_coverage(
    IndexCoverage const& outer,
    IndexCoverage const& inner) noexcept
{
    return std::ranges::all_of(inner, [&](GlobalIndexRange region) {
        return outer.contains(region);
    });
}

[[nodiscard]] bool contains_channels(
    std::span<std::size_t const> outer,
    std::span<std::size_t const> inner) noexcept
{
    return std::ranges::all_of(inner, [&](std::size_t channel) {
        return std::ranges::contains(outer, channel);
    });
}

[[nodiscard]] bool event_matches_type(
    EventTypeId type,
    Event const& event) noexcept
{
    switch (type) {
    case EventTypeId::empty:
        return std::holds_alternative<EmptyEvent>(event);
    case EventTypeId::trigger:
        return std::holds_alternative<TriggerEvent>(event);
    case EventTypeId::boundary:
        return std::holds_alternative<BoundaryEvent>(event);
    case EventTypeId::midi:
        return std::holds_alternative<MidiEvent>(event);
    case EventTypeId::count:
        break;
    }
    return false;
}

template<typename Fn>
void for_each_intersection(
    IndexCoverage const& left,
    IndexCoverage const& right,
    Fn&& fn)
{
    auto intersection = left & right;
    for (auto const region : intersection) fn(region);
}

[[nodiscard]] std::expected<std::size_t, std::string>
event_merge_capacity(std::size_t count)
{
    auto const requested = std::max<std::size_t>(count, 1);
    auto const maximum_power_of_two = std::size_t{1}
        << (std::numeric_limits<std::size_t>::digits - 1);
    if (requested > maximum_power_of_two) {
        return std::unexpected(
            "background event materialization is too large to merge");
    }
    return std::bit_ceil(requested);
}

class TransactionSampleStorage {
    struct AlignedDelete {
        std::size_t alignment = alignof(std::max_align_t);

        void operator()(std::byte* storage) const noexcept
        {
            if (storage) {
                ::operator delete(storage, std::align_val_t{alignment});
            }
        }
    };

    struct OwnedRegion {
        std::unique_ptr<std::byte, AlignedDelete> storage{
            nullptr, AlignedDelete{}};
        std::vector<std::uint8_t> written{};
        ContinuousRegionStorageView view{};
    };

    IndexCoverage const* coverage_ = nullptr;
    ContinuousValueStorageGeometry geometry_{};
    ChannelLayout layout_{};
    std::vector<std::size_t> channels_{};
    std::vector<OwnedRegion> owned_regions_{};
    std::vector<ContinuousRegionStorageView> region_views_{};

    [[nodiscard]] std::optional<std::size_t> channel_offset(
        std::size_t channel) const noexcept
    {
        auto const found = std::ranges::lower_bound(channels_, channel);
        if (found == channels_.end() || *found != channel) return std::nullopt;
        return static_cast<std::size_t>(
            std::distance(channels_.begin(), found));
    }

public:
    TransactionSampleStorage(
        IndexCoverage const& coverage,
        std::span<GlobalIndexRange const> regions,
        ContinuousValueStorageGeometry geometry,
        ChannelLayout layout,
        std::span<std::size_t const> channels)
        : coverage_(&coverage)
        , geometry_(geometry)
        , layout_(layout)
        , channels_(channels.begin(), channels.end())
    {
        if (!is_valid_channel_type(layout.channel_type)
            || !is_valid_sample_stream_layout(layout.sample_layout)) {
            throw std::invalid_argument(
                "background sample storage has an invalid layout");
        }
        std::ranges::sort(channels_);
        auto const duplicate = std::ranges::adjacent_find(channels_);
        if (duplicate != channels_.end()
            || std::ranges::any_of(channels_, [&](std::size_t channel) {
                return channel >= channel_count(layout_);
            })) {
            throw std::invalid_argument(
                "background sample storage has invalid channels");
        }

        if (!is_registered_continuous_value_type(geometry_.value_type)
            || geometry_.value_size_bytes == 0
            || geometry_.element_alignment == 0
            || !std::has_single_bit(geometry_.element_alignment)) {
            throw std::invalid_argument(
                "background continuous storage has invalid geometry");
        }
        owned_regions_.reserve(regions.size());
        region_views_.reserve(regions.size());
        for (auto const range : regions) {
            if (range.empty()
                || range.extent > std::numeric_limits<std::size_t>::max()) {
                throw std::length_error(
                    "background continuous Region is too large");
            }
            auto const frames = static_cast<std::size_t>(range.extent);
            auto const value_count = checked_product(
                frames, channel_count(layout_));
            auto const byte_count = checked_product(
                value_count, geometry_.value_size_bytes);
            auto* storage = static_cast<std::byte*>(::operator new(
                byte_count, std::align_val_t{geometry_.element_alignment}));
            std::fill_n(storage, byte_count, std::byte{});
            OwnedRegion owned{
                .storage = std::unique_ptr<std::byte, AlignedDelete>{
                    storage,
                    AlignedDelete{geometry_.element_alignment}},
                .written = std::vector<std::uint8_t>(value_count, 0),
                .view = {
                    .storage = storage,
                    .range = range,
                    .geometry = geometry_,
                    .channel_layout = layout_,
                },
            };
            owned.view.written_values = owned.written.data();
            region_views_.push_back(owned.view);
            owned_regions_.push_back(std::move(owned));
        }
    }

    [[nodiscard]] Sample read(
        GlobalIndex index,
        std::size_t channel) const noexcept
    {
        auto const region = std::ranges::find_if(
            region_views_, [&](auto const& value) {
                return value.range.contains(index);
            });
        auto const selected_channel = channel_offset(channel);
        IV_ASSERT(region != region_views_.end() && selected_channel,
            "background sample read lies outside realized storage");
        if (region == region_views_.end() || !selected_channel
            || geometry_.value_type != continuous_value_type_id_v<Sample>) {
            return {};
        }
        return details::ContinuousRegionView<Sample, false>{*region}.at(
            index, channel);
    }

    [[nodiscard]] bool write(
        GlobalIndex index,
        std::size_t channel,
        Sample value) noexcept
    {
        auto const region = std::ranges::find_if(
            region_views_, [&](auto const& candidate) {
                return candidate.range.contains(index);
            });
        auto const selected_channel = channel_offset(channel);
        if (region == region_views_.end() || !selected_channel
            || geometry_.value_type != continuous_value_type_id_v<Sample>) {
            return false;
        }
        details::ContinuousRegionView<Sample, true>{*region}.at(index, channel)
            = value;
        return true;
    }

    [[nodiscard]] bool complete() const noexcept
    {
        return std::ranges::all_of(
            owned_regions_, [&](OwnedRegion const& region) {
                auto const frames = region.view.frame_count();
                auto const channel_count_value = channel_count(layout_);
                for (auto const channel : channels_) {
                    for (std::size_t frame = 0; frame < frames; ++frame) {
                        auto const native_index =
                            layout_.sample_layout
                                    == SampleStreamLayout::planar
                                ? channel * frames + frame
                                : frame * channel_count_value + channel;
                        if (native_index >= region.written.size()
                            || region.written[native_index] == 0) {
                            return false;
                        }
                    }
                }
                return true;
            });
    }

    [[nodiscard]] BackgroundSampleReadView read_view() const noexcept
    {
        return {
            .coverage_value = coverage_,
            .regions = region_views_,
            .geometry = geometry_,
            .layout = layout_,
            .channels = channels_,
        };
    }

    [[nodiscard]] BackgroundSampleWriteView write_view() noexcept
    {
        return {
            .coverage_value = coverage_,
            .regions = region_views_,
            .geometry = geometry_,
            .layout = layout_,
            .channels = channels_,
        };
    }
};

class TransactionEventStorage {
    struct Region {
        GlobalIndexRange range{};
        std::size_t capacity = 0;
        std::vector<TimedEvent> events{};
        std::vector<std::size_t> source_indices{};
    };

    IndexCoverage const* coverage_ = nullptr;
    EventTypeId type_ = EventTypeId::empty;
    std::vector<GlobalIndexRange> region_ranges_{};
    std::vector<Region> regions_{};

    [[nodiscard]] bool write_to_region(
        std::size_t region_index,
        TimedEvent const& event,
        std::size_t source_index) noexcept
    {
        if (region_index >= regions_.size()) return false;
        auto& region = regions_[region_index];
        if (!region.range.contains(event.time)
            || !event_matches_type(type_, event.value)
            || region.events.size() == region.capacity
            || (!region.events.empty()
                && RangeRelativeIndexOrder{region.range.begin}(
                    event.time, region.events.back().time))) {
            return false;
        }
        region.events.push_back(event);
        region.source_indices.push_back(source_index);
        return true;
    }

    template<typename Visit>
    void visit_merged(GlobalIndexRange requested, Visit&& visit) const
    {
        struct Entry {
            TimedEvent const* event = nullptr;
            std::size_t source_index = 0;
            std::size_t region = 0;
            std::size_t ordinal = 0;
        };
        std::vector<Entry> entries;
        for (std::size_t region_index = 0;
             region_index < regions_.size(); ++region_index) {
            auto const& region = regions_[region_index];
            for (std::size_t event = 0; event < region.events.size(); ++event) {
                if (!requested.contains(region.events[event].time)) continue;
                entries.push_back({
                    .event = std::addressof(region.events[event]),
                    .source_index = region.source_indices[event],
                    .region = region_index,
                    .ordinal = event,
                });
            }
        }
        std::stable_sort(entries.begin(), entries.end(),
            [&](Entry const& left, Entry const& right) {
                if (left.event->time != right.event->time) {
                    return RangeRelativeIndexOrder{requested.begin}(
                        left.event->time, right.event->time);
                }
                if (left.source_index != right.source_index) {
                    return left.source_index < right.source_index;
                }
                if (left.region != right.region) {
                    return left.region < right.region;
                }
                return left.ordinal < right.ordinal;
            });
        for (auto const& entry : entries) {
            visit(*entry.event, entry.source_index);
        }
    }

public:
    TransactionEventStorage(
        IndexCoverage const& coverage,
        std::span<GlobalIndexRange const> regions,
        EventTypeId type,
        double max_events_per_index)
        : coverage_(&coverage)
        , type_(type)
    {
        if (type >= EventTypeId::count) {
            throw std::invalid_argument(
                "background event storage has an invalid type");
        }
        if (regions.empty() && !coverage.empty()) {
            region_ranges_.assign(coverage.begin(), coverage.end());
        } else {
            region_ranges_.assign(regions.begin(), regions.end());
        }
        if (region_coverage(region_ranges_) != coverage) {
            throw std::invalid_argument(
                "background event Regions do not match selected coverage");
        }
        regions_.reserve(region_ranges_.size());
        for (auto const range : region_ranges_) {
            if (range.empty()
                || range.extent > std::numeric_limits<std::size_t>::max()) {
                throw std::invalid_argument(
                    "background event storage has an invalid Region");
            }
            auto const capacity = event_count_for_sample_span(
                max_events_per_index, static_cast<std::size_t>(range.extent));
            if (!capacity) {
                throw std::length_error(
                    "background event Region capacity is not representable");
            }
            Region region{
                .range = range,
                .capacity = *capacity,
            };
            region.events.reserve(region.capacity);
            region.source_indices.reserve(region.capacity);
            regions_.push_back(std::move(region));
        }
    }

    [[nodiscard]] bool write(TimedEvent const& event) noexcept
    {
        return write(event, std::numeric_limits<std::size_t>::max());
    }

    [[nodiscard]] bool write(
        TimedEvent const& event, std::size_t source_index) noexcept
    {
        for (std::size_t index = 0; index < regions_.size(); ++index) {
            if (regions_[index].range.contains(event.time)) {
                return write_to_region(index, event, source_index);
            }
        }
        return false;
    }

    [[nodiscard]] bool write_region(
        std::size_t region,
        TimedEvent const& event) noexcept
    {
        return write_to_region(
            region, event, std::numeric_limits<std::size_t>::max());
    }

    [[nodiscard]] bool write_region(
        std::size_t region,
        TimedEvent const& event,
        std::size_t source_index) noexcept
    {
        return write_to_region(region, event, source_index);
    }

    void for_each_region(
        std::size_t region_index,
        GlobalIndexRange requested,
        void* visitor_data,
        BackgroundEventReadView::VisitEvent visitor) const
    {
        if (region_index >= regions_.size()) return;
        auto const& region = regions_[region_index];
        for (auto const& event : region.events) {
            if (requested.contains(event.time)) visitor(visitor_data, event);
        }
    }

    void for_each_indexed(
        GlobalIndexRange region,
        void* visitor_data,
        BackgroundEventReadView::VisitIndexedEvent visitor) const
    {
        visit_merged(region, [&](TimedEvent const& event, std::size_t source) {
            visitor(visitor_data, event, source);
        });
    }

    void for_each(
        GlobalIndexRange region,
        void* visitor_data,
        BackgroundEventReadView::VisitEvent visitor) const
    {
        visit_merged(region, [&](TimedEvent const& event, std::size_t) {
            visitor(visitor_data, event);
        });
    }

    [[nodiscard]] BackgroundEventReadView read_view() const noexcept
    {
        return {
            .data = this,
            .coverage_value = coverage_,
            .regions = region_ranges_,
            .type = type_,
            .for_each_event = +[](void const* opaque,
                                  GlobalIndexRange region,
                                  void* visitor_data,
                                  BackgroundEventReadView::VisitEvent visitor) {
                static_cast<TransactionEventStorage const*>(opaque)->for_each(
                    region, visitor_data, visitor);
            },
            .for_each_region_event = +[](
                void const* opaque,
                std::size_t region_index,
                GlobalIndexRange region,
                void* visitor_data,
                BackgroundEventReadView::VisitEvent visitor) {
                static_cast<TransactionEventStorage const*>(opaque)
                    ->for_each_region(
                        region_index, region, visitor_data, visitor);
            },
            .for_each_indexed_event = +[](
                void const* opaque,
                GlobalIndexRange region,
                void* visitor_data,
                BackgroundEventReadView::VisitIndexedEvent visitor) {
                static_cast<TransactionEventStorage const*>(opaque)
                    ->for_each_indexed(region, visitor_data, visitor);
            },
        };
    }

    [[nodiscard]] BackgroundEventWriteView write_view() noexcept
    {
        return {
            .data = this,
            .coverage_value = coverage_,
            .regions = region_ranges_,
            .type = type_,
            .write_event = +[](void* opaque, TimedEvent const& event) noexcept {
                return static_cast<TransactionEventStorage*>(opaque)->write(event);
            },
            .write_region_event = +[](
                void* opaque,
                std::size_t region,
                TimedEvent const& event) noexcept {
                return static_cast<TransactionEventStorage*>(opaque)
                    ->write_region(region, event);
            },
            .write_indexed_region_event = +[](
                void* opaque,
                std::size_t region,
                TimedEvent const& event,
                std::size_t source_index) noexcept {
                return static_cast<TransactionEventStorage*>(opaque)
                    ->write_region(region, event, source_index);
            },
            .write_indexed_event = +[](
                void* opaque,
                TimedEvent const& event,
                std::size_t source_index) noexcept {
                return static_cast<TransactionEventStorage*>(opaque)->write(
                    event, source_index);
            },
        };
    }
};

class PersistedSampleReader {
    PersistedDataStore::Snapshot const* snapshot_ = nullptr;
    PersistedOutputHandle output_{};
    IndexCoverage const* coverage_ = nullptr;
    ContinuousValueStorageGeometry geometry_{};
    ChannelLayout layout_{};
    std::span<std::size_t const> channels_{};
    std::vector<ContinuousRegionStorageView> regions_{};
    bool selected_regions_complete_ = false;

public:
    PersistedSampleReader(
        PersistedDataStore::Snapshot const& snapshot,
        PersistedOutputHandle output,
        IndexCoverage const& coverage,
        std::span<GlobalIndexRange const> selected_regions,
        ContinuousValueStorageGeometry geometry,
        ChannelLayout layout,
        std::span<std::size_t const> channels)
        : snapshot_(&snapshot)
        , output_(std::move(output))
        , coverage_(&coverage)
        , geometry_(geometry)
        , layout_(layout)
        , channels_(channels)
    {
        auto retained = snapshot_->sample_regions(output_, layout_);
        std::vector<bool> used(retained.size(), false);
        for (auto const selected : selected_regions) {
            for (std::size_t index = 0; index < retained.size(); ++index) {
                auto const& region = retained[index];
                if (used[index] || region.range != selected
                    || region.geometry != geometry) {
                    continue;
                }
                used[index] = true;
                regions_.push_back({
                    .storage = const_cast<std::byte*>(region.storage_data()),
                    .range = region.range,
                    .geometry = region.geometry,
                    .channel_layout = region.layout,
                });
                break;
            }
        }
        selected_regions_complete_ = regions_.size() == selected_regions.size();
    }

    [[nodiscard]] bool complete() const noexcept
    {
        if (!selected_regions_complete_) return false;
        IndexCoverage descriptor_coverage;
        for (auto const& region : regions_) {
            descriptor_coverage.include(region.range);
        }
        if (!contains_coverage(descriptor_coverage, *coverage_)) return false;
        if (coverage_->empty()) return true;
        auto const* retained = snapshot_->find_sample_coverage(
            output_, layout_, geometry_);
        return retained && contains_coverage(*retained, *coverage_)
            && std::ranges::none_of(channels_, [&](std::size_t channel) {
                return channel >= channel_count(layout_);
            });
    }

    [[nodiscard]] Sample read(
        GlobalIndex index,
        std::size_t channel) const noexcept
    {
        auto const* region = snapshot_->find_sample_region(output_, index);
        IV_ASSERT(region != nullptr,
            "persisted sample read has no retained region");
        if (!region || region->layout != layout_
            || channel >= channel_count(region->layout)) {
            return {};
        }
        return region->at(index, channel);
    }

    [[nodiscard]] BackgroundSampleReadView view() const noexcept
    {
        return {
            .coverage_value = coverage_,
            .regions = regions_,
            .geometry = geometry_,
            .layout = layout_,
            .channels = channels_,
        };
    }
};

class PersistedEventReader {
    PersistedDataStore::Snapshot const* snapshot_ = nullptr;
    PersistedOutputHandle output_{};
    IndexCoverage const* coverage_ = nullptr;
    EventTypeId type_ = EventTypeId::empty;
    std::vector<PersistedEventChunk const*> regions_{};
    std::vector<GlobalIndexRange> region_ranges_{};
    bool selected_regions_complete_ = false;

public:
    PersistedEventReader(
        PersistedDataStore::Snapshot const& snapshot,
        PersistedOutputHandle output,
        IndexCoverage const& coverage,
        std::span<GlobalIndexRange const> selected_regions,
        EventTypeId type)
        : snapshot_(&snapshot)
        , output_(std::move(output))
        , coverage_(&coverage)
        , type_(type)
    {
        auto retained = snapshot_->event_regions(output_, type_);
        std::vector<bool> used(retained.size(), false);
        for (auto const selected : selected_regions) {
            for (std::size_t index = 0; index < retained.size(); ++index) {
                if (used[index] || retained[index].range != selected) continue;
                used[index] = true;
                regions_.push_back(std::addressof(retained[index]));
                region_ranges_.push_back(selected);
                break;
            }
        }
        selected_regions_complete_ =
            regions_.size() == selected_regions.size();
    }

    [[nodiscard]] bool complete() const noexcept
    {
        if (!selected_regions_complete_) return false;
        IndexCoverage descriptor_coverage;
        for (auto const region : region_ranges_) {
            descriptor_coverage.include(region);
        }
        if (!contains_coverage(descriptor_coverage, *coverage_)) return false;
        if (coverage_->empty()) return true;
        auto const* retained = snapshot_->find_event_coverage(output_, type_);
        return retained && contains_coverage(*retained, *coverage_);
    }

    void for_each(
        GlobalIndexRange region,
        void* visitor_data,
        BackgroundEventReadView::VisitEvent visitor) const
    {
        if (region.empty()) return;
        std::vector<std::pair<TimedEvent const*, std::size_t>> events;
        for (std::size_t region_index = 0;
             region_index < regions_.size(); ++region_index) {
            for (auto const& event : regions_[region_index]->events) {
                if (region.contains(event.time)) {
                    events.emplace_back(std::addressof(event), region_index);
                }
            }
        }
        std::stable_sort(events.begin(), events.end(), [&](auto left, auto right) {
            if (left.first->time != right.first->time) {
                return RangeRelativeIndexOrder{region.begin}(
                    left.first->time, right.first->time);
            }
            return left.second < right.second;
        });
        for (auto const [event, unused] : events) {
            static_cast<void>(unused);
            visitor(visitor_data, *event);
        }
    }

    void for_each_region(
        std::size_t region_index,
        GlobalIndexRange requested,
        void* visitor_data,
        BackgroundEventReadView::VisitEvent visitor) const
    {
        if (region_index >= regions_.size()) return;
        for (auto const& event : regions_[region_index]->events) {
            if (requested.contains(event.time)) visitor(visitor_data, event);
        }
    }

    [[nodiscard]] BackgroundEventReadView view() const noexcept
    {
        return {
            .data = this,
            .coverage_value = coverage_,
            .regions = region_ranges_,
            .type = type_,
            .for_each_event = +[](void const* opaque,
                                  GlobalIndexRange region,
                                  void* visitor_data,
                                  BackgroundEventReadView::VisitEvent visitor) {
                static_cast<PersistedEventReader const*>(opaque)->for_each(
                    region, visitor_data, visitor);
            },
            .for_each_region_event = +[](
                void const* opaque,
                std::size_t region_index,
                GlobalIndexRange region,
                void* visitor_data,
                BackgroundEventReadView::VisitEvent visitor) {
                static_cast<PersistedEventReader const*>(opaque)
                    ->for_each_region(
                        region_index, region, visitor_data, visitor);
            },
        };
    }
};

[[nodiscard]] bool storage_is_runtime_produced(
    graph_jit::BackgroundEvaluationPlan const& plan,
    graph_jit::PortStoragePlan const& storage) noexcept
{
    if (storage.sample_materialization || storage.event_materialization) {
        return true;
    }
    if (!storage.output_port || *storage.output_port >= plan.ports.size()) {
        return false;
    }
    auto const& output = plan.ports[*storage.output_port];
    return output.authored_tock_output || output.replayed_tick_output;
}

} // namespace

IndexCoverage const& BackgroundSampleReadView::coverage() const noexcept
{
    return coverage_value ? *coverage_value : empty_storage_coverage;
}

bool BackgroundSampleReadView::has_channel(
    std::size_t channel) const noexcept
{
    return std::ranges::contains(channels, channel);
}

Sample BackgroundSampleReadView::at(
    GlobalIndex index,
    std::size_t channel) const noexcept
{
    IV_ASSERT(*this, "background sample storage has no readable view");
    IV_ASSERT(coverage().contains(index) && has_channel(channel),
        "background sample read lies outside selected storage");
    if (!*this || geometry.value_type != continuous_value_type_id_v<Sample>) {
        return {};
    }
    auto const found = std::ranges::find_if(
        regions, [&](auto const& region) { return region.range.contains(index); });
    IV_ASSERT(found != regions.end(),
        "background sample read has no containing Region");
    return found == regions.end()
        ? Sample{}
        : details::ContinuousRegionView<Sample, false>{*found}.at(
              index, channel);
}

ContinuousRegionStorageView const& BackgroundSampleReadView::region(
    std::size_t index) const noexcept
{
    IV_ASSERT(index < regions.size(),
        "background continuous read Region is out of bounds");
    return regions[index];
}

IndexCoverage const& BackgroundSampleWriteView::coverage() const noexcept
{
    return coverage_value ? *coverage_value : empty_storage_coverage;
}

bool BackgroundSampleWriteView::has_channel(
    std::size_t channel) const noexcept
{
    return std::ranges::contains(channels, channel);
}

bool BackgroundSampleWriteView::write(
    GlobalIndex index,
    std::size_t channel,
    Sample value) const noexcept
{
    if (!*this || !coverage().contains(index) || !has_channel(channel)
        || geometry.value_type != continuous_value_type_id_v<Sample>) {
        return false;
    }
    auto const found = std::ranges::find_if(
        regions, [&](auto const& region) { return region.range.contains(index); });
    if (found == regions.end()) return false;
    details::ContinuousRegionView<Sample, true>{*found}.at(index, channel) = value;
    return true;
}

bool BackgroundSampleWriteView::write(
    std::size_t region_index,
    GlobalIndex index,
    std::size_t channel,
    Sample value) const noexcept
{
    if (!*this || region_index >= regions.size() || !has_channel(channel)
        || geometry.value_type != continuous_value_type_id_v<Sample>
        || !regions[region_index].range.contains(index)) {
        return false;
    }
    details::ContinuousRegionView<Sample, true>{regions[region_index]}.at(
        index, channel) = value;
    return true;
}

ContinuousRegionStorageView const& BackgroundSampleWriteView::region(
    std::size_t index) const noexcept
{
    IV_ASSERT(index < regions.size(),
        "background continuous write Region is out of bounds");
    return regions[index];
}

IndexCoverage const& BackgroundEventReadView::coverage() const noexcept
{
    return coverage_value ? *coverage_value : empty_storage_coverage;
}

IndexCoverage const& BackgroundEventWriteView::coverage() const noexcept
{
    return coverage_value ? *coverage_value : empty_storage_coverage;
}

bool BackgroundEventWriteView::write(TimedEvent const& event) const noexcept
{
    return *this && coverage().contains(event.time)
        && write_event(data, event);
}

bool BackgroundEventWriteView::write(
    TimedEvent const& event, std::size_t source_index) const noexcept
{
    return *this && coverage().contains(event.time)
        && write_indexed_event
        && write_indexed_event(data, event, source_index);
}

bool BackgroundEventWriteView::write(
    std::size_t region,
    TimedEvent const& event) const noexcept
{
    return *this && region < regions.size()
        && regions[region].contains(event.time)
        && write_region_event
        && write_region_event(data, region, event);
}

bool BackgroundEventWriteView::write(
    std::size_t region,
    TimedEvent const& event,
    std::size_t source_index) const noexcept
{
    return *this && region < regions.size()
        && regions[region].contains(event.time)
        && write_indexed_region_event
        && write_indexed_region_event(data, region, event, source_index);
}

struct BackgroundStorageRealization::Slot {
    graph_jit::PortStoragePlan const* plan = nullptr;
    IndexCoverage const* coverage = nullptr;
    std::optional<PersistedOutputHandle> persisted_output{};
    std::shared_ptr<TransactionSampleStorage> owned_sample{};
    std::shared_ptr<TransactionEventStorage> owned_event{};
    std::unique_ptr<PersistedSampleReader> persisted_sample{};
    std::unique_ptr<PersistedEventReader> persisted_event{};
    std::optional<BackgroundSampleReadView> sample_read{};
    std::optional<BackgroundSampleWriteView> sample_write{};
    std::optional<BackgroundEventReadView> event_read{};
    std::optional<BackgroundEventWriteView> event_write{};
};

BackgroundStorageRealization::BackgroundStorageRealization(
    graph_jit::BackgroundEvaluationPlan const& plan,
    PersistedDataStore& data,
    BackgroundStorageSelection selection,
    graph_jit::SequentialInputCombineFunction combine_sequential_input,
    graph_jit::SequentialInputInitializeFunction initialize_sequential_input)
    : plan_(&plan)
    , selection_(std::move(selection))
    , executed_operations_(plan.runtime.operations.size(), false)
    , combine_sequential_input_(combine_sequential_input)
    , initialize_sequential_input_(initialize_sequential_input)
{
    if (selection_.storage_coverage.size() != plan.storage.ports.size()) {
        throw std::invalid_argument(
            "background storage coverage is not aligned with the compiled plan");
    }
    if (selection_.storage_regions.empty()) {
        selection_.storage_regions.resize(plan.storage.ports.size());
        for (graph_jit::PortStorageIndex index = 0;
             index < plan.storage.ports.size(); ++index) {
            selection_.storage_regions[index].assign(
                selection_.storage_coverage[index].begin(),
                selection_.storage_coverage[index].end());
        }
    } else if (selection_.storage_regions.size()
               != plan.storage.ports.size()) {
        throw std::invalid_argument(
            "background storage Regions are not aligned with the compiled plan");
    }
    auto const has_explicit_published_output_regions =
        !selection_.published_output_regions.empty();
    if (!has_explicit_published_output_regions) {
        selection_.published_output_regions.resize(plan.storage.ports.size());
    } else if (selection_.published_output_regions.size()
               != plan.storage.ports.size()) {
        throw std::invalid_argument(
            "background published output Regions are not aligned with the compiled plan");
    }
    if (selection_.persisted_replacement_coverage.empty()) {
        selection_.persisted_replacement_coverage =
            selection_.storage_coverage;
    } else if (selection_.persisted_replacement_coverage.size()
               != plan.storage.ports.size()) {
        throw std::invalid_argument(
            "background persisted replacement coverage is not aligned with the compiled plan");
    }
    if (!selection_.produce_storage.empty()
        && selection_.produce_storage.size() != plan.storage.ports.size()) {
        throw std::invalid_argument(
            "background storage production is not aligned with the compiled plan");
    }

    auto storage_is_produced = [&](graph_jit::PortStorageIndex index) {
        return selection_.produce_storage.empty()
            ? storage_is_runtime_produced(plan, plan.storage.ports[index])
            : selection_.produce_storage[index];
    };

    // One authored output has one writable Region list even when
    // planning retained several physical storage roles for it. The former
    // scalar writer broadcast every value to those roles. Region views cannot
    // express such a broadcast while retaining contiguous storage, so all
    // transaction-produced roles of one logical output share one producer-home
    // allocation instead.
    std::vector<std::vector<graph_jit::PortStorageIndex>> output_storage(
        plan.ports.size());
    for (graph_jit::PortStorageIndex index = 0;
         index < plan.storage.ports.size(); ++index) {
        auto const& storage = plan.storage.ports[index];
        if (!storage.output_port
            || *storage.output_port >= plan.ports.size()
            || storage.storage == graph_jit::PortStorageKind::current_tick
            || !storage_is_produced(index)) {
            continue;
        }
        output_storage[*storage.output_port].push_back(index);
    }
    for (auto const& group : output_storage) {
        if (group.empty()) continue;
        auto const& first_storage = plan.storage.ports[group.front()];
        if (std::ranges::any_of(group, [&](graph_jit::PortStorageIndex index) {
                auto const& candidate = plan.storage.ports[index];
                if (candidate.kind != first_storage.kind) return true;
                return candidate.kind == PortKind::sample
                    ? candidate.sample_layout != first_storage.sample_layout
                        || candidate.continuous_geometry
                            != first_storage.continuous_geometry
                    : candidate.event_type != first_storage.event_type
                        || candidate.max_events_per_index
                            != first_storage.max_events_per_index;
            })) {
            throw std::invalid_argument(
                "background output storage roles disagree on value representation");
        }
        IndexCoverage selected;
        for (auto const index : group) {
            selected.include(selection_.storage_coverage[index]);
        }

        std::span<GlobalIndexRange const> published;
        if (has_explicit_published_output_regions) {
            for (auto const index : group) {
                auto const& candidate =
                    selection_.published_output_regions[index];
                if (candidate.empty()) continue;
                if (!published.empty()
                    && !std::ranges::equal(published, candidate)) {
                    throw std::invalid_argument(
                        "background output storage roles disagree on their published Region list");
                }
                published = candidate;
            }
        } else if (group.size() == 1) {
            published = selection_.storage_regions[group.front()];
        }
        std::vector<GlobalIndexRange> regions;
        if (!published.empty()) {
            for (auto const region : published) {
                if (!(IndexCoverage{region} & selected).empty()) {
                    regions.push_back(region);
                    selected.include(region);
                }
            }
        } else {
            regions.assign(selected.begin(), selected.end());
        }
        for (auto const index : group) {
            selection_.storage_coverage[index] = selected;
            selection_.storage_regions[index] = regions;
            if (!has_explicit_published_output_regions) {
                selection_.published_output_regions[index] = regions;
            }
            if (plan.storage.ports[index].storage
                    == graph_jit::PortStorageKind::persisted_data) {
                selection_.persisted_replacement_coverage[index].include(
                    selected);
            }
        }
    }

    std::vector<std::shared_ptr<TransactionSampleStorage>> output_samples(
        plan.ports.size());
    std::vector<std::shared_ptr<TransactionEventStorage>> output_events(
        plan.ports.size());
    slots_.reserve(plan.storage.ports.size());
    for (graph_jit::PortStorageIndex index = 0;
         index < plan.storage.ports.size(); ++index) {
        auto const& planned = plan.storage.ports[index];
        auto created = std::make_unique<Slot>();
        created->plan = &planned;
        created->coverage = &selection_.storage_coverage[index];

        if (planned.storage == graph_jit::PortStorageKind::persisted_data) {
            created->persisted_output = data.resolve_output(
                persisted_output_id(plan, index, selection_.generation));
        }

        auto const produced = storage_is_produced(index);
        if (produced && planned.storage != graph_jit::PortStorageKind::current_tick) {
            if (planned.kind == PortKind::sample) {
                if (planned.output_port
                    && *planned.output_port < output_samples.size()) {
                    auto& shared = output_samples[*planned.output_port];
                    if (!shared) {
                        std::vector<std::size_t> channels(
                            channel_count(planned.sample_layout));
                        std::iota(channels.begin(), channels.end(), 0);
                        shared = std::make_shared<TransactionSampleStorage>(
                            *created->coverage,
                            selection_.storage_regions[index],
                            planned.continuous_geometry,
                            planned.sample_layout,
                            channels);
                    }
                    created->owned_sample = shared;
                } else {
                    created->owned_sample =
                        std::make_shared<TransactionSampleStorage>(
                            *created->coverage,
                            selection_.storage_regions[index],
                            planned.continuous_geometry,
                            planned.sample_layout,
                            planned.sample_channels);
                }
                created->sample_read = created->owned_sample->read_view();
                created->sample_write = created->owned_sample->write_view();
            } else {
                if (planned.output_port
                    && *planned.output_port < output_events.size()) {
                    auto& shared = output_events[*planned.output_port];
                    if (!shared) {
                        shared = std::make_shared<TransactionEventStorage>(
                            *created->coverage,
                            selection_.storage_regions[index],
                            planned.event_type,
                            planned.max_events_per_index);
                    }
                    created->owned_event = shared;
                } else {
                    created->owned_event =
                        std::make_shared<TransactionEventStorage>(
                            *created->coverage,
                            selection_.storage_regions[index],
                            planned.event_type,
                            planned.max_events_per_index);
                }
                created->event_read = created->owned_event->read_view();
                created->event_write = created->owned_event->write_view();
            }
        } else if (planned.storage
                == graph_jit::PortStorageKind::persisted_data
            && selection_.published) {
            if (planned.kind == PortKind::sample) {
                created->persisted_sample =
                    std::make_unique<PersistedSampleReader>(
                        *selection_.published,
                        *created->persisted_output,
                        *created->coverage,
                        selection_.storage_regions[index],
                        planned.continuous_geometry,
                        planned.sample_layout,
                        planned.sample_channels);
                created->sample_read = created->persisted_sample->view();
            } else {
                created->persisted_event =
                    std::make_unique<PersistedEventReader>(
                        *selection_.published,
                        *created->persisted_output,
                        *created->coverage,
                        selection_.storage_regions[index],
                        planned.event_type);
                created->event_read = created->persisted_event->view();
            }
        }
        slots_.push_back(std::move(created));
    }
}

BackgroundStorageRealization::~BackgroundStorageRealization() = default;

BackgroundStorageRealization::Slot& BackgroundStorageRealization::slot(
    graph_jit::PortStorageIndex index)
{
    if (index >= slots_.size()) {
        throw std::out_of_range("background storage index is out of range");
    }
    return *slots_[index];
}

BackgroundStorageRealization::Slot const& BackgroundStorageRealization::slot(
    graph_jit::PortStorageIndex index) const
{
    if (index >= slots_.size()) {
        throw std::out_of_range("background storage index is out of range");
    }
    return *slots_[index];
}

std::size_t BackgroundStorageRealization::storage_count() const noexcept
{
    return slots_.size();
}

std::size_t BackgroundStorageRealization::operation_count() const noexcept
{
    return executed_operations_.size();
}

bool BackgroundStorageRealization::operation_executed(
    graph_jit::BackgroundRuntimeOperationIndex index) const noexcept
{
    return index < executed_operations_.size() && executed_operations_[index];
}

void BackgroundStorageRealization::bind_sample(
    graph_jit::PortStorageIndex index,
    BackgroundSampleReadView read,
    BackgroundSampleWriteView write)
{
    if (sealed_) {
        throw std::logic_error("background storage realization is sealed");
    }
    auto& selected = slot(index);
    if (selected.plan->kind != PortKind::sample || !read) {
        throw std::invalid_argument(
            "background sample binding is incompatible with storage");
    }
    selected.sample_read = read;
    if (write) selected.sample_write = write;
}

void BackgroundStorageRealization::bind_event(
    graph_jit::PortStorageIndex index,
    BackgroundEventReadView read,
    BackgroundEventWriteView write)
{
    if (sealed_) {
        throw std::logic_error("background storage realization is sealed");
    }
    auto& selected = slot(index);
    if (selected.plan->kind != PortKind::event || !read) {
        throw std::invalid_argument(
            "background event binding is incompatible with storage");
    }
    selected.event_read = read;
    if (write) selected.event_write = write;
}

BackgroundSampleReadView const* BackgroundStorageRealization::sample_read(
    graph_jit::PortStorageIndex index) const
{
    auto const& selected = slot(index);
    return selected.sample_read ? &*selected.sample_read : nullptr;
}

BackgroundSampleWriteView const* BackgroundStorageRealization::sample_write(
    graph_jit::PortStorageIndex index) const
{
    auto const& selected = slot(index);
    return selected.sample_write ? &*selected.sample_write : nullptr;
}

BackgroundEventReadView const* BackgroundStorageRealization::event_read(
    graph_jit::PortStorageIndex index) const
{
    auto const& selected = slot(index);
    return selected.event_read ? &*selected.event_read : nullptr;
}

BackgroundEventWriteView const* BackgroundStorageRealization::event_write(
    graph_jit::PortStorageIndex index) const
{
    auto const& selected = slot(index);
    return selected.event_write ? &*selected.event_write : nullptr;
}

PersistedOutputHandle const*
BackgroundStorageRealization::persisted_output_handle(
    graph_jit::PortStorageIndex index) const
{
    auto const& selected = slot(index);
    return selected.persisted_output
        ? &*selected.persisted_output
        : nullptr;
}

std::expected<void, std::string> BackgroundStorageRealization::seal()
{
    auto const valid_sample_regions = [&](Slot const& selected,
                                          auto const& view) {
        IndexCoverage described;
        for (auto const& region : view.regions) {
            if (region.storage == nullptr || region.range.empty()
                || region.geometry != selected.plan->continuous_geometry
                || region.channel_layout != selected.plan->sample_layout) {
                return false;
            }
            described.include(region.range);
        }
        return contains_coverage(described, *selected.coverage);
    };
    auto const valid_sample_read = [&](Slot const& selected) {
        auto const& planned = *selected.plan;
        return selected.sample_read
            && selected.sample_read->geometry
                == planned.continuous_geometry
            && selected.sample_read->layout == planned.sample_layout
            && contains_coverage(
                selected.sample_read->coverage(), *selected.coverage)
            && valid_sample_regions(selected, *selected.sample_read)
            && contains_channels(
                selected.sample_read->channels, planned.sample_channels);
    };
    auto const valid_sample_write = [&](Slot const& selected) {
        auto const& planned = *selected.plan;
        return selected.sample_write
            && selected.sample_write->geometry
                == planned.continuous_geometry
            && selected.sample_write->layout == planned.sample_layout
            && contains_coverage(
                selected.sample_write->coverage(), *selected.coverage)
            && valid_sample_regions(selected, *selected.sample_write)
            && contains_channels(
                selected.sample_write->channels, planned.sample_channels);
    };
    auto const valid_event_read = [&](Slot const& selected) {
        return selected.event_read
            && selected.event_read->type == selected.plan->event_type
            && (selected.event_read->coverage().empty()
                || selected.event_read->for_each_region_event != nullptr)
            && region_coverage(selected.event_read->regions)
                == selected.event_read->coverage()
            && contains_coverage(
                selected.event_read->coverage(), *selected.coverage);
    };
    auto const valid_event_write = [&](Slot const& selected) {
        return selected.event_write
            && selected.event_write->type == selected.plan->event_type
            && (selected.event_write->coverage().empty()
                || selected.event_write->write_region_event != nullptr)
            && region_coverage(selected.event_write->regions)
                == selected.event_write->coverage()
            && contains_coverage(
                selected.event_write->coverage(), *selected.coverage);
    };
    auto require_read = [&](graph_jit::PortStorageIndex index)
        -> std::expected<void, std::string> {
        auto const& selected = slot(index);
        if ((selected.persisted_sample &&
             !selected.persisted_sample->complete()) ||
            (selected.persisted_event &&
             !selected.persisted_event->complete())) {
            return std::unexpected(
                "background persisted input is missing selected published "
                "coverage");
        }
        auto const valid = selected.plan->kind == PortKind::sample
            ? valid_sample_read(selected)
            : valid_event_read(selected);
        if (!valid) {
            return std::unexpected(
                "background storage has no compatible readable view");
        }
        return {};
    };
    auto require_write = [&](graph_jit::PortStorageIndex index)
        -> std::expected<void, std::string> {
        auto const& selected = slot(index);
        auto const valid = selected.plan->kind == PortKind::sample
            ? valid_sample_write(selected)
            : valid_event_write(selected);
        if (!valid) {
            return std::unexpected(
                "background storage has no compatible writable view");
        }
        return {};
    };

    for (auto const& binding : plan_->runtime.bindings) {
        for (auto const storage : binding.storage) {
            if (slot(storage).coverage->empty()) continue;
            if (binding.direction == graph_jit::PortDirection::output
                && !selection_.produce_storage.empty()
                && !selection_.produce_storage[storage]) {
                continue;
            }
            auto valid = binding.direction == graph_jit::PortDirection::input
                ? require_read(storage)
                : require_write(storage);
            if (!valid) return valid;
        }
    }
    for (auto const& materialization : plan_->storage.sample_materializations) {
        if (slot(materialization.output).coverage->empty()) continue;
        for (auto const input : materialization.inputs) {
            if (auto valid = require_read(input); !valid) return valid;
        }
        if (auto valid = require_write(materialization.output); !valid) {
            return valid;
        }
    }
    for (auto const& materialization : plan_->storage.event_materializations) {
        if (slot(materialization.output).coverage->empty()) continue;
        for (auto const input : materialization.inputs) {
            if (auto valid = require_read(input); !valid) return valid;
        }
        if (auto valid = require_write(materialization.output); !valid) {
            return valid;
        }
    }
    for (auto const& direct : plan_->storage.direct_samples) {
        if (slot(direct.storage).coverage->empty()) continue;
        if (auto valid = require_read(direct.storage); !valid) return valid;
    }
    for (auto const& direct : plan_->storage.direct_events) {
        if (slot(direct.storage).coverage->empty()) continue;
        if (auto valid = require_read(direct.storage); !valid) return valid;
    }

    sealed_ = true;
    return {};
}

std::expected<void, std::string>
BackgroundStorageRealization::execute_operation(
    graph_jit::BackgroundRuntimeOperationIndex index)
{
    if (!sealed_) {
        return std::unexpected(
            "background storage operations require a sealed realization");
    }
    if (index >= plan_->runtime.operations.size()) {
        return std::unexpected(
            "background runtime operation index is out of range");
    }
    if (executed_operations_[index]) {
        return std::unexpected(
            "background runtime operation executed more than once");
    }

    auto const& operation = plan_->runtime.operations[index];
    std::expected<void, std::string> result;
    switch (operation.kind) {
    case graph_jit::BackgroundRuntimeOperationKind::direct_sample: {
        if (operation.operation >= plan_->storage.direct_samples.size()) {
            result = std::unexpected(
                "background direct-sample operation is out of range");
            break;
        }
        auto const& direct =
            plan_->storage.direct_samples[operation.operation];
        auto const* source = sample_read(direct.storage);
        if (!source || !source->has_channel(direct.source_channel)) {
            result = std::unexpected(
                "background direct-sample operation has no source channel");
        }
        // The node binding frame applies target_channel/read_latency. There is
        // deliberately no copy or destination storage for a direct operation.
        break;
    }
    case graph_jit::BackgroundRuntimeOperationKind::direct_event: {
        if (operation.operation >= plan_->storage.direct_events.size()) {
            result = std::unexpected(
                "background direct-event operation is out of range");
            break;
        }
        auto const& direct =
            plan_->storage.direct_events[operation.operation];
        if (!event_read(direct.storage)) {
            result = std::unexpected(
                "background direct-event operation has no source view");
        }
        // The binding frame applies the target history window directly to the
        // selected source view; direct delivery owns no derived event buffer.
        break;
    }
    case graph_jit::BackgroundRuntimeOperationKind::sample_materialization: {
        if (operation.operation
            >= plan_->storage.sample_materializations.size()) {
            result = std::unexpected(
                "background sample materialization is out of range");
            break;
        }
        auto const& materialization =
            plan_->storage.sample_materializations[operation.operation];
        auto const* target = sample_write(materialization.output);
        auto const& target_coverage =
            *slot(materialization.output).coverage;
        if (!target
            || target->layout.channel_type
                != materialization.target_layout.channel_type
            || materialization.output >= plan_->storage.ports.size()) {
            result = std::unexpected(
                "background sample materialization has no compatible target");
            break;
        }
        auto const geometry = plan_->storage.ports[materialization.output]
            .continuous_geometry;
        if (target->geometry != geometry
            || geometry.value_size_bytes == 0
            || geometry.element_alignment == 0
            || !std::has_single_bit(geometry.element_alignment)) {
            result = std::unexpected(
                "background continuous materialization has invalid target geometry");
            break;
        }
        if (materialization.inputs.size()
                != materialization.source_channels.size()
            || materialization.inputs.size()
                != materialization.source_read_latencies.size()) {
            result = std::unexpected(
                "background sample materialization inputs are not aligned");
            break;
        }

        struct RuntimeProjection {
            std::optional<ChannelConversionPlan> conversion{};
            std::vector<std::size_t> source_indices{};
            std::vector<std::size_t> target_channels{};
            std::vector<float> transform_coefficients{};
        };
        std::vector<RuntimeProjection> projections;
        auto append_projection = [&] (
            ChannelTypeId source_type,
            std::vector<std::size_t> source_indices,
            ChannelTypeId target_type,
            std::vector<std::size_t> target_channels,
            std::size_t transform_source_count = 0,
            std::size_t transform_target_count = 0,
            std::vector<float> transform_coefficients = {})
            -> std::expected<void, std::string> {
            auto const source_count = transform_coefficients.empty()
                ? channel_count(source_type)
                : transform_source_count;
            auto const target_count = transform_coefficients.empty()
                ? channel_count(target_type)
                : transform_target_count;
            if (source_indices.size() != source_count
                || target_channels.size() != target_count
                || (!transform_coefficients.empty()
                    && transform_coefficients.size()
                        != source_count * target_count)) {
                return std::unexpected(
                    "background sample projection has invalid channel arity");
            }
            for (auto const source_index : source_indices) {
                if (source_index >= materialization.inputs.size()) {
                    return std::unexpected(
                        "background sample projection has an invalid source");
                }
                auto const* source = sample_read(
                    materialization.inputs[source_index]);
                auto const channel =
                    materialization.source_channels[source_index].channel;
                if (!source || !source->has_channel(channel)) {
                    return std::unexpected(
                        "background sample projection has no source channel");
                }
            }
            if (!std::ranges::all_of(
                    target_channels,
                    [&](std::size_t channel) {
                        return target->has_channel(channel);
                    })) {
                return std::unexpected(
                    "background sample projection has no target channel");
            }
            RuntimeProjection projection{
                .source_indices = std::move(source_indices),
                .target_channels = std::move(target_channels),
                .transform_coefficients = std::move(transform_coefficients),
            };
            try {
                if (projection.transform_coefficients.empty()) {
                    projection.conversion = ChannelConversionRegistry::plan(
                        ChannelLayout{
                            .channel_type = source_type,
                            .sample_layout = SampleStreamLayout::interleaved,
                        },
                        ChannelLayout{
                            .channel_type = target_type,
                            .sample_layout = SampleStreamLayout::interleaved,
                        });
                }
            } catch (std::exception const& error) {
                return std::unexpected(
                    "background sample projection conversion is unsupported: "
                    + std::string(error.what()));
            }
            projections.push_back(std::move(projection));
            return {};
        };

        if (materialization.projections.empty()) {
            std::vector<std::size_t> source_indices(
                materialization.inputs.size());
            std::iota(source_indices.begin(), source_indices.end(), 0);
            result = append_projection(
                materialization.source_type,
                std::move(source_indices),
                materialization.target_layout.channel_type,
                materialization.target_channels);
        } else {
            for (auto const& projection : materialization.projections) {
                result = append_projection(
                    projection.source_type,
                    projection.source_channel_indices,
                    projection.target_type,
                    projection.target_channels,
                    projection.transform_source_channel_count,
                    projection.transform_target_channel_count,
                    projection.transform_coefficients);
                if (!result) break;
            }
        }
        if (!result) break;

        if (materialization.target_port >= plan_->ports.size()) {
            result = std::unexpected(
                "background sample materialization has no logical target port");
            break;
        }
        auto const& logical_target = plan_->ports[materialization.target_port];
        struct SourceRegionGroup {
            BackgroundSampleReadView const* view = nullptr;
            std::size_t latency = 0;
        };
        std::vector<SourceRegionGroup> source_groups;
        std::vector<std::size_t> input_group(materialization.inputs.size());
        for (std::size_t source = 0;
             source < materialization.inputs.size(); ++source) {
            auto const* view = sample_read(materialization.inputs[source]);
            if (!view || view->geometry != geometry) {
                result = std::unexpected(
                    "background continuous materialization source geometry disagrees with its target");
                break;
            }
            auto const latency = materialization.source_read_latencies[source];
            auto const found = std::ranges::find_if(
                source_groups,
                [&](SourceRegionGroup const& group) {
                    return group.view->regions.data() == view->regions.data()
                        && group.latency == latency;
                });
            if (found == source_groups.end()) {
                input_group[source] = source_groups.size();
                source_groups.push_back({view, latency});
            } else {
                input_group[source] = static_cast<std::size_t>(
                    std::distance(source_groups.begin(), found));
            }
        }
        if (!result) break;
        struct RegionContribution {
            GlobalIndexRange range{};
            std::size_t projection = 0;
            std::vector<std::optional<std::size_t>> source_regions{};
        };
        std::vector<RegionContribution> contributions;
        for (std::size_t projection_index = 0;
             projection_index < projections.size(); ++projection_index) {
            std::vector<std::size_t> projection_groups;
            for (auto const source_index :
                 projections[projection_index].source_indices) {
                auto const group = input_group[source_index];
                if (!std::ranges::contains(projection_groups, group)) {
                    projection_groups.push_back(group);
                }
            }

            std::vector<RegionContribution> projection_contributions;
            bool first_group = true;
            for (auto const group : projection_groups) {
                auto const& source = source_groups[group];
                if (first_group) {
                    for (std::size_t region = 0;
                         region < source.view->regions.size(); ++region) {
                        auto const range = source.view->regions[region].range;
                        RegionContribution contribution{
                            .range = {
                                .begin = range.begin.advanced_by(
                                    source.latency),
                                .extent = range.extent,
                            },
                            .projection = projection_index,
                            .source_regions = std::vector<
                                std::optional<std::size_t>>(
                                source_groups.size()),
                        };
                        contribution.source_regions[group] = region;
                        projection_contributions.push_back(
                            std::move(contribution));
                    }
                    first_group = false;
                    continue;
                }
                std::vector<RegionContribution> intersections;
                for (auto const& contribution : projection_contributions) {
                    for (std::size_t region = 0;
                         region < source.view->regions.size(); ++region) {
                        auto const raw = source.view->regions[region].range;
                        auto const shifted = GlobalIndexRange{
                            .begin = raw.begin.advanced_by(source.latency),
                            .extent = raw.extent,
                        };
                        for_each_range_intersection(
                            contribution.range,
                            shifted,
                            [&](GlobalIndexRange part) {
                                auto next = contribution;
                                next.range = part;
                                next.source_regions[group] = region;
                                intersections.push_back(std::move(next));
                            });
                    }
                }
                projection_contributions = std::move(intersections);
            }
            contributions.insert(
                contributions.end(),
                std::make_move_iterator(projection_contributions.begin()),
                std::make_move_iterator(projection_contributions.end()));
        }
        std::erase_if(
            contributions,
            [&](RegionContribution const& contribution) {
                return (IndexCoverage{contribution.range} & target_coverage)
                    .empty();
            });

        auto const target_channel_count = channel_count(target->layout);
        std::vector<AlignedByteBuffer> accumulated_target_values;
        std::vector<AlignedByteBuffer> projected_target_values;
        accumulated_target_values.reserve(target_channel_count);
        projected_target_values.reserve(target_channel_count);
        for (std::size_t channel = 0; channel < target_channel_count;
             ++channel) {
            accumulated_target_values.emplace_back(
                geometry.value_size_bytes, geometry.element_alignment);
            projected_target_values.emplace_back(
                geometry.value_size_bytes, geometry.element_alignment);
        }
        auto source_value_storage = [&] (
            ContinuousRegionStorageView const& region,
            GlobalIndex value_index,
            std::size_t channel) -> std::byte const* {
            auto const offset = region.range.offset_of(value_index);
            if (!offset || channel >= channel_count(region.channel_layout)) {
                return nullptr;
            }
            auto const frames = region.frame_count();
            auto const channels = channel_count(region.channel_layout);
            auto const value = region.channel_layout.sample_layout
                    == SampleStreamLayout::planar
                ? channel * frames + static_cast<std::size_t>(*offset)
                : static_cast<std::size_t>(*offset) * channels + channel;
            return region.storage + value * geometry.value_size_bytes;
        };
        auto write_target_value = [&] (
            ContinuousRegionStorageView const& region,
            GlobalIndex value_index,
            std::size_t channel,
            std::byte const* value) {
            auto const offset = region.range.offset_of(value_index);
            if (!offset || channel >= channel_count(region.channel_layout)) {
                return false;
            }
            auto const frames = region.frame_count();
            auto const channels = channel_count(region.channel_layout);
            auto const native_index = region.channel_layout.sample_layout
                    == SampleStreamLayout::planar
                ? channel * frames + static_cast<std::size_t>(*offset)
                : static_cast<std::size_t>(*offset) * channels + channel;
            std::memcpy(
                region.storage + native_index * geometry.value_size_bytes,
                value,
                geometry.value_size_bytes);
            if (region.written_values) region.written_values[native_index] = 1;
            return true;
        };
        auto initialize_accumulated_values = [&] {
            for (auto const channel : materialization.target_channels) {
                if (channel >= accumulated_target_values.size()) return false;
                auto& destination = accumulated_target_values[channel];
                if (logical_target.input_access == InputAccessKind::sequential) {
                    if (initialize_sequential_input_) {
                        if (!initialize_sequential_input_(
                            materialization.target_port,
                            destination.data(),
                            destination.size())) {
                            return false;
                        }
                    } else if (geometry.value_type
                            == continuous_value_type_id_v<Sample>
                        && destination.size() == sizeof(Sample)) {
                        std::memcpy(
                            destination.data(),
                            &logical_target.sample_neutral_value,
                            sizeof(Sample));
                    } else {
                        return false;
                    }
                } else {
                    std::fill_n(
                        destination.data(), destination.size(), std::byte{});
                }
            }
            return true;
        };
        auto combine_projected_value = [&] (
            std::size_t channel,
            std::byte const* source) {
            auto& destination = accumulated_target_values[channel];
            if (logical_target.input_access == InputAccessKind::sequential) {
                return combine_sequential_input_
                    && combine_sequential_input_(
                        materialization.target_port,
                        destination.data(),
                        source,
                        destination.size());
            }
            if (geometry.channel_scalar_lane_count == 0) {
                std::memcpy(
                    destination.data(), source, geometry.value_size_bytes);
                return true;
            }
            if (geometry.value_size_bytes
                != geometry.channel_scalar_lane_count * sizeof(float)) {
                return false;
            }
            auto destination_lanes = std::span{
                reinterpret_cast<float*>(destination.data()),
                geometry.channel_scalar_lane_count};
            auto source_lanes = std::span{
                reinterpret_cast<float const*>(source),
                geometry.channel_scalar_lane_count};
            for (std::size_t lane = 0; lane < destination_lanes.size(); ++lane) {
                destination_lanes[lane] += source_lanes[lane];
            }
            return true;
        };
        auto apply_contribution = [&](RegionContribution const& contribution,
                                      GlobalIndex sample_index) {
            if (contribution.projection >= projections.size()) {
                result = std::unexpected(
                    "background sample contribution lost its projection");
                return;
            }
            auto const& projection = projections[contribution.projection];
            std::vector<std::byte const*> source_storage(
                projection.source_indices.size());
            for (std::size_t channel = 0;
                 channel < projection.source_indices.size(); ++channel) {
                auto const source_index = projection.source_indices[channel];
                auto const group_index = input_group[source_index];
                auto const& group = source_groups[group_index];
                auto const latency =
                    materialization.source_read_latencies[source_index];
                auto const source_sample_index =
                    sample_index.retreated_by(latency);
                if (group_index >= contribution.source_regions.size()
                    || !contribution.source_regions[group_index]
                    || *contribution.source_regions[group_index]
                        >= group.view->regions.size()) {
                    result = std::unexpected(
                        "background sample contribution lost its source Region");
                    return;
                }
                auto const& source_region = group.view->regions[
                    *contribution.source_regions[group_index]];
                source_storage[channel] = source_value_storage(
                    source_region,
                    source_sample_index,
                    materialization.source_channels[source_index].channel);
                if (!source_storage[channel]) {
                    result = std::unexpected(
                        "background sample source Region does not cover its delayed read");
                    return;
                }
            }
            if (!result) return;
            for (auto const channel : projection.target_channels) {
                std::fill_n(
                    projected_target_values[channel].data(),
                    geometry.value_size_bytes,
                    std::byte{});
            }
            if (geometry.channel_scalar_lane_count == 0) {
                if (source_storage.size() != 1
                    || projection.target_channels.size() != 1) {
                    result = std::unexpected(
                        "scalar continuous materialization cannot apply a channel projection");
                    return;
                }
                std::memcpy(
                    projected_target_values[
                        projection.target_channels.front()].data(),
                    source_storage.front(),
                    geometry.value_size_bytes);
            } else {
                if (geometry.value_size_bytes
                    != geometry.channel_scalar_lane_count * sizeof(float)) {
                    result = std::unexpected(
                        "audio continuous geometry has an invalid scalar-lane extent");
                    return;
                }
                std::array<Sample, maximum_supported_channel_count>
                    source_values{};
                std::array<Sample, maximum_supported_channel_count>
                    target_values{};
                for (std::size_t lane = 0;
                     lane < geometry.channel_scalar_lane_count; ++lane) {
                    for (std::size_t source_channel = 0;
                         source_channel < projection.source_indices.size();
                         ++source_channel) {
                        source_values[source_channel] = Sample{
                            reinterpret_cast<float const*>(
                                source_storage[source_channel])[lane]};
                    }
                    if (projection.conversion) {
                        projection.conversion->convert(
                            source_values.data(), target_values.data(), 1);
                    } else {
                        for (std::size_t target_channel = 0;
                             target_channel
                                < projection.target_channels.size();
                             ++target_channel) {
                            Sample value{};
                            for (std::size_t source_channel = 0;
                                 source_channel
                                    < projection.source_indices.size();
                                 ++source_channel) {
                                value += source_values[source_channel]
                                    * projection.transform_coefficients[
                                        target_channel
                                            * projection.source_indices.size()
                                        + source_channel];
                            }
                            target_values[target_channel] = value;
                        }
                    }
                    for (std::size_t target_channel = 0;
                         target_channel < projection.target_channels.size();
                         ++target_channel) {
                        reinterpret_cast<float*>(
                            projected_target_values[
                                projection.target_channels[target_channel]]
                                .data())[lane]
                            = target_values[target_channel].value;
                    }
                }
            }
            for (auto const target_channel : projection.target_channels) {
                if (!combine_projected_value(
                        target_channel,
                        projected_target_values[target_channel].data())) {
                    result = std::unexpected(
                        "background sample materialization combiner rejected a contribution");
                    return;
                }
            }
        };

        std::vector<bool> used_contributions(contributions.size(), false);
        for (std::size_t target_region_index = 0;
             target_region_index < target->regions.size();
             ++target_region_index) {
            auto const target_region = target->regions[target_region_index].range;
            std::optional<std::size_t> fixed_contribution;
            if (logical_target.input_access != InputAccessKind::sequential) {
                for (std::size_t contribution = 0;
                     contribution < contributions.size(); ++contribution) {
                    if (!used_contributions[contribution]
                        && contributions[contribution].range == target_region) {
                        fixed_contribution = contribution;
                        used_contributions[contribution] = true;
                        break;
                    }
                }
                if (!fixed_contribution) {
                    result = std::unexpected(
                        "background Coverage materialization lost a logical Region contribution");
                    break;
                }
            }
            for (GlobalIndex::extent_type frame = 0;
                 frame < target_region.extent; ++frame) {
                auto const sample_index = target_region.begin.advanced_by(frame);
                if (!initialize_accumulated_values()) {
                    result = std::unexpected(
                        "background continuous materialization neutral initializer rejected its target geometry");
                    break;
                }
                if (fixed_contribution) {
                    apply_contribution(
                        contributions[*fixed_contribution], sample_index);
                } else {
                    for (auto const& contribution : contributions) {
                        if (contribution.range.contains(sample_index)) {
                            apply_contribution(contribution, sample_index);
                            if (!result) break;
                        }
                    }
                }
                if (!result) break;
                for (auto const channel : materialization.target_channels) {
                    if (!write_target_value(
                            target->regions[target_region_index],
                            sample_index,
                            channel,
                            accumulated_target_values[channel].data())) {
                        result = std::unexpected(
                            "background sample materialization write failed");
                        break;
                    }
                }
                if (!result) break;
            }
            if (!result) break;
        }
        break;
    }
    case graph_jit::BackgroundRuntimeOperationKind::event_materialization: {
        if (operation.operation
            >= plan_->storage.event_materializations.size()) {
            result = std::unexpected(
                "background event materialization is out of range");
            break;
        }
        auto const& materialization =
            plan_->storage.event_materializations[operation.operation];
        auto const* target = event_write(materialization.output);
        auto const& target_coverage =
            *slot(materialization.output).coverage;
        if (!target || target->type != materialization.target_type) {
            result = std::unexpected(
                "background event materialization has no compatible target");
            break;
        }
        if (materialization.inputs.empty()) {
            result = std::unexpected(
                "background event materialization has no sources");
            break;
        }
        if (materialization.source_conversions.size()
                != materialization.inputs.size()
            || materialization.semantic_source_indices.size()
                != materialization.inputs.size()
            || materialization.conversion.source_type
                != materialization.source_type
            || materialization.conversion.target_type
                != materialization.target_type
            || materialization.conversion.step_count
                > EventConversionPlan::max_steps
            || !EventConversionRegistry::is_nonexpanding(
                materialization.conversion)) {
            result = std::unexpected(
                "background event materialization has an invalid conversion");
            break;
        }
        if (materialization.target_port
            && *materialization.target_port >= plan_->ports.size()) {
            result = std::unexpected(
                "background event materialization has no logical target port");
            break;
        }

        if (materialization.target_port
            && *materialization.target_port < plan_->ports.size()
            && plan_->ports[*materialization.target_port].input_access
                != InputAccessKind::sequential) {
            struct Contribution {
                std::size_t source = 0;
                std::size_t region = 0;
                GlobalIndexRange range{};
            };
            std::vector<Contribution> contributions;
            for (std::size_t source_index = 0;
                 source_index < materialization.inputs.size(); ++source_index) {
                auto const* source = event_read(
                    materialization.inputs[source_index]);
                auto const& conversion =
                    materialization.source_conversions[source_index];
                if (!source || source->type != conversion.source_type
                    || conversion.target_type != materialization.source_type
                    || conversion.step_count > EventConversionPlan::max_steps
                    || !EventConversionRegistry::is_nonexpanding(conversion)
                    || source->for_each_region_event == nullptr) {
                    result = std::unexpected(
                        "background event Region materialization has no compatible source");
                    break;
                }
                for (std::size_t region = 0;
                     region < source->regions.size(); ++region) {
                    for (auto const available : target_coverage) {
                        for_each_range_intersection(
                            source->regions[region],
                            available,
                            [&](GlobalIndexRange part) {
                                contributions.push_back({
                                    .source = source_index,
                                    .region = region,
                                    .range = part,
                                });
                            });
                    }
                }
            }
            if (!result) break;
            std::vector<bool> used(contributions.size(), false);
            for (std::size_t target_region = 0;
                 target_region < target->regions.size(); ++target_region) {
                auto const found = [&]() -> std::optional<std::size_t> {
                    for (std::size_t index = 0;
                         index < contributions.size(); ++index) {
                        if (!used[index]
                            && contributions[index].range
                                == target->regions[target_region]) {
                            return index;
                        }
                    }
                    return std::nullopt;
                }();
                if (!found) {
                    result = std::unexpected(
                        "background event Coverage materialization lost a logical Region contribution");
                    break;
                }
                used[*found] = true;
                auto const contribution = contributions[*found];
                auto const* source = event_read(
                    materialization.inputs[contribution.source]);
                auto const source_index =
                    materialization.semantic_source_indices[
                        contribution.source];
                bool written = true;
                source->for_each_region(
                    contribution.region,
                    contribution.range,
                    [&](TimedEvent const& event) {
                        EventConversionRegistry::instance().convert(
                            materialization.source_conversions[
                                contribution.source],
                            event,
                            [&](TimedEvent const& source_converted) {
                                EventConversionRegistry::instance().convert(
                                    materialization.conversion,
                                    source_converted,
                                    [&](TimedEvent const& target_converted) {
                                        written = written && target->write(
                                            target_region,
                                            target_converted,
                                            source_index);
                                    });
                            });
                    });
                if (!written) {
                    result = std::unexpected(
                        "background event Region materialization write failed");
                    break;
                }
            }
            if (result && std::ranges::contains(used, false)) {
                result = std::unexpected(
                    "background event Coverage materialization has an unused logical Region contribution");
            }
            break;
        }

        std::vector<std::vector<TimedEvent>> converted(
            materialization.inputs.size());
        std::size_t total_count = 0;
        for (std::size_t source_index = 0;
             source_index < materialization.inputs.size(); ++source_index) {
            auto const* source = event_read(
                materialization.inputs[source_index]);
            auto const& source_conversion =
                materialization.source_conversions[source_index];
            if (!source || source->type != source_conversion.source_type
                || source_conversion.target_type
                    != materialization.source_type
                || source_conversion.step_count
                    > EventConversionPlan::max_steps
                || !EventConversionRegistry::is_nonexpanding(
                    source_conversion)) {
                result = std::unexpected(
                    "background event materialization has no compatible source");
                break;
            }
            std::vector<TimedEvent> source_events;
            bool ordered = true;
            for_each_intersection(
                *slot(materialization.inputs[source_index]).coverage,
                target_coverage,
                [&](GlobalIndexRange region) {
                    source->for_each(region, [&](TimedEvent const& event) {
                        if (!source_events.empty()
                            && SignedGlobalIndexOrder{}(
                                event.time, source_events.back().time)) {
                            ordered = false;
                        }
                        source_events.push_back(event);
                    });
                });
            if (!ordered) {
                result = std::unexpected(
                    "background event source is not time ordered");
                break;
            }
            converted[source_index].resize(source_events.size());
            if (!source_events.empty()) {
                auto const& conversion = source_conversion;
                auto const written = graph_jit::detail::
                    iv_graph_jit_convert_event_sequence(
                        static_cast<std::uint32_t>(conversion.source_type),
                        static_cast<std::uint32_t>(conversion.target_type),
                        static_cast<std::uint32_t>(conversion.steps[0]),
                        static_cast<std::uint32_t>(conversion.steps[1]),
                        static_cast<std::uint32_t>(conversion.steps[2]),
                        conversion.step_count,
                        source_events.data(),
                        source_events.size(),
                        converted[source_index].data(),
                        converted[source_index].size());
                converted[source_index].resize(written);
            }
            if (converted[source_index].size()
                > std::numeric_limits<std::size_t>::max() - total_count) {
                result = std::unexpected(
                    "background event materialization is too large");
                break;
            }
            total_count += converted[source_index].size();
        }
        if (!result) break;

        auto capacity = event_merge_capacity(total_count);
        if (!capacity) {
            result = std::unexpected(std::move(capacity.error()));
            break;
        }
        std::vector<TimedEvent> merged(*capacity);
        std::vector<std::size_t> merged_source_indices(*capacity);
        auto const merge_origin = target_coverage.empty()
            ? GlobalIndex{}
            : target_coverage.begin()->begin;
        std::size_t merged_count = 0;
        for (std::size_t source = 0; source < converted.size(); ++source) {
            merged_count = graph_jit::detail::
                iv_graph_jit_merge_ordered_event_sequence(
                merged.data(),
                merged_source_indices.data(),
                merged.size(), 0, merged_count,
                converted[source].data(),
                converted[source].size(),
                converted[source].size(),
                materialization.semantic_source_indices[source],
                merge_origin);
        }
        if (merged_count != total_count) {
            result = std::unexpected(
                "background event materialization merge failed");
            break;
        }
        auto const& conversion = materialization.conversion;
        for (std::size_t event = 0; event < merged_count; ++event) {
            bool written = true;
            EventConversionRegistry::instance().convert(
                conversion,
                merged[event],
                [&](TimedEvent const& converted_event) {
                    written = written && target->write(
                        converted_event, merged_source_indices[event]);
                });
            if (!written) {
                result = std::unexpected(
                    "background event materialization write failed");
                break;
            }
        }
        break;
    }
    }

    if (result) executed_operations_[index] = true;
    return result;
}

std::expected<void, std::string>
BackgroundStorageRealization::validate_produced_storage() const
{
    if (!sealed_) {
        return std::unexpected(
            "background storage realization is not sealed");
    }
    for (graph_jit::PortStorageIndex index = 0; index < slots_.size(); ++index) {
        auto const& selected = *slots_[index];
        if (selected.owned_sample && !selected.owned_sample->complete()) {
            return std::unexpected(
                "background continuous output did not initialize every "
                "selected value and channel");
        }
    }
    return {};
}

std::expected<std::unique_ptr<TickMaterializationSnapshot>, std::string>
BackgroundStorageRealization::make_tick_materialization_snapshot(
    std::uint64_t generation,
    std::uint64_t semantic_version,
    PersistedDataVersion data_version) const
{
    if (auto complete = validate_produced_storage(); !complete) {
        return std::unexpected(std::move(complete.error()));
    }

    struct SampleRoute {
        BackgroundSampleReadView const* view = nullptr;
        std::size_t logical_channel = 0;
        std::size_t storage_channel = 0;
        std::size_t latency = 0;
    };
    auto materialized_tick_storage = [&](graph_jit::PortStorageIndex index) {
        return index < plan_->storage.ports.size()
            && (plan_->storage.ports[index].storage
                    == graph_jit::PortStorageKind::tick_sequential
                || plan_->storage.ports[index].storage
                    == graph_jit::PortStorageKind::tick_random_access);
    };

    struct TickBindingView {
        graph_jit::BackgroundPortIndex port = 0;
        std::span<graph_jit::PortStorageIndex const> storage{};
    };
    std::vector<TickBindingView> sample_bindings;
    sample_bindings.reserve(
        plan_->tick_runtime.sequential_sample_inputs.size()
        + plan_->tick_runtime.random_access_sample_inputs.size());
    for (auto const& binding :
         plan_->tick_runtime.sequential_sample_inputs) {
        sample_bindings.push_back({binding.port, binding.storage});
    }
    for (auto const& binding :
         plan_->tick_runtime.random_access_sample_inputs) {
        sample_bindings.push_back({binding.port, binding.storage});
    }
    std::vector<TickBindingView> event_bindings;
    event_bindings.reserve(
        plan_->tick_runtime.sequential_event_inputs.size()
        + plan_->tick_runtime.random_access_event_inputs.size());
    for (auto const& binding :
         plan_->tick_runtime.sequential_event_inputs) {
        event_bindings.push_back({binding.port, binding.storage});
    }
    for (auto const& binding :
         plan_->tick_runtime.random_access_event_inputs) {
        event_bindings.push_back({binding.port, binding.storage});
    }

    std::vector<TickMaterializedSampleInput> samples;
    for (auto const& binding : sample_bindings) {
        if (binding.port >= plan_->ports.size()) {
            return std::unexpected(
                "Tick sample materialization references a missing input port");
        }
        auto const& port = plan_->ports[binding.port];
        std::vector<SampleRoute> routes;
        auto append_route = [&](SampleRoute route) {
            if (std::ranges::none_of(routes, [&](SampleRoute const& existing) {
                    return existing.view->regions.data()
                            == route.view->regions.data()
                        && existing.view->coverage_value
                            == route.view->coverage_value
                        && existing.logical_channel == route.logical_channel
                        && existing.storage_channel == route.storage_channel
                        && existing.latency == route.latency;
                })) {
                routes.push_back(route);
            }
        };
        for (auto const& direct : plan_->storage.direct_samples) {
            if (!materialized_tick_storage(direct.storage)
                || !std::ranges::contains(binding.storage, direct.storage)
                || direct.target_subset
                    >= plan_->sample_target_subsets.size()
                || plan_->sample_target_subsets[direct.target_subset].port
                    != port.configured_port) {
                continue;
            }
            auto const* view = sample_read(direct.storage);
            if (view) {
                append_route({
                    .view = view,
                    .logical_channel = direct.target_channel,
                    .storage_channel = direct.source_channel,
                    .latency = direct.read_latency,
                });
            }
        }
        for (auto const storage_index : binding.storage) {
            if (!materialized_tick_storage(storage_index)) continue;
            auto const* view = sample_read(storage_index);
            if (!view) continue;
            auto const& storage = plan_->storage.ports[storage_index];
            for (auto const subset : storage.target_subsets) {
                if (subset >= plan_->sample_target_subsets.size()) continue;
                auto const& target = plan_->sample_target_subsets[subset];
                if (target.port != port.configured_port) continue;
                for (auto const channel : target.channels) {
                    if (view->has_channel(channel.channel)) {
                        append_route({
                            .view = view,
                            .logical_channel = channel.channel,
                            .storage_channel = channel.channel,
                        });
                    }
                }
            }
        }
        auto const channels = channel_count(port.sample_layout);
        struct FrozenSourceGroup {
            BackgroundSampleReadView const* view = nullptr;
            std::size_t latency = 0;
            std::vector<std::size_t> routes{};
        };
        std::vector<FrozenSourceGroup> groups;
        for (std::size_t route_index = 0;
             route_index < routes.size(); ++route_index) {
            auto const& route = routes[route_index];
            auto found = std::ranges::find_if(
                groups,
                [&](FrozenSourceGroup const& group) {
                    return group.view->regions.data()
                            == route.view->regions.data()
                        && group.latency == route.latency;
                });
            if (found == groups.end()) {
                groups.push_back({
                    .view = route.view,
                    .latency = route.latency,
                    .routes = {route_index},
                });
            } else {
                found->routes.push_back(route_index);
            }
        }
        auto group_has_channel = [&](FrozenSourceGroup const& group,
                                     std::size_t channel) {
            return std::ranges::any_of(
                group.routes,
                [&](std::size_t route) {
                    return routes[route].logical_channel == channel;
                });
        };
        auto group_is_complete = [&](FrozenSourceGroup const& group) {
            for (std::size_t channel = 0; channel < channels; ++channel) {
                if (!group_has_channel(group, channel)) return false;
            }
            return true;
        };
        constexpr auto no_region = std::numeric_limits<std::size_t>::max();
        struct FrozenRegion {
            GlobalIndexRange range{};
            std::vector<std::size_t> source_regions{};
        };
        std::vector<FrozenRegion> frozen_regions;
        std::vector<std::size_t> incomplete_groups;
        for (std::size_t group = 0; group < groups.size(); ++group) {
            if (!group_is_complete(groups[group])) {
                incomplete_groups.push_back(group);
                continue;
            }
            for (std::size_t region = 0;
                 region < groups[group].view->regions.size(); ++region) {
                auto selected = std::vector<std::size_t>(
                    groups.size(), no_region);
                selected[group] = region;
                auto const raw = groups[group].view->regions[region].range;
                frozen_regions.push_back({
                    .range = {
                        .begin = raw.begin.advanced_by(groups[group].latency),
                        .extent = raw.extent,
                    },
                    .source_regions = std::move(selected),
                });
            }
        }
        if (!incomplete_groups.empty()) {
            for (std::size_t channel = 0; channel < channels; ++channel) {
                if (std::ranges::none_of(
                        incomplete_groups,
                        [&](std::size_t group) {
                            return group_has_channel(groups[group], channel);
                        })) {
                    return std::unexpected(
                        "Tick sample materialization has incomplete routed channels");
                }
            }
            std::vector<FrozenRegion> composed;
            bool first = true;
            for (auto const group : incomplete_groups) {
                auto const& source = groups[group];
                if (first) {
                    for (std::size_t region = 0;
                         region < source.view->regions.size(); ++region) {
                        auto selected = std::vector<std::size_t>(
                            groups.size(), no_region);
                        selected[group] = region;
                        auto const raw = source.view->regions[region].range;
                        composed.push_back({
                            .range = {
                                .begin = raw.begin.advanced_by(source.latency),
                                .extent = raw.extent,
                            },
                            .source_regions = std::move(selected),
                        });
                    }
                    first = false;
                    continue;
                }
                std::vector<FrozenRegion> intersections;
                for (auto const& previous : composed) {
                    for (std::size_t region = 0;
                         region < source.view->regions.size(); ++region) {
                        auto const raw = source.view->regions[region].range;
                        auto const shifted = GlobalIndexRange{
                            .begin = raw.begin.advanced_by(source.latency),
                            .extent = raw.extent,
                        };
                        for_each_range_intersection(
                            previous.range,
                            shifted,
                            [&](GlobalIndexRange part) {
                                auto next = previous;
                                next.range = part;
                                next.source_regions[group] = region;
                                intersections.push_back(std::move(next));
                            });
                    }
                }
                composed = std::move(intersections);
            }
            frozen_regions.insert(
                frozen_regions.end(),
                std::make_move_iterator(composed.begin()),
                std::make_move_iterator(composed.end()));
        }
        if (port.ordering == CoverageOrdering::sorted) {
            std::stable_sort(
                frozen_regions.begin(),
                frozen_regions.end(),
                [](FrozenRegion const& left, FrozenRegion const& right) {
                    if (left.range.begin == right.range.begin) {
                        return left.range.extent > right.range.extent;
                    }
                    return SignedGlobalIndexOrder{}(
                        left.range.begin, right.range.begin);
                });
        }
        if (frozen_regions.empty()) continue;

        std::vector<std::size_t> frozen_channels(channels);
        std::iota(frozen_channels.begin(), frozen_channels.end(), 0);
        auto const geometry = port.continuous_geometry;
        if (geometry.value_size_bytes == 0
            || std::ranges::any_of(
                groups,
                [&](FrozenSourceGroup const& group) {
                    return group.view->geometry != geometry;
                })) {
            return std::unexpected(
                "Tick continuous materialization has inconsistent source geometry");
        }
        std::vector<std::byte> frozen_values;
        std::size_t frozen_frames = 0;
        for (auto const& region : frozen_regions) {
            if (region.range.extent
                > std::numeric_limits<std::size_t>::max() - frozen_frames) {
                return std::unexpected(
                    "Tick sample materialization is too large");
            }
            frozen_frames += static_cast<std::size_t>(region.range.extent);
        }
        frozen_values.reserve(checked_product(
            checked_product(frozen_frames, channels),
            geometry.value_size_bytes));
        bool missing_route = false;
        auto append_value = [&](FrozenRegion const& region,
                                GlobalIndex sample,
                                std::size_t channel) {
            for (std::size_t group = 0; group < groups.size(); ++group) {
                if (region.source_regions[group] == no_region) continue;
                auto const route = std::ranges::find_if(
                    groups[group].routes,
                    [&](std::size_t route_index) {
                        return routes[route_index].logical_channel == channel;
                    });
                if (route == groups[group].routes.end()) continue;
                auto const& selected_route = routes[*route];
                auto const source_index = sample.retreated_by(
                    selected_route.latency);
                auto const& source_region = groups[group].view->regions[
                    region.source_regions[group]];
                auto const source_frame =
                    source_region.range.offset_of(source_index);
                if (!source_frame) continue;
                auto const source_channels = channel_count(
                    source_region.channel_layout);
                if (selected_route.storage_channel >= source_channels) continue;
                auto const source_value =
                    source_region.channel_layout.sample_layout
                            == SampleStreamLayout::planar
                        ? selected_route.storage_channel
                                * source_region.frame_count()
                            + static_cast<std::size_t>(*source_frame)
                        : static_cast<std::size_t>(*source_frame)
                                * source_channels
                            + selected_route.storage_channel;
                auto const* begin = source_region.storage
                    + source_value * geometry.value_size_bytes;
                frozen_values.insert(
                    frozen_values.end(),
                    begin,
                    begin + geometry.value_size_bytes);
                return;
            }
            missing_route = true;
        };
        for (auto const& region : frozen_regions) {
            if (port.sample_layout.sample_layout
                == SampleStreamLayout::planar) {
                for (auto const channel : frozen_channels) {
                    for (GlobalIndex::extent_type frame = 0;
                         frame < region.range.extent; ++frame) {
                        append_value(
                            region,
                            region.range.begin.advanced_by(frame),
                            channel);
                        if (missing_route) break;
                    }
                    if (missing_route) break;
                }
            } else {
                for (GlobalIndex::extent_type frame = 0;
                     frame < region.range.extent; ++frame) {
                    auto const sample = region.range.begin.advanced_by(frame);
                    for (auto const channel : frozen_channels) {
                        append_value(region, sample, channel);
                        if (missing_route) break;
                    }
                    if (missing_route) break;
                }
            }
            if (missing_route) break;
        }
        if (missing_route) {
            return std::unexpected(
                "Tick sample materialization has incomplete routed coverage");
        }
        std::vector<GlobalIndexRange> ranges;
        ranges.reserve(frozen_regions.size());
        for (auto const& region : frozen_regions) {
            ranges.push_back(region.range);
        }
        if (geometry.value_type == continuous_value_type_id_v<Sample>) {
            if (frozen_values.size() % sizeof(Sample) != 0) {
                return std::unexpected(
                    "Tick Sample materialization has invalid byte geometry");
            }
            std::vector<Sample> values(
                frozen_values.size() / sizeof(Sample));
            std::memcpy(
                values.data(), frozen_values.data(), frozen_values.size());
            samples.push_back(make_tick_materialized_sample_input(
                binding.port,
                port.sample_layout,
                std::move(frozen_channels),
                std::move(ranges),
                std::move(values)));
        } else {
            samples.push_back(make_tick_materialized_continuous_input(
                binding.port,
                port.sample_layout,
                geometry,
                std::move(frozen_channels),
                std::move(ranges),
                std::move(frozen_values)));
        }
    }

    std::vector<TickMaterializedEventInput> events;
    for (auto const& binding : event_bindings) {
        if (binding.port >= plan_->ports.size()) {
            return std::unexpected(
                "Tick event materialization references a missing input port");
        }
        auto const& port = plan_->ports[binding.port];
        struct EventRoute {
            BackgroundEventReadView const* view = nullptr;
            std::optional<std::size_t> fixed_source_index{};
        };
        std::vector<EventRoute> routes;
        auto append_route = [&] (
            BackgroundEventReadView const* view,
            std::optional<std::size_t> fixed_source_index = {}) {
            if (!view || view->type != port.event_type) return;
            auto existing = std::ranges::find_if(
                routes,
                [&](EventRoute const& candidate) {
                    return candidate.view->data == view->data;
                });
            if (existing != routes.end()) {
                if (!existing->fixed_source_index && fixed_source_index) {
                    existing->fixed_source_index = fixed_source_index;
                }
                return;
            }
            routes.push_back({view, fixed_source_index});
        };
        for (auto const& direct : plan_->storage.direct_events) {
            if (!materialized_tick_storage(direct.storage)
                || !std::ranges::contains(binding.storage, direct.storage)
                || direct.target_subset >= plan_->event_target_subsets.size()) {
                continue;
            }
            auto const& target =
                plan_->event_target_subsets[direct.target_subset].port;
            if (target.bundle == port.configured_port.node_bundle_handle
                && target.port == port.configured_port.port_index) {
                std::optional<std::size_t> semantic_source_index;
                if (direct.connection < plan_->connections.size()
                    && direct.source_subset
                        < plan_->event_source_subsets.size()) {
                    auto const source = plan_->event_source_subsets[
                        direct.source_subset].port;
                    auto const& connection =
                        plan_->connections[direct.connection];
                    auto const delivery = std::ranges::find_if(
                        connection.event_deliveries,
                        [&](graph_jit::EventDeliveryPlan const& candidate) {
                            return candidate.source == source
                                && candidate.target == target;
                        });
                    if (delivery != connection.event_deliveries.end()) {
                        semantic_source_index =
                            delivery->semantic_source_index;
                    }
                }
                append_route(
                    event_read(direct.storage), semantic_source_index);
            }
        }
        for (auto const storage_index : binding.storage) {
            if (!materialized_tick_storage(storage_index)) continue;
            auto const& storage = plan_->storage.ports[storage_index];
            for (auto const subset : storage.target_subsets) {
                if (subset >= plan_->event_target_subsets.size()) continue;
                auto const& target = plan_->event_target_subsets[subset].port;
                if (target.bundle == port.configured_port.node_bundle_handle
                    && target.port == port.configured_port.port_index) {
                    append_route(event_read(storage_index));
                }
            }
        }
        IndexCoverage available;
        for (auto const& route : routes) {
            available.include(route.view->coverage());
        }
        if (available.empty()) continue;
        if (port.input_access != InputAccessKind::sequential) {
            struct FrozenRegion {
                GlobalIndexRange range{};
                std::vector<TimedEvent> events{};
                std::vector<std::size_t> source_indices{};
            };
            std::vector<FrozenRegion> frozen_regions;
            for (auto const& route : routes) {
                if (route.view->for_each_region_event == nullptr) {
                    return std::unexpected(
                        "Tick event materialization source has no Region reader");
                }
                for (std::size_t region = 0;
                     region < route.view->regions.size(); ++region) {
                    auto const range = route.view->regions[region];
                    if ((IndexCoverage{range} & available).empty()) continue;
                    FrozenRegion frozen{.range = range};
                    route.view->for_each_region(
                        region, range, [&](TimedEvent const& event) {
                        frozen.events.push_back(event);
                        frozen.source_indices.push_back(
                            route.fixed_source_index.value_or(
                                std::numeric_limits<std::size_t>::max()));
                    });
                    frozen_regions.push_back(std::move(frozen));
                }
            }
            if (port.ordering == CoverageOrdering::sorted) {
                std::stable_sort(
                    frozen_regions.begin(),
                    frozen_regions.end(),
                    [](FrozenRegion const& left, FrozenRegion const& right) {
                        if (left.range.begin == right.range.begin) {
                            return left.range.extent > right.range.extent;
                        }
                        return SignedGlobalIndexOrder{}(
                            left.range.begin, right.range.begin);
                    });
            }
            if (port.input_access == InputAccessKind::region
                && frozen_regions.size() != 1) {
                return std::unexpected(
                    "Tick event Region input does not resolve to one Region");
            }
            std::vector<GlobalIndexRange> ranges;
            std::vector<std::vector<TimedEvent>> region_events;
            std::vector<std::vector<std::size_t>> region_indices;
            ranges.reserve(frozen_regions.size());
            region_events.reserve(frozen_regions.size());
            region_indices.reserve(frozen_regions.size());
            for (auto& region : frozen_regions) {
                ranges.push_back(region.range);
                region_events.push_back(std::move(region.events));
                region_indices.push_back(std::move(region.source_indices));
            }
            events.push_back(make_tick_materialized_event_input(
                binding.port,
                port.event_type,
                std::move(ranges),
                std::move(region_events),
                std::move(region_indices)));
            continue;
        }
        std::vector<TimedEvent> frozen_events;
        std::vector<std::size_t> frozen_indices;
        for (auto const region : available) {
            GlobalIndex::extent_type consumed = 0;
            while (consumed < region.extent) {
                auto const cursor = region.begin.advanced_by(consumed);
                EventRoute const* selected = nullptr;
                GlobalIndex::extent_type selected_extent = 0;
                for (auto const& route : routes) {
                    for (auto const covered : route.view->coverage()) {
                        auto const offset = covered.offset_of(cursor);
                        if (!offset) continue;
                        selected = std::addressof(route);
                        selected_extent = std::min(
                            region.extent - consumed,
                            covered.extent - *offset);
                        break;
                    }
                    if (selected) break;
                }
                if (!selected || selected_extent == 0) {
                    return std::unexpected(
                        "Tick event materialization has incomplete routed coverage");
                }
                selected->view->for_each_indexed(
                    {cursor, selected_extent},
                    [&](TimedEvent const& event, std::size_t source_index) {
                        frozen_events.push_back(event);
                        frozen_indices.push_back(
                            selected->fixed_source_index.value_or(
                                source_index));
                    });
                consumed += selected_extent;
            }
        }
        events.push_back(make_tick_materialized_event_input(
            binding.port,
            port.event_type,
            std::move(available),
            std::move(frozen_events),
            std::move(frozen_indices)));
    }

    return std::make_unique<TickMaterializationSnapshot>(
        generation,
        semantic_version,
        data_version,
        std::move(samples),
        std::move(events));
}

std::expected<void, std::string>
BackgroundStorageRealization::stage_persisted_data(
    PersistedDataStore::Candidate& candidate) const
{
    if (auto complete = validate_produced_storage(); !complete)
        return complete;

    struct StagedRange {
        PersistedOutputHandle output{};
        graph_jit::PortStorageIndex storage = 0;
        GlobalIndexRange range{};
    };
    std::vector<StagedRange> staged;

    for (graph_jit::PortStorageIndex index = 0;
         index < plan_->storage.ports.size(); ++index) {
        auto const& planned = plan_->storage.ports[index];
        auto const& selected = slot(index);
        auto const produced = selection_.produce_storage.empty()
            ? storage_is_runtime_produced(*plan_, planned)
            : selection_.produce_storage[index];
        if (planned.storage != graph_jit::PortStorageKind::persisted_data) {
            continue;
        }
        auto const& replacement =
            selection_.persisted_replacement_coverage[index];
        if (planned.kind == PortKind::sample) {
            if (replacement.empty()) continue;
        } else if (replacement.empty()) {
            continue;
        }
        if (!selected.persisted_output) {
            return std::unexpected(
                "produced persisted storage has no output identity");
        }

        if (planned.kind == PortKind::sample) {
            auto const channels = channel_count(planned.sample_layout);
            std::vector<PersistedSampleRegion> retained_regions;
            auto const* view = produced ? sample_read(index) : nullptr;
            if (produced
                && (!view
                    || view->geometry != planned.continuous_geometry
                    || planned.sample_channels.size() != channels
                    || !std::ranges::all_of(
                        planned.sample_channels,
                        [&](std::size_t channel) {
                            return channel < channels
                                && view->has_channel(channel);
                        }))) {
                    return std::unexpected(
                        "persisted sample Regions have incompatible storage");
            }
            if (view) retained_regions.reserve(view->regions.size());
            for (auto const& descriptor : view
                    ? view->regions
                    : std::span<ContinuousRegionStorageView const>{}) {
                if ((IndexCoverage{descriptor.range}
                        & *selected.coverage).empty()) {
                    continue;
                }
                if (descriptor.channel_layout != planned.sample_layout
                    || descriptor.geometry != view->geometry
                    || descriptor.range.extent
                        > std::numeric_limits<std::size_t>::max()) {
                    return std::unexpected(
                        "persisted sample Region has incompatible metadata");
                }
                if (std::ranges::any_of(
                        staged, [&](StagedRange const& previous) {
                            return previous.output
                                    == *selected.persisted_output
                                && previous.storage != index
                                && IndexCoverage{previous.range}.intersects(
                                    descriptor.range);
                        })) {
                    return std::unexpected(
                        "persisted output range is produced by multiple storage slots");
                }
                auto const frames = static_cast<std::size_t>(
                    descriptor.range.extent);
                PersistedSampleRegion retained{
                    .output = *selected.persisted_output,
                    .range = descriptor.range,
                    .layout = planned.sample_layout,
                    .geometry = descriptor.geometry,
                };
                auto const value_count = checked_product(frames, channels);
                auto const byte_count = checked_product(
                    value_count, descriptor.geometry.value_size_bytes);
                if (descriptor.geometry.value_type
                    == continuous_value_type_id_v<Sample>) {
                    retained.values.resize(value_count);
                } else {
                    retained.continuous_values = PersistedContinuousStorage{
                        byte_count, descriptor.geometry.element_alignment};
                }
                std::memcpy(
                    retained.storage_data(), descriptor.storage, byte_count);
                retained_regions.push_back(std::move(retained));
                staged.push_back({
                    .output = *selected.persisted_output,
                    .storage = index,
                    .range = descriptor.range,
                });
            }
            auto const positional_tick_output = planned.output_port
                && *planned.output_port < plan_->ports.size()
                && plan_->ports[*planned.output_port].output_production
                    == OutputProductionKind::tick;
            if (positional_tick_output) {
                for (auto& region : retained_regions) {
                    candidate.overwrite(std::move(region));
                }
                continue;
            }
            candidate.replace_sample_regions(
                *selected.persisted_output,
                planned.sample_layout,
                planned.continuous_geometry,
                replacement,
                selection_.published_output_regions[index],
                std::move(retained_regions));
            continue;
        }

        auto const* view = produced ? event_read(index) : nullptr;
        if (produced && (!view || view->type != planned.event_type)) {
            return std::unexpected(
                "persisted event Regions have incompatible storage");
        }
        std::vector<PersistedEventChunk> retained_regions;
        if (view) retained_regions.reserve(view->regions.size());
        for (std::size_t region_index = 0;
             view && region_index < view->regions.size(); ++region_index) {
            auto const region = view->regions[region_index];
            if ((IndexCoverage{region} & *selected.coverage).empty()) continue;
            if (std::ranges::any_of(staged, [&](StagedRange const& previous) {
                    return previous.output == *selected.persisted_output
                        && previous.storage != index
                        && IndexCoverage{previous.range}.intersects(region);
                })) {
                return std::unexpected(
                    "persisted output range is produced by multiple storage slots");
            }
            PersistedEventChunk retained{
                .output = *selected.persisted_output,
                .range = region,
                .type = planned.event_type,
            };
            view->for_each_region(
                region_index, region, [&](TimedEvent const& event) {
                retained.events.push_back(event);
            });
            retained_regions.push_back(std::move(retained));
            staged.push_back({
                .output = *selected.persisted_output,
                .storage = index,
                .range = region,
            });
        }
        auto const positional_tick_output = planned.output_port
            && *planned.output_port < plan_->ports.size()
            && plan_->ports[*planned.output_port].output_production
                == OutputProductionKind::tick;
        if (positional_tick_output) {
            for (auto& region : retained_regions) {
                candidate.overwrite(std::move(region));
            }
            continue;
        }
        candidate.replace_event_regions(
            *selected.persisted_output,
            planned.event_type,
            replacement,
            selection_.published_output_regions[index],
            std::move(retained_regions));
    }
    return {};
}

BackgroundStorageOperationFrame::BackgroundStorageOperationFrame(
    BackgroundStorageRealization& realization,
    std::span<graph_jit::BackgroundRuntimeOperationIndex const> before,
    std::span<graph_jit::BackgroundRuntimeOperationIndex const> after) noexcept
    : realization_(&realization)
    , before_(before)
    , after_(after)
{}

void BackgroundStorageOperationFrame::set_leaf_hooks(
    void* data, LeafHook prepare_leaf, LeafHook finalize_leaf) noexcept
{
    leaf_data_ = data;
    prepare_leaf_ = prepare_leaf;
    finalize_leaf_ = finalize_leaf;
}

void BackgroundStorageOperationFrame::execute(
    std::span<graph_jit::BackgroundRuntimeOperationIndex const> operations)
{
    if (!realization_) {
        throw std::logic_error(
            "background storage operation frame has no realization");
    }
    for (auto const operation : operations) {
        auto executed = realization_->execute_operation(operation);
        if (!executed) {
            throw std::runtime_error(std::move(executed.error()));
        }
    }
}

void BackgroundStorageOperationFrame::prepare()
{
    execute(before_);
    if (prepare_leaf_) prepare_leaf_(leaf_data_);
}

void BackgroundStorageOperationFrame::finalize()
{
    if (finalize_leaf_) finalize_leaf_(leaf_data_);
    execute(after_);
}

void BackgroundStorageOperationFrame::prepare_callback(void* opaque)
{
    if (!opaque) {
        throw std::invalid_argument(
            "background prepare callback has no operation frame");
    }
    static_cast<BackgroundStorageOperationFrame*>(opaque)->prepare();
}

void BackgroundStorageOperationFrame::finalize_callback(void* opaque)
{
    if (!opaque) {
        throw std::invalid_argument(
            "background finalize callback has no operation frame");
    }
    static_cast<BackgroundStorageOperationFrame*>(opaque)->finalize();
}

} // namespace iv
