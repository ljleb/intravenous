#include <intravenous/runtime/background_storage_realization.h>

#include <intravenous/compat.h>

#include <algorithm>
#include <cmath>
#include <limits>
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

class TransactionSampleStorage {
    Coverage const* coverage_ = nullptr;
    ChannelLayout layout_{};
    std::vector<std::size_t> channels_{};
    std::vector<std::size_t> region_offsets_{};
    std::size_t frame_count_ = 0;
    std::vector<Sample> values_{};

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
        values_.resize(checked_product(frame_count_, channels_.size()));
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
        values_[*selected_channel * frame_count_ + *frame] = value;
        return true;
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

[[nodiscard]] PersistedOutputId persisted_output_for(
    graph_jit::BackgroundEvaluationPlan const& plan,
    graph_jit::PortStoragePlan const& storage,
    std::uint64_t generation)
{
    if (!storage.output_port || *storage.output_port >= plan.ports.size()) {
        throw std::invalid_argument(
            "persisted storage has no generation output identity");
    }
    auto const output_port = *storage.output_port;
    auto const& output = plan.ports[output_port];
    if (output.stable_identity) return *output.stable_identity;
    return GenerationLocalPersistedOutputId{
        .generation = generation,
        .port = output_port,
        .kind = storage.kind,
    };
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
{
    if (selection_.storage_coverage.size() != plan.storage.ports.size()) {
        throw std::invalid_argument(
            "background storage coverage is not aligned with the compiled plan");
    }
    slots_.reserve(plan.storage.ports.size());
    for (graph_jit::PortStorageIndex index = 0;
         index < plan.storage.ports.size(); ++index) {
        auto const& planned = plan.storage.ports[index];
        auto created = std::make_unique<Slot>();
        created->plan = &planned;
        created->coverage = &selection_.storage_coverage[index];

        if (planned.storage == graph_jit::PortStorageKind::persisted_pages) {
            created->persisted_identity = persisted_output_for(
                plan, planned, selection_.generation);
        }

        auto const produced = storage_is_runtime_produced(plan, planned);
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
            auto valid = binding.direction == graph_jit::PortDirection::input
                ? require_read(storage)
                : require_write(storage);
            if (!valid) return valid;
        }
    }
    for (auto const& materialization : plan_->storage.sample_materializations) {
        for (auto const input : materialization.inputs) {
            if (auto valid = require_read(input); !valid) return valid;
        }
        if (auto valid = require_write(materialization.output); !valid) {
            return valid;
        }
    }
    for (auto const& materialization : plan_->storage.event_materializations) {
        for (auto const input : materialization.inputs) {
            if (auto valid = require_read(input); !valid) return valid;
        }
        if (auto valid = require_write(materialization.output); !valid) {
            return valid;
        }
    }
    for (auto const& direct : plan_->storage.direct_samples) {
        if (auto valid = require_read(direct.storage); !valid) return valid;
    }
    for (auto const& direct : plan_->storage.direct_events) {
        if (auto valid = require_read(direct.storage); !valid) return valid;
    }

    sealed_ = true;
    return {};
}

} // namespace iv
