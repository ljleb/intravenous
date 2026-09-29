#include <intravenous/runtime/tick_capture_store.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace iv {
namespace {

[[nodiscard]] std::size_t align_up(
    std::size_t value, std::size_t alignment)
{
    auto const remainder = value % alignment;
    if (remainder == 0) return value;
    if (value > std::numeric_limits<std::size_t>::max()
            - (alignment - remainder)) {
        throw std::length_error("Tick capture block stride is too large");
    }
    return value + alignment - remainder;
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

struct TickCaptureStore::Block {
    Block* free_next = nullptr;
    std::atomic<Block*> sealed_next{nullptr};
    Block* retired_next = nullptr;
    std::byte* payload = nullptr;
    std::size_t payload_capacity = 0;
    std::size_t payload_size = 0;
    CaptureSequence sequence = 0;
    PersistedOutputId const* output = nullptr;
    SampleIndex begin = 0;
    std::size_t sample_count = 0;
    TickCapturePayloadKind payload_kind = TickCapturePayloadKind::samples;
    ChannelLayout sample_layout{};
    EventTypeId event_type = EventTypeId::empty;
    std::size_t event_count = 0;
};

class TickCaptureStore::Impl {
public:
    enum class AccessState : std::uint8_t {
        idle,
        starting,
        callback,
    };

    static_assert(std::atomic<Block*>::is_always_lock_free);
    static_assert(std::atomic<CaptureSequence>::is_always_lock_free);
    static_assert(std::atomic<std::size_t>::is_always_lock_free);
    static_assert(std::atomic<AccessState>::is_always_lock_free);

    struct Slab {
        std::unique_ptr<Block[]> blocks{};
        std::unique_ptr<std::byte[]> payload{};
        std::size_t block_count = 0;
    };

    explicit Impl(std::size_t selected_payload_capacity)
        : payload_capacity(selected_payload_capacity)
        , payload_stride(align_up(
            selected_payload_capacity, alignof(std::max_align_t)))
        , sentinel(std::make_unique<Block>())
        , audio_tail(sentinel.get())
        , consumer_head(sentinel.get())
    {
        if (payload_capacity == 0) {
            throw std::invalid_argument(
                "Tick capture payload capacity must be non-zero");
        }
    }

    std::size_t payload_capacity = 0;
    std::size_t payload_stride = 0;
    std::mutex control_mutex{};
    std::vector<std::unique_ptr<PersistedOutputId const>> outputs{};
    std::vector<Slab> slabs{};

    std::atomic<Block*> free_head{nullptr};
    std::atomic<std::size_t> free_count{0};
    std::atomic<AccessState> access_state{AccessState::idle};
    std::atomic<CaptureSequence> callback_begin{0};

    std::unique_ptr<Block> sentinel{};
    Block* audio_tail = nullptr;
    Block* consumer_head = nullptr;
    CaptureSequence next_sequence = 0;
    CaptureSequence processed = 0;
    std::atomic<CaptureSequence> published{0};

    Block* retired_head = nullptr;
    Block* retired_tail = nullptr;
    std::atomic<std::size_t> retired_count{0};

    void push_free(Block& block) noexcept
    {
        block.output = nullptr;
        block.payload_size = 0;
        block.sample_count = 0;
        block.event_count = 0;
        block.retired_next = nullptr;
        auto* head = free_head.load(std::memory_order_relaxed);
        do {
            block.free_next = head;
        } while (!free_head.compare_exchange_weak(
            head,
            &block,
            std::memory_order_release,
            std::memory_order_relaxed));
        free_count.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] Block* pop_free() noexcept
    {
        auto* head = free_head.load(std::memory_order_acquire);
        while (head != nullptr
            && !free_head.compare_exchange_weak(
                head,
                head->free_next,
                std::memory_order_acquire,
                std::memory_order_relaxed)) {
        }
        if (head) {
            head->free_next = nullptr;
            free_count.fetch_sub(1, std::memory_order_relaxed);
        }
        return head;
    }

    void retire(Block& block) noexcept
    {
        block.retired_next = nullptr;
        if (retired_tail) {
            retired_tail->retired_next = &block;
        } else {
            retired_head = &block;
        }
        retired_tail = &block;
        retired_count.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] TickCaptureRecordView view(Block const& block) const noexcept
    {
        return {
            .sequence = block.sequence,
            .output = block.output,
            .begin = block.begin,
            .sample_count = block.sample_count,
            .payload_kind = block.payload_kind,
            .sample_layout = block.sample_layout,
            .event_type = block.event_type,
            .event_count = block.event_count,
            .payload = {block.payload, block.payload_size},
        };
    }
};

PersistedOutputId const& TickCaptureOutputHandle::output() const noexcept
{
    assert(output_ != nullptr);
    return *output_;
}

TickCaptureStore::TickCaptureStore(std::size_t block_payload_capacity)
    : impl_(std::make_unique<Impl>(block_payload_capacity))
{}

TickCaptureStore::~TickCaptureStore() = default;

TickCaptureOutputHandle TickCaptureStore::register_output(
    PersistedOutputId output)
{
    std::scoped_lock lock(impl_->control_mutex);
    auto const found = std::ranges::find_if(
        impl_->outputs,
        [&](auto const& candidate) { return *candidate == output; });
    if (found != impl_->outputs.end()) {
        return {*this, **found};
    }
    auto stored = std::make_unique<PersistedOutputId const>(std::move(output));
    auto const* result = stored.get();
    impl_->outputs.push_back(std::move(stored));
    return {*this, *result};
}

void TickCaptureStore::provision(std::size_t block_count)
{
    if (block_count == 0) return;
    if (block_count > std::numeric_limits<std::size_t>::max()
            / impl_->payload_stride) {
        throw std::length_error("Tick capture slab is too large");
    }

    Impl::Slab slab{
        .blocks = std::make_unique<Block[]>(block_count),
        .payload = std::make_unique<std::byte[]>(
            block_count * impl_->payload_stride),
        .block_count = block_count,
    };
    for (std::size_t index = 0; index < block_count; ++index) {
        slab.blocks[index].payload =
            slab.payload.get() + index * impl_->payload_stride;
        slab.blocks[index].payload_capacity = impl_->payload_capacity;
    }

    std::scoped_lock lock(impl_->control_mutex);
    impl_->slabs.push_back(std::move(slab));
    auto& published = impl_->slabs.back();
    for (std::size_t index = 0; index < published.block_count; ++index) {
        impl_->push_free(published.blocks[index]);
    }
}

TickCaptureStore::CallbackScope TickCaptureStore::begin_callback() noexcept
{
    auto expected = Impl::AccessState::idle;
    if (!impl_->access_state.compare_exchange_strong(
            expected, Impl::AccessState::starting,
            std::memory_order_acq_rel,
            std::memory_order_relaxed)) {
        return {};
    }
    impl_->callback_begin.store(
        impl_->published.load(std::memory_order_acquire),
        std::memory_order_relaxed);
    impl_->access_state.store(
        Impl::AccessState::callback, std::memory_order_release);
    return CallbackScope{*this};
}

void TickCaptureStore::end_callback() noexcept
{
    impl_->access_state.store(
        Impl::AccessState::idle, std::memory_order_release);
}

TickCaptureStore::Writer TickCaptureStore::acquire() noexcept
{
    if (impl_->access_state.load(std::memory_order_relaxed)
        != Impl::AccessState::callback) {
        return {};
    }
    auto* block = impl_->pop_free();
    return block ? Writer{*this, *block} : Writer{};
}

void TickCaptureStore::abandon(Block& block) noexcept
{
    impl_->push_free(block);
}

bool TickCaptureStore::seal_samples(
    Block& block,
    TickCaptureOutputHandle output,
    SampleIndex begin,
    std::size_t sample_count,
    ChannelLayout layout) noexcept
{
    if (output.owner_ != this || output.output_ == nullptr
        || persisted_output_kind(*output.output_) != PortKind::sample
        || sample_count == 0
        || !is_valid_channel_type(layout.channel_type)
        || !is_valid_sample_stream_layout(layout.sample_layout)
        || sample_count > std::numeric_limits<SampleIndex>::max() - begin) {
        return false;
    }
    auto const channels = channel_count(layout);
    if (sample_count > std::numeric_limits<std::size_t>::max() / channels) {
        return false;
    }
    auto const values = sample_count * channels;
    if (values > std::numeric_limits<std::size_t>::max() / sizeof(Sample)) {
        return false;
    }
    auto const bytes = values * sizeof(Sample);
    if (bytes > block.payload_capacity
        || impl_->next_sequence == std::numeric_limits<CaptureSequence>::max()) {
        return false;
    }

    block.output = output.output_;
    block.begin = begin;
    block.sample_count = sample_count;
    block.payload_kind = TickCapturePayloadKind::samples;
    block.sample_layout = layout;
    block.event_type = EventTypeId::empty;
    block.event_count = 0;
    block.payload_size = bytes;
    block.sequence = impl_->next_sequence;
    block.sealed_next.store(nullptr, std::memory_order_relaxed);
    impl_->audio_tail->sealed_next.store(&block, std::memory_order_release);
    impl_->audio_tail = &block;
    ++impl_->next_sequence;
    impl_->published.store(impl_->next_sequence, std::memory_order_release);
    return true;
}

bool TickCaptureStore::seal_events(
    Block& block,
    TickCaptureOutputHandle output,
    SampleIndex begin,
    std::size_t sample_count,
    EventTypeId type,
    std::size_t event_count) noexcept
{
    if (output.owner_ != this || output.output_ == nullptr
        || persisted_output_kind(*output.output_) != PortKind::event
        || sample_count == 0 || type >= EventTypeId::count
        || sample_count > std::numeric_limits<SampleIndex>::max() - begin
        || event_count > std::numeric_limits<std::size_t>::max()
                / sizeof(TimedEvent)) {
        return false;
    }
    auto const bytes = event_count * sizeof(TimedEvent);
    if (bytes > block.payload_capacity
        || impl_->next_sequence == std::numeric_limits<CaptureSequence>::max()) {
        return false;
    }
    auto const end = begin + static_cast<SampleIndex>(sample_count);
    auto const* events = reinterpret_cast<TimedEvent const*>(block.payload);
    for (std::size_t index = 0; index < event_count; ++index) {
        auto const time = static_cast<SampleIndex>(events[index].time);
        if (time < begin || time >= end
            || !event_matches_type(type, events[index].value)
            || (index != 0 && events[index].time < events[index - 1].time)) {
            return false;
        }
    }

    block.output = output.output_;
    block.begin = begin;
    block.sample_count = sample_count;
    block.payload_kind = TickCapturePayloadKind::events;
    block.sample_layout = {};
    block.event_type = type;
    block.event_count = event_count;
    block.payload_size = bytes;
    block.sequence = impl_->next_sequence;
    block.sealed_next.store(nullptr, std::memory_order_relaxed);
    impl_->audio_tail->sealed_next.store(&block, std::memory_order_release);
    impl_->audio_tail = &block;
    ++impl_->next_sequence;
    impl_->published.store(impl_->next_sequence, std::memory_order_release);
    return true;
}

TickCaptureStore::Batch TickCaptureStore::snapshot_pending() noexcept
{
    auto const cutoff = impl_->published.load(std::memory_order_acquire);
    auto* first = impl_->consumer_head->sealed_next.load(
        std::memory_order_acquire);
    return {*this, first, impl_->processed, cutoff};
}

bool TickCaptureStore::commit(Batch&& batch) noexcept
{
    std::scoped_lock lock(impl_->control_mutex);
    if (batch.store_ != this || batch.begin_ != impl_->processed
        || batch.cutoff_ < batch.begin_
        || batch.cutoff_
            > impl_->published.load(std::memory_order_acquire)) {
        return false;
    }
    if (batch.begin_ == batch.cutoff_) {
        batch = {};
        return true;
    }

    auto* current = impl_->consumer_head->sealed_next.load(
        std::memory_order_acquire);
    if (current != batch.first_) return false;
    for (auto sequence = batch.begin_; sequence < batch.cutoff_; ++sequence) {
        if (current == nullptr || current->sequence != sequence) return false;
        current = current->sealed_next.load(std::memory_order_acquire);
    }

    for (auto sequence = batch.begin_; sequence < batch.cutoff_; ++sequence) {
        auto* next = impl_->consumer_head->sealed_next.load(
            std::memory_order_acquire);
        auto* previous = impl_->consumer_head;
        impl_->consumer_head = next;
        if (previous != impl_->sentinel.get()) impl_->retire(*previous);
    }
    impl_->processed = batch.cutoff_;
    batch = {};
    return true;
}

std::size_t TickCaptureStore::reclaim_committed() noexcept
{
    std::scoped_lock lock(impl_->control_mutex);
    auto const state = impl_->access_state.load(std::memory_order_acquire);
    auto const cutoff = state == Impl::AccessState::idle
        ? std::numeric_limits<CaptureSequence>::max()
        : state == Impl::AccessState::callback
            ? impl_->callback_begin.load(std::memory_order_relaxed)
            : CaptureSequence{0};
    std::size_t reclaimed = 0;
    while (impl_->retired_head
        && impl_->retired_head->sequence < cutoff) {
        auto* block = impl_->retired_head;
        impl_->retired_head = block->retired_next;
        if (!impl_->retired_head) impl_->retired_tail = nullptr;
        impl_->retired_count.fetch_sub(1, std::memory_order_relaxed);
        impl_->push_free(*block);
        ++reclaimed;
    }
    return reclaimed;
}

std::size_t TickCaptureStore::block_payload_capacity() const noexcept
{
    return impl_->payload_capacity;
}

std::size_t TickCaptureStore::free_block_count() const noexcept
{
    return impl_->free_count.load(std::memory_order_relaxed);
}

std::size_t TickCaptureStore::retired_block_count() const noexcept
{
    return impl_->retired_count.load(std::memory_order_relaxed);
}

CaptureSequence TickCaptureStore::processed_sequence() const noexcept
{
    return impl_->processed;
}

CaptureSequence TickCaptureStore::published_sequence() const noexcept
{
    return impl_->published.load(std::memory_order_acquire);
}

TickCaptureStore::Writer::~Writer()
{
    reset();
}

TickCaptureStore::Writer::Writer(Writer&& other) noexcept
    : store_(std::exchange(other.store_, nullptr))
    , block_(std::exchange(other.block_, nullptr))
{}

TickCaptureStore::Writer& TickCaptureStore::Writer::operator=(
    Writer&& other) noexcept
{
    if (this == &other) return *this;
    reset();
    store_ = std::exchange(other.store_, nullptr);
    block_ = std::exchange(other.block_, nullptr);
    return *this;
}

void TickCaptureStore::Writer::reset() noexcept
{
    if (store_ && block_) store_->abandon(*block_);
    store_ = nullptr;
    block_ = nullptr;
}

std::span<std::byte> TickCaptureStore::Writer::payload() noexcept
{
    return block_
        ? std::span<std::byte>{block_->payload, block_->payload_capacity}
        : std::span<std::byte>{};
}

bool TickCaptureStore::Writer::seal_samples(
    TickCaptureOutputHandle output,
    SampleIndex begin,
    std::size_t sample_count,
    ChannelLayout layout) noexcept
{
    if (!store_ || !block_
        || !store_->seal_samples(
            *block_, output, begin, sample_count, layout)) {
        return false;
    }
    store_ = nullptr;
    block_ = nullptr;
    return true;
}

bool TickCaptureStore::Writer::seal_events(
    TickCaptureOutputHandle output,
    SampleIndex begin,
    std::size_t sample_count,
    EventTypeId type,
    std::size_t event_count) noexcept
{
    if (!store_ || !block_
        || !store_->seal_events(
            *block_, output, begin, sample_count, type, event_count)) {
        return false;
    }
    store_ = nullptr;
    block_ = nullptr;
    return true;
}

TickCaptureStore::CallbackScope::~CallbackScope()
{
    if (store_) store_->end_callback();
}

TickCaptureStore::CallbackScope::CallbackScope(CallbackScope&& other) noexcept
    : store_(std::exchange(other.store_, nullptr))
{}

TickCaptureStore::CallbackScope& TickCaptureStore::CallbackScope::operator=(
    CallbackScope&& other) noexcept
{
    if (this == &other) return *this;
    if (store_) store_->end_callback();
    store_ = std::exchange(other.store_, nullptr);
    return *this;
}

TickCaptureStore::Batch::Batch(Batch&& other) noexcept
    : store_(std::exchange(other.store_, nullptr))
    , first_(std::exchange(other.first_, nullptr))
    , begin_(std::exchange(other.begin_, 0))
    , cutoff_(std::exchange(other.cutoff_, 0))
{}

TickCaptureStore::Batch& TickCaptureStore::Batch::operator=(
    Batch&& other) noexcept
{
    if (this == &other) return *this;
    store_ = std::exchange(other.store_, nullptr);
    first_ = std::exchange(other.first_, nullptr);
    begin_ = std::exchange(other.begin_, 0);
    cutoff_ = std::exchange(other.cutoff_, 0);
    return *this;
}

std::size_t TickCaptureStore::Batch::size() const noexcept
{
    return cutoff_ >= begin_
        ? static_cast<std::size_t>(cutoff_ - begin_)
        : 0;
}

void TickCaptureStore::Batch::for_each(
    void* data, VisitRecord visitor) const
{
    if (!store_ || visitor == nullptr) return;
    auto* current = first_;
    for (auto sequence = begin_; sequence < cutoff_; ++sequence) {
        if (!current || current->sequence != sequence) return;
        auto const record = store_->impl_->view(*current);
        visitor(data, record);
        current = current->sealed_next.load(std::memory_order_acquire);
    }
}

} // namespace iv
