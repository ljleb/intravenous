#include <intravenous/runtime/background_storage_realization.h>

#include <intravenous/compat.h>
#include <intravenous/graph_jit/event_conversion_runtime.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <numeric>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace iv {
namespace {

Coverage const empty_storage_coverage{};

[[nodiscard]] std::size_t coverage_sample_count(Coverage const& coverage)
{
    std::size_t result = 0;
    for (auto const region : coverage.regions()) {
        auto const size = region.end - region.begin;
        if (size > std::numeric_limits<std::size_t>::max() - result) {
            throw std::length_error("background storage coverage is too large");
        }
        result += static_cast<std::size_t>(size);
    }
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
    Coverage const& outer,
    Coverage const& inner) noexcept
{
    return std::ranges::all_of(inner.regions(), [&](IndexRegion region) {
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
    Coverage const& left,
    Coverage const& right,
    Fn&& fn)
{
    auto left_region = left.regions().begin();
    auto right_region = right.regions().begin();
    while (left_region != left.regions().end()
        && right_region != right.regions().end()) {
        auto const intersection = IndexRegion{
            .begin = std::max(left_region->begin, right_region->begin),
            .end = std::min(left_region->end, right_region->end),
        };
        if (intersection.valid() && !intersection.empty()) fn(intersection);
        if (left_region->end < right_region->end) {
            ++left_region;
        } else {
            ++right_region;
        }
    }
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
    Coverage const* coverage_ = nullptr;
    ChannelLayout layout_{};
    std::vector<std::size_t> channels_{};
    std::vector<std::size_t> region_offsets_{};
    std::size_t frame_count_ = 0;
    std::vector<Sample> values_{};
    std::vector<std::uint8_t> written_{};

    [[nodiscard]] std::optional<std::size_t> frame_offset(
        SampleIndex index) const noexcept
    {
        auto const regions = coverage_->regions();
        for (std::size_t region = 0; region < regions.size(); ++region) {
            if (regions[region].contains(index)) {
                return region_offsets_[region]
                    + static_cast<std::size_t>(index - regions[region].begin);
            }
            if (index < regions[region].begin) break;
        }
        return std::nullopt;
    }

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
        Coverage const& coverage,
        ChannelLayout layout,
        std::span<std::size_t const> channels)
        : coverage_(&coverage)
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

        region_offsets_.reserve(coverage_->size());
        for (auto const region : coverage_->regions()) {
            region_offsets_.push_back(frame_count_);
            auto const length = region.end - region.begin;
            if (length > std::numeric_limits<std::size_t>::max() - frame_count_) {
                throw std::length_error(
                    "background sample storage coverage is too large");
            }
            frame_count_ += static_cast<std::size_t>(length);
        }
        auto const value_count = checked_product(frame_count_, channels_.size());
        values_.resize(value_count);
        written_.resize(value_count, 0);
    }

    [[nodiscard]] Sample read(
        SampleIndex index,
        std::size_t channel) const noexcept
    {
        auto const frame = frame_offset(index);
        auto const selected_channel = channel_offset(channel);
        IV_ASSERT(frame && selected_channel,
            "background sample read lies outside realized storage");
        if (!frame || !selected_channel) return {};
        return values_[*selected_channel * frame_count_ + *frame];
    }

    [[nodiscard]] bool write(
        SampleIndex index,
        std::size_t channel,
        Sample value) noexcept
    {
        auto const frame = frame_offset(index);
        auto const selected_channel = channel_offset(channel);
        if (!frame || !selected_channel) return false;
        auto const offset = *selected_channel * frame_count_ + *frame;
        values_[offset] = value;
        written_[offset] = 1;
        return true;
    }

    [[nodiscard]] bool complete() const noexcept
    {
        return std::ranges::all_of(
            written_, [](std::uint8_t written) { return written != 0; });
    }

    [[nodiscard]] BackgroundSampleReadView read_view() const noexcept
    {
        return {
            .data = this,
            .coverage_value = coverage_,
            .layout = layout_,
            .channels = channels_,
            .read_sample = +[](void const* opaque,
                               SampleIndex index,
                               std::size_t channel) noexcept {
                return static_cast<TransactionSampleStorage const*>(opaque)
                    ->read(index, channel);
            },
        };
    }

    [[nodiscard]] BackgroundSampleWriteView write_view() noexcept
    {
        return {
            .data = this,
            .coverage_value = coverage_,
            .layout = layout_,
            .channels = channels_,
            .write_sample = +[](void* opaque,
                                SampleIndex index,
                                std::size_t channel,
                                Sample value) noexcept {
                return static_cast<TransactionSampleStorage*>(opaque)
                    ->write(index, channel, value);
            },
        };
    }
};

class TransactionEventStorage {
    Coverage const* coverage_ = nullptr;
    EventTypeId type_ = EventTypeId::empty;
    std::size_t capacity_ = 0;
    std::vector<TimedEvent> events_{};

public:
    TransactionEventStorage(
        Coverage const& coverage,
        EventTypeId type,
        double max_events_per_index)
        : coverage_(&coverage)
        , type_(type)
    {
        if (type >= EventTypeId::count) {
            throw std::invalid_argument(
                "background event storage has an invalid type");
        }
        auto const capacity = event_count_for_sample_span(
            max_events_per_index, coverage_sample_count(coverage));
        if (!capacity) {
            throw std::length_error(
                "background event storage capacity is not representable");
        }
        capacity_ = *capacity;
        events_.reserve(capacity_);
    }

    [[nodiscard]] bool write(TimedEvent const& event) noexcept
    {
        auto const index = static_cast<SampleIndex>(event.time);
        if (!coverage_->contains(index)
            || !event_matches_type(type_, event.value)
            || events_.size() == capacity_
            || (!events_.empty() && event.time < events_.back().time)) {
            return false;
        }
        events_.push_back(event);
        return true;
    }

    void for_each(
        IndexRegion region,
        void* visitor_data,
        BackgroundEventReadView::VisitEvent visitor) const
    {
        auto first = std::ranges::lower_bound(events_, region.begin, {},
            [](TimedEvent const& event) {
                return static_cast<SampleIndex>(event.time);
            });
        for (; first != events_.end()
             && static_cast<SampleIndex>(first->time) < region.end; ++first) {
            visitor(visitor_data, *first);
        }
    }

    [[nodiscard]] BackgroundEventReadView read_view() const noexcept
    {
        return {
            .data = this,
            .coverage_value = coverage_,
            .type = type_,
            .for_each_event = +[](void const* opaque,
                                  IndexRegion region,
                                  void* visitor_data,
                                  BackgroundEventReadView::VisitEvent visitor) {
                static_cast<TransactionEventStorage const*>(opaque)->for_each(
                    region, visitor_data, visitor);
            },
        };
    }

    [[nodiscard]] BackgroundEventWriteView write_view() noexcept
    {
        return {
            .data = this,
            .coverage_value = coverage_,
            .type = type_,
            .write_event = +[](void* opaque, TimedEvent const& event) noexcept {
                return static_cast<TransactionEventStorage*>(opaque)->write(event);
            },
        };
    }
};

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

class PersistedSampleReader {
    PersistedPageStore::Snapshot const* snapshot_ = nullptr;
    PersistedOutputId output_{};
    Coverage const* coverage_ = nullptr;
    ChannelLayout layout_{};
    std::span<std::size_t const> channels_{};

public:
    PersistedSampleReader(
        PersistedPageStore::Snapshot const& snapshot,
        PersistedOutputId output,
        Coverage const& coverage,
        ChannelLayout layout,
        std::span<std::size_t const> channels) noexcept
        : snapshot_(&snapshot)
        , output_(std::move(output))
        , coverage_(&coverage)
        , layout_(layout)
        , channels_(channels)
    {}

    [[nodiscard]] bool complete() const noexcept
    {
        auto const width = snapshot_->page_width();
        if (coverage_->empty()) return true;
        if (width == 0) return false;
        for (auto const region : coverage_->regions()) {
            auto page_index = region.begin / width;
            auto const last_page = (region.end - 1) / width;
            for (;; ++page_index) {
                auto const page_begin = page_index * width;
                auto const page_end = saturating_sample_index_add(
                    page_begin, static_cast<SampleIndex>(width));
                auto const required = IndexRegion{
                    std::max(region.begin, page_begin),
                    std::min(region.end, page_end),
                };
                auto const* page =
                    snapshot_->find_sample_page(output_, page_index);
                if (!page || page->layout != layout_ ||
                    !page->domain.contains(required) ||
                    std::ranges::any_of(channels_, [&](std::size_t channel) {
                        return channel >= channel_count(page->layout);
                    })) {
                    return false;
                }
                if (page_index == last_page) break;
            }
        }
        return true;
    }

    [[nodiscard]] Sample read(
        SampleIndex index,
        std::size_t channel) const noexcept
    {
        auto const width = snapshot_->page_width();
        IV_ASSERT(width != 0, "persisted sample view has no page grid");
        if (width == 0) return {};
        auto const page_index = index / width;
        auto const* page = snapshot_->find_sample_page(output_, page_index);
        IV_ASSERT(page && page->domain.contains(index),
            "persisted sample read has no published page");
        if (!page || !page->domain.contains(index)
            || page->layout.channel_type != layout_.channel_type
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
        IV_ASSERT(offset < page->values.size(),
            "persisted sample page payload is incomplete");
        return offset < page->values.size() ? page->values[offset] : Sample{};
    }

    [[nodiscard]] BackgroundSampleReadView view() const noexcept
    {
        return {
            .data = this,
            .coverage_value = coverage_,
            .layout = layout_,
            .channels = channels_,
            .read_sample = +[](void const* opaque,
                               SampleIndex index,
                               std::size_t channel) noexcept {
                return static_cast<PersistedSampleReader const*>(opaque)
                    ->read(index, channel);
            },
        };
    }
};

class PersistedEventReader {
    PersistedPageStore::Snapshot const* snapshot_ = nullptr;
    PersistedOutputId output_{};
    Coverage const* coverage_ = nullptr;
    EventTypeId type_ = EventTypeId::empty;

public:
    PersistedEventReader(
        PersistedPageStore::Snapshot const& snapshot,
        PersistedOutputId output,
        Coverage const& coverage,
        EventTypeId type) noexcept
        : snapshot_(&snapshot)
        , output_(std::move(output))
        , coverage_(&coverage)
        , type_(type)
    {}

    [[nodiscard]] bool complete() const noexcept
    {
        auto const width = snapshot_->page_width();
        if (coverage_->empty()) return true;
        if (width == 0) return false;
        for (auto const region : coverage_->regions()) {
            auto page_index = region.begin / width;
            auto const last_page = (region.end - 1) / width;
            for (;; ++page_index) {
                auto const page_begin = page_index * width;
                auto const page_end = saturating_sample_index_add(
                    page_begin, static_cast<SampleIndex>(width));
                auto const required = IndexRegion{
                    std::max(region.begin, page_begin),
                    std::min(region.end, page_end),
                };
                auto const* page = snapshot_->find_event_page(output_, page_index);
                if (!page || page->type != type_ ||
                    !page->domain.contains(required)) {
                    return false;
                }
                if (page_index == last_page) break;
            }
        }
        return true;
    }

    void for_each(
        IndexRegion region,
        void* visitor_data,
        BackgroundEventReadView::VisitEvent visitor) const
    {
        auto const width = snapshot_->page_width();
        IV_ASSERT(width != 0, "persisted event view has no page grid");
        if (width == 0 || region.empty()) return;
        auto const first_page = region.begin / width;
        auto const last_page = (region.end - 1) / width;
        for (auto page_index = first_page;; ++page_index) {
            auto const* page = snapshot_->find_event_page(output_, page_index);
            IV_ASSERT(page != nullptr,
                "persisted event read has no published page");
            if (page) {
                IV_ASSERT(page->type == type_,
                    "persisted event page type does not match its runtime view");
                if (page->type == type_) {
                    auto const page_begin = page_index * width;
                    for (auto const& stored : page->events) {
                        auto const absolute = page_begin
                            + static_cast<SampleIndex>(stored.time);
                        if (absolute < region.begin || absolute >= region.end) {
                            continue;
                        }
                        IV_ASSERT(
                            absolute <= std::numeric_limits<EventTime>::max(),
                            "persisted event time exceeds EventTime");
                        if (absolute > std::numeric_limits<EventTime>::max()) {
                            continue;
                        }
                        TimedEvent event = stored;
                        event.time = static_cast<EventTime>(absolute);
                        visitor(visitor_data, event);
                    }
                }
            }
            if (page_index == last_page) break;
        }
    }

    [[nodiscard]] BackgroundEventReadView view() const noexcept
    {
        return {
            .data = this,
            .coverage_value = coverage_,
            .type = type_,
            .for_each_event = +[](void const* opaque,
                                  IndexRegion region,
                                  void* visitor_data,
                                  BackgroundEventReadView::VisitEvent visitor) {
                static_cast<PersistedEventReader const*>(opaque)->for_each(
                    region, visitor_data, visitor);
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

Coverage const& BackgroundSampleReadView::coverage() const noexcept
{
    return coverage_value ? *coverage_value : empty_storage_coverage;
}

bool BackgroundSampleReadView::has_channel(
    std::size_t channel) const noexcept
{
    return std::ranges::contains(channels, channel);
}

Sample BackgroundSampleReadView::at(
    SampleIndex index,
    std::size_t channel) const noexcept
{
    IV_ASSERT(*this, "background sample storage has no readable view");
    IV_ASSERT(coverage().contains(index) && has_channel(channel),
        "background sample read lies outside selected storage");
    return *this ? read_sample(data, index, channel) : Sample{};
}

Coverage const& BackgroundSampleWriteView::coverage() const noexcept
{
    return coverage_value ? *coverage_value : empty_storage_coverage;
}

bool BackgroundSampleWriteView::has_channel(
    std::size_t channel) const noexcept
{
    return std::ranges::contains(channels, channel);
}

bool BackgroundSampleWriteView::write(
    SampleIndex index,
    std::size_t channel,
    Sample value) const noexcept
{
    return *this && coverage().contains(index) && has_channel(channel)
        && write_sample(data, index, channel, value);
}

Coverage const& BackgroundEventReadView::coverage() const noexcept
{
    return coverage_value ? *coverage_value : empty_storage_coverage;
}

Coverage const& BackgroundEventWriteView::coverage() const noexcept
{
    return coverage_value ? *coverage_value : empty_storage_coverage;
}

bool BackgroundEventWriteView::write(TimedEvent const& event) const noexcept
{
    return *this && coverage().contains(static_cast<SampleIndex>(event.time))
        && write_event(data, event);
}

struct BackgroundStorageRealization::Slot {
    graph_jit::PortStoragePlan const* plan = nullptr;
    Coverage const* coverage = nullptr;
    std::optional<PersistedOutputId> persisted_identity{};
    std::unique_ptr<TransactionSampleStorage> owned_sample{};
    std::unique_ptr<TransactionEventStorage> owned_event{};
    std::unique_ptr<PersistedSampleReader> persisted_sample{};
    std::unique_ptr<PersistedEventReader> persisted_event{};
    std::optional<BackgroundSampleReadView> sample_read{};
    std::optional<BackgroundSampleWriteView> sample_write{};
    std::optional<BackgroundEventReadView> event_read{};
    std::optional<BackgroundEventWriteView> event_write{};
};

BackgroundStorageRealization::BackgroundStorageRealization(
    graph_jit::BackgroundEvaluationPlan const& plan,
    BackgroundStorageSelection selection)
    : plan_(&plan)
    , selection_(std::move(selection))
    , executed_operations_(plan.runtime.operations.size(), false)
{
    if (selection_.storage_coverage.size() != plan.storage.ports.size()) {
        throw std::invalid_argument(
            "background storage coverage is not aligned with the compiled plan");
    }
    if (!selection_.produce_storage.empty()
        && selection_.produce_storage.size() != plan.storage.ports.size()) {
        throw std::invalid_argument(
            "background storage production is not aligned with the compiled plan");
    }
    slots_.reserve(plan.storage.ports.size());
    for (graph_jit::PortStorageIndex index = 0;
         index < plan.storage.ports.size(); ++index) {
        auto const& planned = plan.storage.ports[index];
        auto created = std::make_unique<Slot>();
        created->plan = &planned;
        created->coverage = &selection_.storage_coverage[index];

        if (planned.storage == graph_jit::PortStorageKind::persisted_pages) {
            created->persisted_identity = persisted_output_id(
                plan, index, selection_.generation);
        }

        auto const produced = selection_.produce_storage.empty()
            ? storage_is_runtime_produced(plan, planned)
            : selection_.produce_storage[index];
        if (produced && planned.storage != graph_jit::PortStorageKind::current_tick) {
            if (planned.kind == PortKind::sample) {
                created->owned_sample = std::make_unique<TransactionSampleStorage>(
                    *created->coverage,
                    planned.sample_layout,
                    planned.sample_channels);
                created->sample_read = created->owned_sample->read_view();
                created->sample_write = created->owned_sample->write_view();
            } else {
                created->owned_event = std::make_unique<TransactionEventStorage>(
                    *created->coverage,
                    planned.event_type,
                    planned.max_events_per_index);
                created->event_read = created->owned_event->read_view();
                created->event_write = created->owned_event->write_view();
            }
        } else if (planned.storage
                == graph_jit::PortStorageKind::persisted_pages
            && selection_.published
            && selection_.published->page_width() != 0) {
            if (planned.kind == PortKind::sample) {
                created->persisted_sample =
                    std::make_unique<PersistedSampleReader>(
                        *selection_.published,
                        *created->persisted_identity,
                        *created->coverage,
                        planned.sample_layout,
                        planned.sample_channels);
                created->sample_read = created->persisted_sample->view();
            } else {
                created->persisted_event =
                    std::make_unique<PersistedEventReader>(
                        *selection_.published,
                        *created->persisted_identity,
                        *created->coverage,
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

PersistedOutputId const* BackgroundStorageRealization::persisted_output(
    graph_jit::PortStorageIndex index) const
{
    auto const& selected = slot(index);
    return selected.persisted_identity
        ? &*selected.persisted_identity
        : nullptr;
}

std::expected<void, std::string> BackgroundStorageRealization::seal()
{
    auto const valid_sample_read = [&](Slot const& selected) {
        auto const& planned = *selected.plan;
        return selected.sample_read
            && selected.sample_read->layout.channel_type
                == planned.sample_layout.channel_type
            && contains_coverage(
                selected.sample_read->coverage(), *selected.coverage)
            && contains_channels(
                selected.sample_read->channels, planned.sample_channels);
    };
    auto const valid_sample_write = [&](Slot const& selected) {
        auto const& planned = *selected.plan;
        return selected.sample_write
            && selected.sample_write->layout.channel_type
                == planned.sample_layout.channel_type
            && contains_coverage(
                selected.sample_write->coverage(), *selected.coverage)
            && contains_channels(
                selected.sample_write->channels, planned.sample_channels);
    };
    auto const valid_event_read = [&](Slot const& selected) {
        return selected.event_read
            && selected.event_read->type == selected.plan->event_type
            && contains_coverage(
                selected.event_read->coverage(), *selected.coverage);
    };
    auto const valid_event_write = [&](Slot const& selected) {
        return selected.event_write
            && selected.event_write->type == selected.plan->event_type
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
                != materialization.target_layout.channel_type) {
            result = std::unexpected(
                "background sample materialization has no compatible target");
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
            ChannelConversionPlan conversion{};
            std::vector<std::size_t> source_indices{};
            std::vector<std::size_t> target_channels{};
        };
        std::vector<RuntimeProjection> projections;
        auto append_projection = [&] (
            ChannelTypeId source_type,
            std::vector<std::size_t> source_indices,
            ChannelTypeId target_type,
            std::vector<std::size_t> target_channels)
            -> std::expected<void, std::string> {
            if (source_indices.size() != channel_count(source_type)
                || target_channels.size() != channel_count(target_type)) {
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
            try {
                projections.push_back(RuntimeProjection{
                    .conversion = ChannelConversionRegistry::plan(
                        ChannelLayout{
                            .channel_type = source_type,
                            .sample_layout = SampleStreamLayout::interleaved,
                        },
                        ChannelLayout{
                            .channel_type = target_type,
                            .sample_layout = SampleStreamLayout::interleaved,
                        }),
                    .source_indices = std::move(source_indices),
                    .target_channels = std::move(target_channels),
                });
            } catch (std::exception const& error) {
                return std::unexpected(
                    "background sample projection conversion is unsupported: "
                    + std::string(error.what()));
            }
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
                    projection.target_channels);
                if (!result) break;
            }
        }
        if (!result) break;

        std::array<Sample, 2> source_values{};
        std::array<Sample, 2> target_values{};
        for (auto const region : target_coverage.regions()) {
            for (auto sample_index = region.begin;
                 sample_index < region.end; ++sample_index) {
                for (auto const& projection : projections) {
                    for (std::size_t channel = 0;
                         channel < projection.source_indices.size(); ++channel) {
                        auto const source_index =
                            projection.source_indices[channel];
                        auto const latency = materialization
                            .source_read_latencies[source_index];
                        if (latency > sample_index) {
                            result = std::unexpected(
                                "background sample latency precedes the timeline");
                            break;
                        }
                        auto const source_sample_index = sample_index - latency;
                        auto const* source = sample_read(
                            materialization.inputs[source_index]);
                        if (!slot(materialization.inputs[source_index])
                                .coverage->contains(source_sample_index)) {
                            result = std::unexpected(
                                "background sample source does not cover its delayed read");
                            break;
                        }
                        source_values[channel] = source->at(
                            source_sample_index,
                            materialization.source_channels[source_index]
                                .channel);
                    }
                    if (!result) break;
                    projection.conversion.convert(
                        source_values.data(), target_values.data(), 1);
                    for (std::size_t channel = 0;
                         channel < projection.target_channels.size(); ++channel) {
                        if (!target->write(
                                sample_index,
                                projection.target_channels[channel],
                                target_values[channel])) {
                            result = std::unexpected(
                                "background sample materialization write failed");
                            break;
                        }
                    }
                    if (!result) break;
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
        if (materialization.conversion.source_type
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

        std::vector<std::vector<TimedEvent>> converted(
            materialization.inputs.size());
        std::size_t total_count = 0;
        for (std::size_t source_index = 0;
             source_index < materialization.inputs.size(); ++source_index) {
            auto const* source = event_read(
                materialization.inputs[source_index]);
            if (!source || source->type != materialization.source_type) {
                result = std::unexpected(
                    "background event materialization has no compatible source");
                break;
            }
            std::vector<TimedEvent> source_events;
            bool ordered = true;
            for_each_intersection(
                *slot(materialization.inputs[source_index]).coverage,
                target_coverage,
                [&](IndexRegion region) {
                    source->for_each(region, [&](TimedEvent const& event) {
                        if (!source_events.empty()
                            && event.time < source_events.back().time) {
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
                auto const& conversion = materialization.conversion;
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
        std::ranges::copy(converted.front(), merged.begin());
        std::vector<void const*> source_events;
        std::vector<std::size_t> source_remaining;
        source_events.reserve(converted.size() - 1);
        source_remaining.reserve(converted.size() - 1);
        for (std::size_t source = 1; source < converted.size(); ++source) {
            source_events.push_back(converted[source].data());
            source_remaining.push_back(converted[source].size());
        }
        auto const merged_count = graph_jit::detail::
            iv_graph_jit_merge_event_sequences_into_home(
                merged.data(),
                merged.size(),
                converted.front().size(),
                source_events.data(),
                source_remaining.data(),
                source_events.size());
        if (merged_count != total_count) {
            result = std::unexpected(
                "background event materialization merge failed");
            break;
        }
        for (std::size_t event = 0; event < merged_count; ++event) {
            if (!target->write(merged[event])) {
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
                "background sample output did not initialize every selected "
                "sample and channel");
        }
    }
    return {};
}

std::expected<std::unique_ptr<TickMaterializationSnapshot>, std::string>
BackgroundStorageRealization::make_tick_materialization_snapshot(
    std::uint64_t generation,
    std::uint64_t semantic_version,
    PersistedPageSnapshotVersion pages) const
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
    auto shifted_forward = [](Coverage const& source, std::size_t latency) {
        Coverage result;
        for (auto const region : source.regions()) {
            auto const begin = saturating_sample_index_add(region.begin, latency);
            auto const end = saturating_sample_index_add(region.end, latency);
            if (begin < end) result.include({begin, end});
        }
        return result;
    };
    auto tick_storage = [&](graph_jit::PortStorageIndex index) {
        return index < plan_->storage.ports.size()
            && plan_->storage.ports[index].storage
                == graph_jit::PortStorageKind::tick_random_access;
    };

    std::vector<TickMaterializedSampleInput> samples;
    for (auto const& binding :
         plan_->tick_runtime.random_access_sample_inputs) {
        if (binding.port >= plan_->ports.size()) {
            return std::unexpected(
                "Tick sample materialization references a missing input port");
        }
        auto const& port = plan_->ports[binding.port];
        std::vector<SampleRoute> routes;
        auto append_route = [&](SampleRoute route) {
            if (std::ranges::none_of(routes, [&](SampleRoute const& existing) {
                    return existing.view->data == route.view->data
                        && existing.logical_channel == route.logical_channel
                        && existing.storage_channel == route.storage_channel
                        && existing.latency == route.latency;
                })) {
                routes.push_back(route);
            }
        };
        for (auto const& direct : plan_->storage.direct_samples) {
            if (!tick_storage(direct.storage)
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
            if (!tick_storage(storage_index)) continue;
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
        Coverage complete_coverage;
        auto const channels = channel_count(port.sample_layout);
        for (std::size_t channel = 0; channel < channels; ++channel) {
            Coverage available;
            for (auto const& route : routes) {
                if (route.logical_channel == channel) {
                    available.include(shifted_forward(
                        route.view->coverage(), route.latency));
                }
            }
            complete_coverage = channel == 0
                ? std::move(available)
                : complete_coverage & available;
        }
        if (complete_coverage.empty()) continue;

        TickMaterializedSampleInput frozen{
            .port = binding.port,
            .coverage = std::move(complete_coverage),
            .layout = port.sample_layout,
        };
        frozen.channels.resize(channels);
        std::iota(frozen.channels.begin(), frozen.channels.end(), 0);
        frozen.values.reserve(checked_product(
            coverage_sample_count(frozen.coverage), channels));
        bool missing_route = false;
        for (auto const channel : frozen.channels) {
            for (auto const region : frozen.coverage.regions()) {
                for (auto sample = region.begin; sample < region.end; ++sample) {
                    auto const found = std::ranges::find_if(
                        routes, [&](SampleRoute const& route) {
                            return route.logical_channel == channel
                                && sample >= route.latency
                                && route.view->coverage().contains(
                                    sample - route.latency);
                        });
                    if (found == routes.end()) {
                        missing_route = true;
                        break;
                    }
                    frozen.values.push_back(found->view->at(
                        sample - found->latency, found->storage_channel));
                }
                if (missing_route) break;
            }
            if (missing_route) break;
        }
        if (missing_route) {
            return std::unexpected(
                "Tick sample materialization has incomplete routed coverage");
        }
        samples.push_back(std::move(frozen));
    }

    std::vector<TickMaterializedEventInput> events;
    for (auto const& binding :
         plan_->tick_runtime.random_access_event_inputs) {
        if (binding.port >= plan_->ports.size()) {
            return std::unexpected(
                "Tick event materialization references a missing input port");
        }
        auto const& port = plan_->ports[binding.port];
        std::vector<BackgroundEventReadView const*> routes;
        auto append_route = [&](BackgroundEventReadView const* view) {
            if (view && view->type == port.event_type
                && std::ranges::none_of(routes, [&](auto const* existing) {
                    return existing->data == view->data;
                })) {
                routes.push_back(view);
            }
        };
        for (auto const& direct : plan_->storage.direct_events) {
            if (!tick_storage(direct.storage)
                || !std::ranges::contains(binding.storage, direct.storage)
                || direct.target_subset >= plan_->event_target_subsets.size()) {
                continue;
            }
            auto const& target =
                plan_->event_target_subsets[direct.target_subset].port;
            if (target.bundle == port.configured_port.node_bundle_handle
                && target.port == port.configured_port.port_index) {
                append_route(event_read(direct.storage));
            }
        }
        for (auto const storage_index : binding.storage) {
            if (!tick_storage(storage_index)) continue;
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
        Coverage available;
        for (auto const* route : routes) available.include(route->coverage());
        if (available.empty()) continue;
        TickMaterializedEventInput frozen{
            .port = binding.port,
            .coverage = std::move(available),
            .type = port.event_type,
        };
        for (auto const region : frozen.coverage.regions()) {
            auto cursor = region.begin;
            while (cursor < region.end) {
                BackgroundEventReadView const* selected = nullptr;
                auto selected_end = cursor;
                for (auto const* route : routes) {
                    for (auto const covered : route->coverage().regions()) {
                        if (!covered.contains(cursor)) continue;
                        selected = route;
                        selected_end = std::min(region.end, covered.end);
                        break;
                    }
                    if (selected) break;
                }
                if (!selected || selected_end <= cursor) {
                    return std::unexpected(
                        "Tick event materialization has incomplete routed coverage");
                }
                selected->for_each({cursor, selected_end},
                    [&](TimedEvent const& event) {
                        frozen.events.push_back(event);
                    });
                cursor = selected_end;
            }
        }
        events.push_back(std::move(frozen));
    }

    return std::make_unique<TickMaterializationSnapshot>(
        generation,
        semantic_version,
        pages,
        std::move(samples),
        std::move(events));
}

std::expected<void, std::string>
BackgroundStorageRealization::stage_persisted_pages(
    PersistedPageStore::Candidate& candidate) const
{
    if (auto complete = validate_produced_storage(); !complete)
        return complete;
    auto const width = candidate.page_width();
    if (width == 0) {
        return std::unexpected("persisted candidate has no page grid");
    }

    struct StagedPage {
        PersistedOutputId output{};
        std::uint64_t page = 0;
        graph_jit::PortStorageIndex storage = 0;
    };
    std::vector<StagedPage> staged;

    for (graph_jit::PortStorageIndex index = 0;
         index < plan_->storage.ports.size(); ++index) {
        auto const& planned = plan_->storage.ports[index];
        auto const& selected = slot(index);
        auto const produced = selection_.produce_storage.empty()
            ? storage_is_runtime_produced(*plan_, planned)
            : selection_.produce_storage[index];
        if (planned.storage != graph_jit::PortStorageKind::persisted_pages
            || selected.coverage->empty() || !produced) {
            continue;
        }
        if (!selected.persisted_identity) {
            return std::unexpected(
                "produced persisted storage has no output identity");
        }

        for (auto const region : selected.coverage->regions()) {
            auto page_index = static_cast<std::uint64_t>(region.begin / width);
            auto const last_page =
                static_cast<std::uint64_t>((region.end - 1) / width);
            for (;; ++page_index) {
                auto const existing = std::ranges::find_if(
                    staged, [&](StagedPage const& page) {
                        return page.page == page_index
                            && page.output == *selected.persisted_identity;
                    });
                if (existing != staged.end()) {
                    if (existing->storage != index) {
                        return std::unexpected(
                            "persisted output page is produced by multiple storage slots");
                    }
                    if (page_index == last_page) break;
                    continue;
                }
                auto const page_begin =
                    static_cast<SampleIndex>(page_index * width);
                auto const page_end = saturating_sample_index_add(
                    page_begin, static_cast<SampleIndex>(width));
                auto const domain = *selected.coverage
                    & Coverage{IndexRegion{page_begin, page_end}};
                if (!domain.empty()) {
                    if (planned.kind == PortKind::sample) {
                        auto const* view = sample_read(index);
                        auto const channels = channel_count(planned.sample_layout);
                        if (!view || planned.sample_channels.size() != channels
                            || !std::ranges::all_of(
                                planned.sample_channels,
                                [&](std::size_t channel) {
                                    return channel < channels
                                        && view->has_channel(channel);
                                })) {
                            return std::unexpected(
                                "persisted sample page has incomplete channels");
                        }
                        auto const frames = coverage_sample_count(domain);
                        PersistedSamplePage page{
                            .output = *selected.persisted_identity,
                            .page_index = page_index,
                            .domain = domain,
                            .layout = planned.sample_layout,
                            .packing = PersistedSamplePacking::coverage_packed,
                        };
                        page.values.resize(checked_product(frames, channels));
                        std::size_t packed_frame = 0;
                        for (auto const part : domain.regions()) {
                            for (auto sample = part.begin; sample < part.end;
                                 ++sample, ++packed_frame) {
                                for (std::size_t channel = 0;
                                     channel < channels; ++channel) {
                                    auto const offset =
                                        planned.sample_layout.sample_layout
                                                == SampleStreamLayout::planar
                                            ? channel * frames + packed_frame
                                            : packed_frame * channels + channel;
                                    page.values[offset] = view->at(sample, channel);
                                }
                            }
                        }
                        candidate.put(std::move(page));
                    } else {
                        auto const* view = event_read(index);
                        if (!view) {
                            return std::unexpected(
                                "persisted event page has no readable storage");
                        }
                        PersistedEventPage page{
                            .output = *selected.persisted_identity,
                            .page_index = page_index,
                            .domain = domain,
                            .type = planned.event_type,
                        };
                        for (auto const part : domain.regions()) {
                            view->for_each(part, [&](TimedEvent const& event) {
                                auto stored = event;
                                stored.time = static_cast<EventTime>(
                                    static_cast<SampleIndex>(event.time)
                                    - page_begin);
                                page.events.push_back(std::move(stored));
                            });
                        }
                        candidate.put(std::move(page));
                    }
                    staged.push_back(
                        {*selected.persisted_identity, page_index, index});
                }
                if (page_index == last_page) break;
            }
        }
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
