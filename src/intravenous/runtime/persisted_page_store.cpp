#include <intravenous/runtime/persisted_page_store.h>

#include <algorithm>
#include <cassert>
#include <limits>
#include <stdexcept>
#include <utility>

namespace iv {
namespace {

[[nodiscard]] bool same_page(
    PersistedOutputId const& output,
    std::uint64_t page_index,
    PersistedSamplePage const& page) noexcept
{
    return page.output == output && page.page_index == page_index;
}

[[nodiscard]] bool same_page(
    PersistedOutputId const& output,
    std::uint64_t page_index,
    PersistedEventPage const& page) noexcept
{
    return page.output == output && page.page_index == page_index;
}

[[nodiscard]] IndexRegion page_interval(
    std::uint64_t page_index,
    std::size_t page_width)
{
    auto const width = static_cast<SampleIndex>(page_width);
    auto const maximum = std::numeric_limits<SampleIndex>::max();
    if (page_width == 0 || page_index > maximum / width) {
        throw std::invalid_argument("persisted page index exceeds the sample timeline");
    }
    auto const begin = page_index * width;
    if (begin > maximum - width) {
        throw std::invalid_argument("persisted page interval exceeds the sample timeline");
    }
    return {begin, begin + width};
}

void validate_domain(Coverage const& domain, IndexRegion interval)
{
    if (domain.empty()) {
        throw std::invalid_argument("persisted page domain cannot be empty");
    }
    for (auto const region : domain.regions()) {
        if (!interval.contains(region)) {
            throw std::invalid_argument(
                "persisted page domain lies outside its canonical page interval");
        }
    }
}

[[nodiscard]] std::size_t covered_sample_count(Coverage const& coverage)
{
    std::size_t result = 0;
    for (auto const region : coverage.regions()) {
        auto const length = region.end - region.begin;
        if (length > std::numeric_limits<std::size_t>::max() - result) {
            throw std::invalid_argument("persisted page domain is too large");
        }
        result += static_cast<std::size_t>(length);
    }
    return result;
}

[[nodiscard]] std::size_t checked_product(
    std::size_t left,
    std::size_t right)
{
    if (right != 0 && left > std::numeric_limits<std::size_t>::max() / right) {
        throw std::invalid_argument("persisted sample page payload is too large");
    }
    return left * right;
}

void validate_page(PersistedSamplePage const& page, std::size_t page_width)
{
    if (persisted_output_kind(page.output) != PortKind::sample) {
        throw std::invalid_argument("sample page requires a sample output identity");
    }
    if (!is_valid_channel_type(page.layout.channel_type)
        || !is_valid_sample_stream_layout(page.layout.sample_layout)) {
        throw std::invalid_argument("sample page has an invalid channel layout");
    }
    if (page.packing != PersistedSamplePacking::dense
        && page.packing != PersistedSamplePacking::coverage_packed) {
        throw std::invalid_argument("sample page has an invalid packing");
    }
    validate_domain(page.domain, page_interval(page.page_index, page_width));
    auto const frames = page.packing == PersistedSamplePacking::dense
        ? page_width
        : covered_sample_count(page.domain);
    auto const expected = checked_product(frames, channel_count(page.layout));
    if (page.values.size() != expected) {
        throw std::invalid_argument(
            "sample page payload does not match its packing, domain and layout");
    }
}

void validate_page(PersistedEventPage const& page, std::size_t page_width)
{
    if (persisted_output_kind(page.output) != PortKind::event) {
        throw std::invalid_argument("event page requires an event output identity");
    }
    if (page.type >= EventTypeId::count) {
        throw std::invalid_argument("event page has an invalid event type");
    }
    auto const interval = page_interval(page.page_index, page_width);
    validate_domain(page.domain, interval);
    EventTime previous = 0;
    bool first = true;
    for (auto const& event : page.events) {
        if (event.time >= page_width) {
            throw std::invalid_argument("event lies outside its persisted page");
        }
        if (!page.domain.contains(
                interval.begin + static_cast<SampleIndex>(event.time))) {
            throw std::invalid_argument("event lies outside its persisted page domain");
        }
        auto const type_matches =
            (page.type == EventTypeId::empty
                && std::holds_alternative<EmptyEvent>(event.value))
            || (page.type == EventTypeId::trigger
                && std::holds_alternative<TriggerEvent>(event.value))
            || (page.type == EventTypeId::boundary
                && std::holds_alternative<BoundaryEvent>(event.value))
            || (page.type == EventTypeId::midi
                && std::holds_alternative<MidiEvent>(event.value));
        if (!type_matches) {
            throw std::invalid_argument(
                "event page payload does not match its event type");
        }
        if (!first && event.time < previous) {
            throw std::invalid_argument(
                "persisted event page events must be in nondecreasing time order");
        }
        previous = event.time;
        first = false;
    }
}

} // namespace

PortKind persisted_output_kind(PersistedOutputId const& output) noexcept
{
    return std::visit([](auto const& identity) { return identity.kind; }, output);
}

PersistedOutputId persisted_output_id(
    graph_jit::BackgroundEvaluationPlan const& plan,
    graph_jit::PortStorageIndex storage,
    std::uint64_t generation)
{
    if (storage >= plan.storage.ports.size()) {
        throw std::invalid_argument("persisted storage index is out of range");
    }
    auto const& planned = plan.storage.ports[storage];
    if (planned.storage != graph_jit::PortStorageKind::persisted_pages
        || !planned.output_port || *planned.output_port >= plan.ports.size()) {
        throw std::invalid_argument(
            "persisted storage has no generation output identity");
    }
    auto const output_port = *planned.output_port;
    auto const& output = plan.ports[output_port];
    if (output.stable_identity) return *output.stable_identity;
    return GenerationLocalPersistedOutputId{
        .generation = generation,
        .port = output_port,
        .kind = planned.kind,
    };
}

PersistedSamplePage const* PersistedPageStore::Snapshot::find_sample_page(
    PersistedOutputId const& output,
    std::uint64_t page_index) const noexcept
{
    auto const found = std::ranges::find_if(sample_pages_, [&](auto const& page) {
        return same_page(output, page_index, *page);
    });
    return found == sample_pages_.end() ? nullptr : found->get();
}

PersistedEventPage const* PersistedPageStore::Snapshot::find_event_page(
    PersistedOutputId const& output,
    std::uint64_t page_index) const noexcept
{
    auto const found = std::ranges::find_if(event_pages_, [&](auto const& page) {
        return same_page(output, page_index, *page);
    });
    return found == event_pages_.end() ? nullptr : found->get();
}

Coverage const* PersistedPageStore::Snapshot::find_sample_coverage(
    PersistedOutputId const& output,
    ChannelLayout layout) const noexcept
{
    auto const found = std::ranges::find_if(
        sample_outputs_, [&](auto const& metadata) {
            return metadata.output == output && metadata.layout == layout;
        });
    return found == sample_outputs_.end() ? nullptr : &found->coverage;
}

Coverage const* PersistedPageStore::Snapshot::find_event_coverage(
    PersistedOutputId const& output,
    EventTypeId type) const noexcept
{
    auto const found = std::ranges::find_if(
        event_outputs_, [&](auto const& metadata) {
            return metadata.output == output && metadata.type == type;
        });
    return found == event_outputs_.end() ? nullptr : &found->coverage;
}

void PersistedPageStore::Snapshot::rebuild_output_metadata()
{
    sample_outputs_.clear();
    event_outputs_.clear();
    for (auto const& page : sample_pages_) {
        auto found = std::ranges::find_if(
            sample_outputs_, [&](auto const& metadata) {
                return metadata.output == page->output;
            });
        if (found == sample_outputs_.end()) {
            sample_outputs_.push_back(SampleOutputMetadata{
                .output = page->output,
                .coverage = page->domain,
                .layout = page->layout,
            });
        } else {
            if (found->layout != page->layout) {
                throw std::invalid_argument(
                    "persisted sample output pages disagree on channel layout");
            }
            found->coverage.include(page->domain);
        }
    }
    for (auto const& page : event_pages_) {
        auto found = std::ranges::find_if(
            event_outputs_, [&](auto const& metadata) {
                return metadata.output == page->output;
            });
        if (found == event_outputs_.end()) {
            event_outputs_.push_back(EventOutputMetadata{
                .output = page->output,
                .coverage = page->domain,
                .type = page->type,
            });
        } else {
            if (found->type != page->type) {
                throw std::invalid_argument(
                    "persisted event output pages disagree on event type");
            }
            found->coverage.include(page->domain);
        }
    }
}

PersistedPageStore::Candidate::Candidate(
    PersistedPageStore& store,
    Snapshot const& base,
    std::unique_ptr<Snapshot> successor) noexcept
    : store_(&store)
    , base_(&base)
    , base_version_(base.version())
    , successor_(std::move(successor))
{}

PersistedPageSnapshotVersion
PersistedPageStore::Candidate::target_version() const noexcept
{
    return successor_ ? successor_->version() : PersistedPageSnapshotVersion{};
}

std::size_t PersistedPageStore::Candidate::page_width() const noexcept
{
    return successor_ ? successor_->page_width() : 0;
}

PersistedPageStore::Snapshot const&
PersistedPageStore::Candidate::working_snapshot() const
{
    if (!successor_) throw std::logic_error("persisted page candidate is empty");
    return *successor_;
}

void PersistedPageStore::Candidate::put(PersistedSamplePage page)
{
    if (!successor_) throw std::logic_error("persisted page candidate is empty");
    validate_page(page, successor_->page_width_);
    auto replacement = std::make_shared<PersistedSamplePage const>(std::move(page));
    auto found = std::ranges::find_if(
        successor_->sample_pages_, [&](auto const& existing) {
            return same_page(
                replacement->output, replacement->page_index, *existing);
        });
    if (found == successor_->sample_pages_.end()) {
        auto const insertion = std::ranges::upper_bound(
            successor_->sample_pages_,
            replacement->page_index,
            {},
            [](auto const& existing) { return existing->page_index; });
        successor_->sample_pages_.insert(insertion, std::move(replacement));
    } else {
        *found = std::move(replacement);
    }
}

void PersistedPageStore::Candidate::put(PersistedEventPage page)
{
    if (!successor_) throw std::logic_error("persisted page candidate is empty");
    validate_page(page, successor_->page_width_);
    auto replacement = std::make_shared<PersistedEventPage const>(std::move(page));
    auto found = std::ranges::find_if(
        successor_->event_pages_, [&](auto const& existing) {
            return same_page(
                replacement->output, replacement->page_index, *existing);
        });
    if (found == successor_->event_pages_.end()) {
        auto const insertion = std::ranges::upper_bound(
            successor_->event_pages_,
            replacement->page_index,
            {},
            [](auto const& existing) { return existing->page_index; });
        successor_->event_pages_.insert(insertion, std::move(replacement));
    } else {
        *found = std::move(replacement);
    }
}

void PersistedPageStore::Candidate::erase_page(
    PersistedOutputId const& output,
    std::uint64_t page_index)
{
    if (!successor_) throw std::logic_error("persisted page candidate is empty");
    if (persisted_output_kind(output) == PortKind::sample) {
        std::erase_if(successor_->sample_pages_, [&](auto const& page) {
            return same_page(output, page_index, *page);
        });
    } else {
        std::erase_if(successor_->event_pages_, [&](auto const& page) {
            return same_page(output, page_index, *page);
        });
    }
}

void PersistedPageStore::Candidate::erase_output(
    PersistedOutputId const& output)
{
    if (!successor_) throw std::logic_error("persisted page candidate is empty");
    if (persisted_output_kind(output) == PortKind::sample) {
        std::erase_if(successor_->sample_pages_, [&](auto const& page) {
            return page->output == output;
        });
    } else {
        std::erase_if(successor_->event_pages_, [&](auto const& page) {
            return page->output == output;
        });
    }
}

PersistedPageStore::ReaderPin::ReaderPin(
    ReaderSlotState& slot,
    std::atomic<Snapshot const*> const& published) noexcept
    : slot_(&slot)
{
    assert(slot.pinned.load(std::memory_order_seq_cst) == nullptr);
    slot.acquiring.store(true, std::memory_order_seq_cst);
    snapshot_ = published.load(std::memory_order_seq_cst);
    slot.pinned.store(snapshot_, std::memory_order_seq_cst);
    slot.acquiring.store(false, std::memory_order_seq_cst);
}

void PersistedPageStore::ReaderPin::reset() noexcept
{
    if (!slot_) return;
    slot_->pinned.store(nullptr, std::memory_order_seq_cst);
    slot_ = nullptr;
    snapshot_ = nullptr;
}

PersistedPageStore::ReaderPin::~ReaderPin()
{
    reset();
}

PersistedPageStore::ReaderPin::ReaderPin(ReaderPin&& other) noexcept
    : slot_(std::exchange(other.slot_, nullptr))
    , snapshot_(std::exchange(other.snapshot_, nullptr))
{}

PersistedPageStore::ReaderPin& PersistedPageStore::ReaderPin::operator=(
    ReaderPin&& other) noexcept
{
    if (this == &other) return *this;
    reset();
    slot_ = std::exchange(other.slot_, nullptr);
    snapshot_ = std::exchange(other.snapshot_, nullptr);
    return *this;
}

PersistedPageStore::Snapshot const&
PersistedPageStore::ReaderPin::snapshot() const noexcept
{
    assert(snapshot_ != nullptr);
    return *snapshot_;
}

PersistedPageStore::ReaderSlot::ReaderSlot(
    PersistedPageStore& store,
    ReaderSlotState& state) noexcept
    : store_(&store)
    , state_(&state)
{}

void PersistedPageStore::ReaderSlot::reset() noexcept
{
    if (!state_) return;
    store_->unregister_reader(*state_);
    store_ = nullptr;
    state_ = nullptr;
}

PersistedPageStore::ReaderSlot::~ReaderSlot()
{
    reset();
}

PersistedPageStore::ReaderSlot::ReaderSlot(ReaderSlot&& other) noexcept
    : store_(std::exchange(other.store_, nullptr))
    , state_(std::exchange(other.state_, nullptr))
{}

PersistedPageStore::ReaderSlot& PersistedPageStore::ReaderSlot::operator=(
    ReaderSlot&& other) noexcept
{
    if (this == &other) return *this;
    reset();
    store_ = std::exchange(other.store_, nullptr);
    state_ = std::exchange(other.state_, nullptr);
    return *this;
}

PersistedPageStore::ReaderPin PersistedPageStore::ReaderSlot::pin() noexcept
{
    assert(store_ != nullptr && state_ != nullptr);
    return ReaderPin{*state_, store_->published_};
}

PersistedPageStore::PersistedPageStore()
    : published_owner_(std::make_unique<Snapshot>())
    , published_(published_owner_.get())
{}

PersistedPageStore::~PersistedPageStore()
{
    assert(reader_slots_.empty());
}

PersistedPageStore::Candidate PersistedPageStore::begin_candidate(
    std::uint64_t target_semantic_version,
    std::size_t page_width)
{
    if (page_width == 0) {
        throw std::invalid_argument("persisted page width must be nonzero");
    }
    auto const* base = published_.load(std::memory_order_seq_cst);
    if (target_semantic_version < base->version_.semantic) {
        throw std::invalid_argument(
            "persisted page candidate semantic version cannot move backwards");
    }
    if (base->version_.page
        == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("persisted page version is exhausted");
    }
    if (base->page_width_ != 0 && base->page_width_ != page_width
        && (!base->sample_pages_.empty() || !base->event_pages_.empty())) {
        throw std::invalid_argument(
            "persisted pages must be explicitly repaged before changing page width");
    }

    auto successor = std::make_unique<Snapshot>();
    successor->version_ = {
        .semantic = target_semantic_version,
        .page = base->version_.page + 1,
    };
    successor->page_width_ = page_width;
    successor->sample_pages_ = base->sample_pages_;
    successor->event_pages_ = base->event_pages_;
    return Candidate{*this, *base, std::move(successor)};
}

PersistedPagePublishResult PersistedPageStore::publish(Candidate&& candidate)
{
    if (candidate.store_ != this || !candidate.successor_) {
        throw std::invalid_argument(
            "persisted page candidate does not belong to this store");
    }
    auto const* current = published_.load(std::memory_order_seq_cst);
    if (current != candidate.base_
        || current->version() != candidate.base_version_) {
        candidate = {};
        return PersistedPagePublishResult::stale_base;
    }

    candidate.successor_->rebuild_output_metadata();

    retired_.push_back(std::move(published_owner_));
    published_owner_ = std::move(candidate.successor_);
    published_.store(published_owner_.get(), std::memory_order_seq_cst);
    candidate.store_ = nullptr;
    candidate.base_ = nullptr;
    return PersistedPagePublishResult::published;
}

PersistedPageStore::ReaderSlot PersistedPageStore::register_reader()
{
    auto state = std::make_unique<ReaderSlotState>();
    auto* pointer = state.get();
    reader_slots_.push_back(std::move(state));
    return ReaderSlot{*this, *pointer};
}

void PersistedPageStore::unregister_reader(ReaderSlotState& state) noexcept
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

std::size_t PersistedPageStore::reclaim_retired()
{
    // A reader announces before loading published_. If reclamation observes an
    // acquisition in flight it defers the whole scan; otherwise that reader's
    // later published_ load can only select the current, non-retired root.
    if (std::ranges::any_of(reader_slots_, [](auto const& slot) {
            return slot->acquiring.load(std::memory_order_seq_cst);
        })) {
        return 0;
    }
    auto const before = retired_.size();
    std::erase_if(retired_, [&](auto const& snapshot) {
        return std::ranges::none_of(reader_slots_, [&](auto const& slot) {
            return slot->pinned.load(std::memory_order_seq_cst)
                == snapshot.get();
        });
    });
    return before - retired_.size();
}

} // namespace iv
