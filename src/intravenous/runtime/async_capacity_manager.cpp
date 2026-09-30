#include <intravenous/runtime/async_capacity_manager.h>

#include <bit>
#include <cassert>
#include <limits>
#include <stdexcept>
#include <utility>

namespace iv {

namespace {

[[nodiscard]] std::size_t rounded_block_count(
    std::size_t count, std::size_t granularity)
{
    auto const remainder = count % granularity;
    if (remainder == 0) return count;
    auto const rounding = granularity - remainder;
    if (count > std::numeric_limits<std::size_t>::max() - rounding) {
        throw std::length_error("Async capacity slab is too large");
    }
    return count + rounding;
}

} // namespace

ProducedBlockChain::ProducedBlockChain(
    ProducerReserve& owner,
    AsyncQueueBlock& first,
    AsyncQueueBlock& last) noexcept
    : owner_(&owner), first_(&first), last_(&last)
{}

ProducedBlockChain::~ProducedBlockChain()
{
    reset();
}

ProducedBlockChain::ProducedBlockChain(ProducedBlockChain&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr))
    , first_(std::exchange(other.first_, nullptr))
    , last_(std::exchange(other.last_, nullptr))
{}

ProducedBlockChain& ProducedBlockChain::operator=(
    ProducedBlockChain&& other) noexcept
{
    if (this == &other) return *this;
    reset();
    owner_ = std::exchange(other.owner_, nullptr);
    first_ = std::exchange(other.first_, nullptr);
    last_ = std::exchange(other.last_, nullptr);
    return *this;
}

void ProducedBlockChain::reset() noexcept
{
    if (owner_ && first_) owner_->return_chain(first_);
    owner_ = nullptr;
    first_ = nullptr;
    last_ = nullptr;
}

void ProducedBlockChain::for_each(
    void* data,
    void(*visitor)(void*, AsyncQueueBlock&))
{
    if (!visitor) return;
    for (auto* block = first_; block;) {
        auto* next = block->pending_next_.load(std::memory_order_relaxed);
        visitor(data, *block);
        if (block == last_) break;
        block = next;
    }
}

ProducerReserve::ProducerReserve(std::size_t block_storage_size)
    : block_storage_size_(block_storage_size)
{
    if (!std::has_single_bit(block_storage_size_)) {
        throw std::invalid_argument(
            "Producer reserve block storage size must be a non-zero power of two");
    }
    static_assert(std::atomic<AsyncQueueBlock*>::is_always_lock_free);
    static_assert(std::atomic<std::size_t>::is_always_lock_free);
}

void ProducerReserve::return_block(AsyncQueueBlock& block) noexcept
{
    assert(block.owner_ == this);
    block.pending_next_.store(nullptr, std::memory_order_relaxed);
    block.released_next_ = nullptr;
    block.used_size_ = 0;

    auto* head = ready_head_.load(std::memory_order_relaxed);
    do {
        block.reserve_next_ = head;
    } while (!ready_head_.compare_exchange_weak(
        head,
        &block,
        std::memory_order_release,
        std::memory_order_relaxed));

    // This is reservable-block credit rather than a separately sampled queue
    // size. Publishing the block first ensures the count may understate ready
    // capacity transiently but never claims an unreachable block.
    ready_count_.fetch_add(1, std::memory_order_release);
}

void ProducerReserve::return_chain(AsyncQueueBlock* first) noexcept
{
    while (first) {
        auto* next = first->pending_next_.load(std::memory_order_relaxed);
        return_block(*first);
        first = next;
    }
}

AsyncQueueBlock* ProducerReserve::take_block() noexcept
{
    auto credit = ready_count_.load(std::memory_order_acquire);
    while (credit != 0
        && !ready_count_.compare_exchange_weak(
            credit,
            credit - 1,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
    }
    if (credit == 0) return nullptr;

    auto* head = ready_head_.load(std::memory_order_acquire);
    for (;;) {
        assert(head != nullptr);
        if (!head) return nullptr;
        if (ready_head_.compare_exchange_weak(
                head,
                head->reserve_next_,
                std::memory_order_acquire,
                std::memory_order_relaxed)) {
            head->reserve_next_ = nullptr;
            return head;
        }
    }
}

ProducedBlockChain ProducerReserve::acquire(
    std::size_t block_count) noexcept
{
    if (block_count == 0) return {};

    AsyncQueueBlock* first = nullptr;
    AsyncQueueBlock* last = nullptr;
    for (std::size_t index = 0; index < block_count; ++index) {
        auto* block = take_block();
        if (!block) {
            return_chain(first);
            return {};
        }
        block->pending_next_.store(nullptr, std::memory_order_relaxed);
        if (last) {
            last->pending_next_.store(block, std::memory_order_relaxed);
        } else {
            first = block;
        }
        last = block;
    }
    return {*this, *first, *last};
}

std::size_t ProducerReserve::ready_block_count() const noexcept
{
    return ready_count_.load(std::memory_order_acquire);
}

PinnedBlockPrefix::PinnedBlockPrefix(
    PendingQueue& queue,
    AsyncQueueBlock& first,
    AsyncQueueBlock& last) noexcept
    : queue_(&queue), first_(&first), last_(&last)
{}

PinnedBlockPrefix::PinnedBlockPrefix(PinnedBlockPrefix&& other) noexcept
    : queue_(std::exchange(other.queue_, nullptr))
    , first_(std::exchange(other.first_, nullptr))
    , last_(std::exchange(other.last_, nullptr))
{}

PinnedBlockPrefix& PinnedBlockPrefix::operator=(
    PinnedBlockPrefix&& other) noexcept
{
    if (this == &other) return *this;
    queue_ = std::exchange(other.queue_, nullptr);
    first_ = std::exchange(other.first_, nullptr);
    last_ = std::exchange(other.last_, nullptr);
    return *this;
}

void PinnedBlockPrefix::for_each(
    void* data,
    void(*visitor)(void*, AsyncQueueBlock const&)) const
{
    if (!visitor) return;
    for (auto const* block = first_; block;) {
        auto const* next = block->pending_next_.load(
            std::memory_order_acquire);
        visitor(data, *block);
        if (block == last_) break;
        block = next;
    }
}

void ReleasedBlockQueue::publish(
    AsyncQueueBlock& first, AsyncQueueBlock& last) noexcept
{
    auto* head = released_head_.load(std::memory_order_relaxed);
    do {
        last.released_next_ = head;
    } while (!released_head_.compare_exchange_weak(
        head,
        &first,
        std::memory_order_release,
        std::memory_order_relaxed));
}

AsyncQueueBlock* ReleasedBlockQueue::take_all() noexcept
{
    return released_head_.exchange(nullptr, std::memory_order_acquire);
}

PendingQueue::PendingQueue(ProducerReserve& owner)
    : owner_(&owner)
    , sentinel_(std::make_unique<AsyncQueueBlock>())
    , producer_tail_(sentinel_.get())
    , consumer_head_(sentinel_.get())
    , published_tail_(sentinel_.get())
{
    static_assert(std::atomic<AsyncQueueBlock*>::is_always_lock_free);
}

PendingQueue::~PendingQueue() = default;

bool PendingQueue::publish(ProducedBlockChain&& chain) noexcept
{
    if (chain.owner_ != owner_ || !chain.first_ || !chain.last_) return false;

    chain.last_->pending_next_.store(nullptr, std::memory_order_relaxed);
    producer_tail_->pending_next_.store(
        chain.first_, std::memory_order_release);
    producer_tail_ = chain.last_;
    published_tail_.store(chain.last_, std::memory_order_release);

    chain.owner_ = nullptr;
    chain.first_ = nullptr;
    chain.last_ = nullptr;
    return true;
}

PinnedBlockPrefix PendingQueue::pin() noexcept
{
    // Read the terminal publication first. Acquiring a new terminal also makes
    // the link installed before it visible. Reading an old terminal merely
    // leaves concurrent publication for the next background selection.
    auto* last = published_tail_.load(std::memory_order_acquire);
    if (last == consumer_head_) return {};
    auto* first = consumer_head_->pending_next_.load(
        std::memory_order_acquire);
    assert(first != nullptr);
    if (!first) return {};
    return {*this, *first, *last};
}

bool PendingQueue::release(
    PinnedBlockPrefix&& prefix,
    ReleasedBlockQueue& released) noexcept
{
    if (prefix.queue_ != this || !prefix.first_ || !prefix.last_) return false;
    auto* expected_first = consumer_head_->pending_next_.load(
        std::memory_order_acquire);
    if (expected_first != prefix.first_) return false;

    auto* cursor = prefix.first_;
    while (cursor != prefix.last_) {
        cursor = cursor->pending_next_.load(std::memory_order_acquire);
        if (!cursor) return false;
    }

    AsyncQueueBlock* released_first = nullptr;
    AsyncQueueBlock* released_last = nullptr;
    auto append_released = [&](AsyncQueueBlock& block) {
        block.pending_next_.store(nullptr, std::memory_order_relaxed);
        block.released_next_ = nullptr;
        if (released_last) {
            released_last->released_next_ = &block;
        } else {
            released_first = &block;
        }
        released_last = &block;
    };

    if (consumer_head_ != sentinel_.get()) append_released(*consumer_head_);
    cursor = prefix.first_;
    while (cursor != prefix.last_) {
        auto* next = cursor->pending_next_.load(std::memory_order_relaxed);
        append_released(*cursor);
        cursor = next;
    }

    consumer_head_ = prefix.last_;
    prefix = {};
    if (released_first) released.publish(*released_first, *released_last);
    return true;
}

struct AsyncCapacityManager::Slab {
    std::unique_ptr<AsyncQueueBlock[]> blocks{};
    std::unique_ptr<std::byte[]> storage{};
    std::size_t block_count = 0;
};

AsyncCapacityManager::AsyncCapacityManager(
    std::size_t slab_block_granularity)
    : slab_block_granularity_(slab_block_granularity)
{
    if (!std::has_single_bit(slab_block_granularity_)) {
        throw std::invalid_argument(
            "Async capacity slab granularity must be a non-zero power of two");
    }
}

AsyncCapacityManager::~AsyncCapacityManager() = default;

void AsyncCapacityManager::allocate_slab(
    ProducerReserve& reserve, std::size_t block_count)
{
    if (block_count == 0) return;
    if (block_count > std::numeric_limits<std::size_t>::max()
            / reserve.block_storage_size_) {
        throw std::length_error("Async capacity slab storage is too large");
    }

    auto slab = std::make_unique<Slab>();
    slab->blocks = std::make_unique<AsyncQueueBlock[]>(block_count);
    slab->storage = std::make_unique<std::byte[]>(
        block_count * reserve.block_storage_size_);
    slab->block_count = block_count;
    for (std::size_t index = 0; index < block_count; ++index) {
        auto& block = slab->blocks[index];
        block.owner_ = &reserve;
        block.storage_ = slab->storage.get()
            + index * reserve.block_storage_size_;
        block.storage_size_ = reserve.block_storage_size_;
    }

    auto* published = slab.get();
    slabs_.push_back(std::move(slab));
    for (std::size_t index = 0; index < published->block_count; ++index) {
        reserve.return_block(published->blocks[index]);
    }
}

std::size_t AsyncCapacityManager::maintain(
    ProducerReserve& reserve, ProducerCapacityPolicy policy)
{
    if (policy.maximum_burst == 0) {
        if (policy.low_watermark != 0 || policy.high_watermark != 0) {
            throw std::invalid_argument(
                "Empty producer capacity policy has non-empty watermarks");
        }
        return 0;
    }
    if (policy.low_watermark < policy.maximum_burst
        || policy.high_watermark <= policy.low_watermark) {
        throw std::invalid_argument(
            "Producer capacity policy has invalid C/L/H bounds");
    }

    auto const available = reserve.ready_block_count();
    if (available >= policy.low_watermark) return 0;
    auto const block_count = rounded_block_count(
        policy.high_watermark - available,
        slab_block_granularity_);
    allocate_slab(reserve, block_count);
    return block_count;
}

std::size_t AsyncCapacityManager::reclaim(
    ReleasedBlockQueue& released) noexcept
{
    auto* block = released.take_all();
    std::size_t reclaimed = 0;
    while (block) {
        auto* next = block->released_next_;
        auto* owner = block->owner_;
        assert(owner != nullptr);
        if (owner) owner->return_block(*block);
        block = next;
        ++reclaimed;
    }
    return reclaimed;
}

} // namespace iv
