#include <intravenous/runtime/tick_materialization_snapshot.h>

#include <algorithm>
#include <cassert>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace iv {
namespace {

[[nodiscard]] std::size_t coverage_sample_count(Coverage const& coverage)
{
    std::size_t result = 0;
    for (auto const region : coverage.regions()) {
        auto const length = region.end - region.begin;
        if (length > std::numeric_limits<std::size_t>::max() - result) {
            throw std::length_error(
                "Tick materialization coverage is too large");
        }
        result += static_cast<std::size_t>(length);
    }
    return result;
}

[[nodiscard]] std::size_t checked_product(
    std::size_t left, std::size_t right)
{
    if (right != 0 && left > std::numeric_limits<std::size_t>::max() / right) {
        throw std::length_error(
            "Tick sample materialization is too large");
    }
    return left * right;
}

[[nodiscard]] bool event_matches_type(
    EventTypeId type, Event const& event) noexcept
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

} // namespace

bool TickMaterializedSampleInput::has_channel(
    std::size_t channel) const noexcept
{
    return std::ranges::contains(channels, channel);
}

Sample TickMaterializedSampleInput::at(
    SampleIndex index, std::size_t channel) const noexcept
{
    auto const found = std::ranges::lower_bound(channels, channel);
    if (found == channels.end() || *found != channel) return {};
    std::size_t frame = 0;
    bool covered = false;
    for (auto const region : coverage.regions()) {
        if (region.contains(index)) {
            frame += static_cast<std::size_t>(index - region.begin);
            covered = true;
            break;
        }
        if (index < region.begin) break;
        frame += static_cast<std::size_t>(region.end - region.begin);
    }
    if (!covered) return {};
    std::size_t frames = 0;
    for (auto const region : coverage.regions()) {
        frames += static_cast<std::size_t>(region.end - region.begin);
    }
    auto const selected = static_cast<std::size_t>(
        std::distance(channels.begin(), found));
    auto const offset = selected * frames + frame;
    return offset < values.size() ? values[offset] : Sample{};
}

void TickMaterializedEventInput::for_each(
    SampleIndex begin,
    SampleIndex end,
    void* visitor_data,
    RandomAccessEventInputPort::VisitEvent visitor) const
{
    if (begin >= end || visitor == nullptr) return;
    auto found = std::ranges::lower_bound(events, begin, {},
        [](TimedEvent const& event) {
            return static_cast<SampleIndex>(event.time);
        });
    for (; found != events.end()
         && static_cast<SampleIndex>(found->time) < end; ++found) {
        visitor(visitor_data, *found);
    }
}

TickMaterializationSnapshot::TickMaterializationSnapshot(
    std::uint64_t generation,
    std::uint64_t semantic_version,
    PersistedPageSnapshotVersion pages,
    std::vector<TickMaterializedSampleInput> samples,
    std::vector<TickMaterializedEventInput> events)
    : generation_(generation)
    , semantic_version_(semantic_version)
    , pages_(pages)
    , samples_(std::move(samples))
    , events_(std::move(events))
{
    std::vector<graph_jit::BackgroundPortIndex> ports;
    ports.reserve(samples_.size() + events_.size());
    for (auto const& sample : samples_) {
        if (sample.coverage.empty()
            || !is_valid_channel_type(sample.layout.channel_type)
            || !is_valid_sample_stream_layout(sample.layout.sample_layout)
            || sample.channels.empty()
            || !std::ranges::is_sorted(sample.channels)
            || std::ranges::adjacent_find(sample.channels)
                != sample.channels.end()
            || std::ranges::any_of(
                sample.channels,
                [&](std::size_t channel) {
                    return channel >= channel_count(sample.layout);
                })
            || sample.values.size()
                != checked_product(
                    coverage_sample_count(sample.coverage),
                    sample.channels.size())) {
            throw std::invalid_argument(
                "Tick sample materialization is inconsistent");
        }
        ports.push_back(sample.port);
    }
    for (auto const& event : events_) {
        bool valid = !event.coverage.empty() && event.type < EventTypeId::count;
        EventTime previous = 0;
        bool first = true;
        for (auto const& value : event.events) {
            auto const index = static_cast<SampleIndex>(value.time);
            valid = valid && event.coverage.contains(index)
                && event_matches_type(event.type, value.value)
                && (first || value.time >= previous);
            previous = value.time;
            first = false;
        }
        if (!valid) {
            throw std::invalid_argument(
                "Tick event materialization is inconsistent");
        }
        ports.push_back(event.port);
    }
    std::ranges::sort(ports);
    if (std::ranges::adjacent_find(ports) != ports.end()) {
        throw std::invalid_argument(
            "Tick materialization input port is duplicated");
    }
}

TickMaterializedSampleInput const* TickMaterializationSnapshot::find_sample(
    graph_jit::BackgroundPortIndex port) const noexcept
{
    auto const found = std::ranges::find(
        samples_, port, &TickMaterializedSampleInput::port);
    return found == samples_.end() ? nullptr : &*found;
}

TickMaterializedEventInput const* TickMaterializationSnapshot::find_event(
    graph_jit::BackgroundPortIndex port) const noexcept
{
    auto const found = std::ranges::find(
        events_, port, &TickMaterializedEventInput::port);
    return found == events_.end() ? nullptr : &*found;
}

TickMaterializationStore::ReaderPin::ReaderPin(
    ReaderSlotState& slot,
    std::atomic<TickMaterializationSnapshot const*> const& published) noexcept
    : slot_(&slot)
{
    assert(slot.pinned.load(std::memory_order_seq_cst) == nullptr);
    slot.acquiring.store(true, std::memory_order_seq_cst);
    snapshot_ = published.load(std::memory_order_seq_cst);
    slot.pinned.store(snapshot_, std::memory_order_seq_cst);
    slot.acquiring.store(false, std::memory_order_seq_cst);
}

void TickMaterializationStore::ReaderPin::reset() noexcept
{
    if (!slot_) return;
    slot_->pinned.store(nullptr, std::memory_order_seq_cst);
    slot_ = nullptr;
    snapshot_ = nullptr;
}

TickMaterializationStore::ReaderPin::~ReaderPin()
{
    reset();
}

TickMaterializationStore::ReaderPin::ReaderPin(ReaderPin&& other) noexcept
    : slot_(std::exchange(other.slot_, nullptr))
    , snapshot_(std::exchange(other.snapshot_, nullptr))
{}

TickMaterializationStore::ReaderPin&
TickMaterializationStore::ReaderPin::operator=(ReaderPin&& other) noexcept
{
    if (this == &other) return *this;
    reset();
    slot_ = std::exchange(other.slot_, nullptr);
    snapshot_ = std::exchange(other.snapshot_, nullptr);
    return *this;
}

TickMaterializationSnapshot const&
TickMaterializationStore::ReaderPin::snapshot() const noexcept
{
    assert(snapshot_ != nullptr);
    return *snapshot_;
}

TickMaterializationStore::ReaderSlot::ReaderSlot(
    TickMaterializationStore& store,
    ReaderSlotState& state) noexcept
    : store_(&store)
    , state_(&state)
{}

void TickMaterializationStore::ReaderSlot::reset() noexcept
{
    if (!state_) return;
    store_->unregister_reader(*state_);
    store_ = nullptr;
    state_ = nullptr;
}

TickMaterializationStore::ReaderSlot::~ReaderSlot()
{
    reset();
}

TickMaterializationStore::ReaderSlot::ReaderSlot(ReaderSlot&& other) noexcept
    : store_(std::exchange(other.store_, nullptr))
    , state_(std::exchange(other.state_, nullptr))
{}

TickMaterializationStore::ReaderSlot&
TickMaterializationStore::ReaderSlot::operator=(ReaderSlot&& other) noexcept
{
    if (this == &other) return *this;
    reset();
    store_ = std::exchange(other.store_, nullptr);
    state_ = std::exchange(other.state_, nullptr);
    return *this;
}

TickMaterializationStore::ReaderPin
TickMaterializationStore::ReaderSlot::pin() noexcept
{
    assert(store_ != nullptr && state_ != nullptr);
    return ReaderPin{*state_, store_->published_};
}

TickMaterializationStore::TickMaterializationStore()
    : published_owner_(std::make_unique<TickMaterializationSnapshot>())
    , published_(published_owner_.get())
{}

TickMaterializationStore::~TickMaterializationStore()
{
    assert(reader_slots_.empty());
    while (retired_) {
        auto removed = std::move(retired_);
        retired_ = std::move(removed->retired_next_);
    }
}

std::uint64_t TickMaterializationStore::promote(
    std::unique_ptr<TickMaterializationSnapshot> snapshot) noexcept
{
    assert(snapshot != nullptr);
    if (!snapshot) return promotion_sequence_;
    if (promotion_sequence_ != std::numeric_limits<std::uint64_t>::max()) {
        ++promotion_sequence_;
    }
    snapshot->promotion_sequence_ = promotion_sequence_;
    published_owner_->retired_next_ = std::move(retired_);
    retired_ = std::move(published_owner_);
    published_owner_ = std::move(snapshot);
    published_.store(published_owner_.get(), std::memory_order_seq_cst);
    return promotion_sequence_;
}

TickMaterializationStore::ReaderSlot
TickMaterializationStore::register_reader()
{
    auto state = std::make_unique<ReaderSlotState>();
    auto* pointer = state.get();
    reader_slots_.push_back(std::move(state));
    return ReaderSlot{*this, *pointer};
}

void TickMaterializationStore::unregister_reader(
    ReaderSlotState& state) noexcept
{
    assert(!state.acquiring.load(std::memory_order_seq_cst));
    assert(state.pinned.load(std::memory_order_seq_cst) == nullptr);
    auto const found = std::ranges::find_if(
        reader_slots_, [&](auto const& candidate) {
            return candidate.get() == &state;
        });
    assert(found != reader_slots_.end());
    if (found != reader_slots_.end()) reader_slots_.erase(found);
}

std::size_t TickMaterializationStore::reclaim_retired()
{
    if (std::ranges::any_of(reader_slots_, [](auto const& slot) {
            return slot->acquiring.load(std::memory_order_seq_cst);
        })) {
        return 0;
    }
    std::size_t reclaimed = 0;
    auto* link = &retired_;
    while (*link) {
        auto const pinned = std::ranges::any_of(
            reader_slots_, [&](auto const& slot) {
                return slot->pinned.load(std::memory_order_seq_cst)
                    == link->get();
            });
        if (pinned) {
            link = &(*link)->retired_next_;
            continue;
        }
        auto removed = std::move(*link);
        *link = std::move(removed->retired_next_);
        ++reclaimed;
    }
    return reclaimed;
}

std::size_t TickMaterializationStore::retired_snapshot_count() const noexcept
{
    std::size_t count = 0;
    for (auto const* snapshot = retired_.get(); snapshot != nullptr;
         snapshot = snapshot->retired_next_.get()) {
        ++count;
    }
    return count;
}

} // namespace iv
