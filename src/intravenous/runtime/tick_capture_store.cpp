#include <intravenous/runtime/tick_capture_store.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>
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
    Block* payload_next = nullptr;
    Block* retired_next = nullptr;
    std::byte* payload = nullptr;
    std::size_t segment_size = 0;

    // The remaining fields are meaningful only on a published record head.
    std::size_t record_payload_size = 0;
    CaptureSequence sequence = 0;
    TickCaptureOutputHandle output{};
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
    std::vector<Slab> slabs{};
    std::uint64_t next_output_id = 1;

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
        block.sealed_next.store(nullptr, std::memory_order_relaxed);
        block.payload_next = nullptr;
        block.retired_next = nullptr;
        block.segment_size = 0;
        block.record_payload_size = 0;
        block.output = {};
        block.sample_count = 0;
        block.event_count = 0;
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
};

void TickCapturePayloadView::for_each_segment(
    void* data, VisitSegment visitor) const
{
    if (!first_ || !visitor) return;
    auto const* block = static_cast<TickCaptureStore::Block const*>(first_);
    auto remaining = size_;
    while (block && remaining != 0) {
        auto const count = std::min(block->segment_size, remaining);
        visitor(data, std::span<std::byte const>{block->payload, count});
        remaining -= count;
        block = block->payload_next;
    }
}

bool TickCapturePayloadView::copy_to(
    std::span<std::byte> destination) const noexcept
{
    if (destination.size() != size_ || (!first_ && size_ != 0)) return false;
    auto const* block = static_cast<TickCaptureStore::Block const*>(first_);
    std::size_t offset = 0;
    while (block && offset != size_) {
        auto const count = std::min(block->segment_size, size_ - offset);
        std::memcpy(destination.data() + offset, block->payload, count);
        offset += count;
        block = block->payload_next;
    }
    return offset == size_;
}

TickCaptureStore::TickCaptureStore(std::size_t block_payload_capacity)
    : impl_(std::make_unique<Impl>(block_payload_capacity))
{}

TickCaptureStore::~TickCaptureStore() = default;

TickCaptureOutputHandle TickCaptureStore::register_output(PortKind kind)
{
    if (kind != PortKind::sample && kind != PortKind::event) {
        throw std::invalid_argument(
            "Tick capture output must be a sample or event port");
    }
    std::scoped_lock lock(impl_->control_mutex);
    if (impl_->next_output_id == 0) {
        throw std::length_error("Tick capture output identity space exhausted");
    }
    return {*this, impl_->next_output_id++, kind};
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

TickCaptureStore::RecordWriter TickCaptureStore::reserve_record(
    std::size_t payload_size) noexcept
{
    if (impl_->access_state.load(std::memory_order_relaxed)
        != Impl::AccessState::callback) {
        return {};
    }
    auto const block_count = payload_size == 0
        ? std::size_t{1}
        : std::size_t{1} + (payload_size - 1) / impl_->payload_capacity;
    Block* head = nullptr;
    Block* tail = nullptr;
    auto remaining = payload_size;
    for (std::size_t index = 0; index < block_count; ++index) {
        auto* block = impl_->pop_free();
        if (!block) {
            if (head) abandon_record(*head);
            return {};
        }
        block->segment_size = std::min(remaining, impl_->payload_capacity);
        block->payload_next = nullptr;
        if (tail) {
            tail->payload_next = block;
        } else {
            head = block;
        }
        tail = block;
        remaining -= block->segment_size;
    }
    head->record_payload_size = payload_size;
    return {*this, *head};
}

void TickCaptureStore::abandon_record(Block& head) noexcept
{
    auto* block = &head;
    while (block) {
        auto* next = block->payload_next;
        impl_->push_free(*block);
        block = next;
    }
}

TickCaptureRecordView TickCaptureStore::view(Block const& head) const noexcept
{
    return {
        .sequence = head.sequence,
        .output = head.output,
        .begin = head.begin,
        .sample_count = head.sample_count,
        .payload_kind = head.payload_kind,
        .sample_layout = head.sample_layout,
        .event_type = head.event_type,
        .event_count = head.event_count,
        .payload = TickCapturePayloadView{
            &head, head.record_payload_size},
    };
}

bool TickCaptureStore::seal_samples(
    Block& head,
    TickCaptureOutputHandle output,
    SampleIndex begin,
    std::size_t sample_count,
    ChannelLayout layout) noexcept
{
    if (output.owner_ != this || output.id_ == 0
        || output.kind_ != PortKind::sample
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
    if (bytes != head.record_payload_size
        || impl_->next_sequence == std::numeric_limits<CaptureSequence>::max()) {
        return false;
    }

    head.output = output;
    head.begin = begin;
    head.sample_count = sample_count;
    head.payload_kind = TickCapturePayloadKind::samples;
    head.sample_layout = layout;
    head.event_type = EventTypeId::empty;
    head.event_count = 0;
    for (auto* block = &head; block; block = block->payload_next) {
        block->sequence = impl_->next_sequence;
    }
    head.sealed_next.store(nullptr, std::memory_order_relaxed);
    impl_->audio_tail->sealed_next.store(&head, std::memory_order_release);
    impl_->audio_tail = &head;
    ++impl_->next_sequence;
    impl_->published.store(impl_->next_sequence, std::memory_order_release);
    return true;
}

bool TickCaptureStore::seal_events(
    Block& head,
    TickCaptureOutputHandle output,
    SampleIndex begin,
    std::size_t sample_count,
    EventTypeId type,
    std::size_t event_count) noexcept
{
    if (output.owner_ != this || output.id_ == 0
        || output.kind_ != PortKind::event
        || sample_count == 0 || type >= EventTypeId::count
        || sample_count > std::numeric_limits<SampleIndex>::max() - begin
        || event_count > std::numeric_limits<std::size_t>::max()
                / sizeof(TimedEvent)) {
        return false;
    }
    auto const bytes = event_count * sizeof(TimedEvent);
    if (bytes != head.record_payload_size
        || impl_->next_sequence == std::numeric_limits<CaptureSequence>::max()) {
        return false;
    }

    auto const* read_block = &head;
    std::size_t read_block_offset = 0;
    auto read = [&](std::span<std::byte> destination) {
        std::size_t copied = 0;
        while (read_block && copied != destination.size()) {
            auto const count = std::min(
                read_block->segment_size - read_block_offset,
                destination.size() - copied);
            std::memcpy(
                destination.data() + copied,
                read_block->payload + read_block_offset,
                count);
            copied += count;
            read_block_offset += count;
            if (read_block_offset == read_block->segment_size) {
                read_block = read_block->payload_next;
                read_block_offset = 0;
            }
        }
        return copied == destination.size();
    };

    auto const end = begin + static_cast<SampleIndex>(sample_count);
    TimedEvent previous{};
    for (std::size_t index = 0; index < event_count; ++index) {
        TimedEvent event{};
        if (!read(std::as_writable_bytes(
                std::span{&event, std::size_t{1}}))) {
            return false;
        }
        auto const time = static_cast<SampleIndex>(event.time);
        if (time < begin || time >= end
            || !event_matches_type(type, event.value)
            || (index != 0 && event.time < previous.time)) {
            return false;
        }
        previous = event;
    }

    head.output = output;
    head.begin = begin;
    head.sample_count = sample_count;
    head.payload_kind = TickCapturePayloadKind::events;
    head.sample_layout = {};
    head.event_type = type;
    head.event_count = event_count;
    for (auto* block = &head; block; block = block->payload_next) {
        block->sequence = impl_->next_sequence;
    }
    head.sealed_next.store(nullptr, std::memory_order_relaxed);
    impl_->audio_tail->sealed_next.store(&head, std::memory_order_release);
    impl_->audio_tail = &head;
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
        if (previous != impl_->sentinel.get()) {
            for (auto* block = previous; block; block = block->payload_next) {
                impl_->retire(*block);
            }
        }
    }
    if (impl_->consumer_head != impl_->sentinel.get()) {
        auto* payload = std::exchange(
            impl_->consumer_head->payload_next, nullptr);
        while (payload) {
            auto* next = payload->payload_next;
            impl_->retire(*payload);
            payload = next;
        }
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

TickCaptureStore::RecordWriter::~RecordWriter()
{
    reset();
}

TickCaptureStore::RecordWriter::RecordWriter(RecordWriter&& other) noexcept
    : store_(std::exchange(other.store_, nullptr))
    , head_(std::exchange(other.head_, nullptr))
    , write_block_(std::exchange(other.write_block_, nullptr))
    , write_block_offset_(std::exchange(other.write_block_offset_, 0))
    , written_(std::exchange(other.written_, 0))
{}

TickCaptureStore::RecordWriter& TickCaptureStore::RecordWriter::operator=(
    RecordWriter&& other) noexcept
{
    if (this == &other) return *this;
    reset();
    store_ = std::exchange(other.store_, nullptr);
    head_ = std::exchange(other.head_, nullptr);
    write_block_ = std::exchange(other.write_block_, nullptr);
    write_block_offset_ = std::exchange(other.write_block_offset_, 0);
    written_ = std::exchange(other.written_, 0);
    return *this;
}

void TickCaptureStore::RecordWriter::reset() noexcept
{
    if (store_ && head_) store_->abandon_record(*head_);
    store_ = nullptr;
    head_ = nullptr;
    write_block_ = nullptr;
    write_block_offset_ = 0;
    written_ = 0;
}

std::size_t TickCaptureStore::RecordWriter::payload_size() const noexcept
{
    return head_ ? head_->record_payload_size : 0;
}

bool TickCaptureStore::RecordWriter::append(
    std::span<std::byte const> source) noexcept
{
    if (!head_ || written_ > head_->record_payload_size
        || source.size() > head_->record_payload_size - written_) {
        return false;
    }
    std::size_t copied = 0;
    while (write_block_ && copied != source.size()) {
        auto const count = std::min(
            write_block_->segment_size - write_block_offset_,
            source.size() - copied);
        std::memcpy(
            write_block_->payload + write_block_offset_,
            source.data() + copied,
            count);
        copied += count;
        write_block_offset_ += count;
        if (write_block_offset_ == write_block_->segment_size) {
            write_block_ = write_block_->payload_next;
            write_block_offset_ = 0;
        }
    }
    written_ += copied;
    return copied == source.size();
}

bool TickCaptureStore::RecordWriter::seal_samples(
    TickCaptureOutputHandle output,
    SampleIndex begin,
    std::size_t sample_count,
    ChannelLayout layout) noexcept
{
    if (!store_ || !head_
        || written_ != head_->record_payload_size
        || !store_->seal_samples(
            *head_, output, begin, sample_count, layout)) {
        return false;
    }
    store_ = nullptr;
    head_ = nullptr;
    write_block_ = nullptr;
    write_block_offset_ = 0;
    written_ = 0;
    return true;
}

bool TickCaptureStore::RecordWriter::seal_events(
    TickCaptureOutputHandle output,
    SampleIndex begin,
    std::size_t sample_count,
    EventTypeId type,
    std::size_t event_count) noexcept
{
    if (!store_ || !head_
        || written_ != head_->record_payload_size
        || !store_->seal_events(
            *head_, output, begin, sample_count, type, event_count)) {
        return false;
    }
    store_ = nullptr;
    head_ = nullptr;
    write_block_ = nullptr;
    write_block_offset_ = 0;
    written_ = 0;
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
        auto const record = store_->view(*current);
        visitor(data, record);
        current = current->sealed_next.load(std::memory_order_acquire);
    }
}

} // namespace iv
