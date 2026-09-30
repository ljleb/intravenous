#include <intravenous/runtime/async_capacity_manager.h>

#include <algorithm>
#include <bit>
#include <cassert>
#include <chrono>
#include <limits>
#include <new>
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

void validate_policy(ProducerCapacityPolicy policy)
{
    if (policy.maximum_burst == 0) {
        if (policy.low_watermark != 0 || policy.high_watermark != 0) {
            throw std::invalid_argument(
                "Empty producer capacity policy has non-empty watermarks");
        }
        return;
    }
    if (policy.low_watermark < policy.maximum_burst
        || policy.high_watermark <= policy.low_watermark) {
        throw std::invalid_argument(
            "Producer capacity policy has invalid C/L/H bounds");
    }
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

ProducerCapacityRegistration::ProducerCapacityRegistration(
    AsyncCapacityManager& manager,
    ProducerReserve& reserve,
    ProducerCapacityPolicy policy) noexcept
    : manager_(&manager), reserve_(&reserve), policy_(policy)
{}

ProducerCapacityRegistration::~ProducerCapacityRegistration()
{
    if (manager_) manager_->unregister(*this);
}

AsyncCapacityManager::AsyncCapacityManager(
    std::size_t slab_block_granularity)
    : slab_block_granularity_(slab_block_granularity)
{
    if (!std::has_single_bit(slab_block_granularity_)) {
        throw std::invalid_argument(
            "Async capacity slab granularity must be a non-zero power of two");
    }
    worker_ = std::jthread([this](std::stop_token stop) { run(stop); });
}

AsyncCapacityManager::~AsyncCapacityManager()
{
    worker_.request_stop();
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();

    std::scoped_lock lock(mutex_);
    for (auto* registration : registrations_) {
        registration->manager_ = nullptr;
    }
    registrations_.clear();
}

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

std::size_t AsyncCapacityManager::maintain_locked(
    ProducerReserve& reserve, ProducerCapacityPolicy policy)
{
    validate_policy(policy);
    if (policy.maximum_burst == 0) return 0;

    auto const available = reserve.ready_block_count();
    if (available >= policy.low_watermark) return 0;
    auto const block_count = rounded_block_count(
        policy.high_watermark - available,
        slab_block_granularity_);
    allocate_slab(reserve, block_count);
    return block_count;
}

std::size_t AsyncCapacityManager::maintain(
    ProducerReserve& reserve, ProducerCapacityPolicy policy)
{
    std::scoped_lock lock(mutex_);
    return maintain_locked(reserve, policy);
}

std::unique_ptr<ProducerCapacityRegistration>
AsyncCapacityManager::register_producer(
    ProducerReserve& reserve, ProducerCapacityPolicy policy)
{
    validate_policy(policy);
    auto registration = std::unique_ptr<ProducerCapacityRegistration>{
        new ProducerCapacityRegistration{*this, reserve, policy}};
    {
        std::scoped_lock lock(mutex_);
        auto const duplicate = std::find_if(
            registrations_.begin(), registrations_.end(),
            [&](auto const* existing) {
                return existing->reserve_ == &reserve;
            });
        if (duplicate != registrations_.end()) {
            throw std::invalid_argument(
                "Producer reserve is already registered");
        }
        static_cast<void>(maintain_locked(reserve, policy));
        registrations_.push_back(registration.get());
        ++registration_revision_;
    }
    wake_.notify_one();
    return registration;
}

void AsyncCapacityManager::unregister(
    ProducerCapacityRegistration& registration) noexcept
{
    {
        std::scoped_lock lock(mutex_);
        // Registration teardown occurs after its realtime/background endpoints
        // stop publishing. Drain blocks already returned by background while
        // holding the same mutex that excludes the manager worker.
        static_cast<void>(reclaim_locked(released_blocks_));
        auto const found = std::find(
            registrations_.begin(), registrations_.end(), &registration);
        if (found != registrations_.end()) registrations_.erase(found);
        registration.manager_ = nullptr;
        registration.reserve_ = nullptr;
        ++registration_revision_;
    }
    wake_.notify_one();
}

void AsyncCapacityManager::run(std::stop_token stop) noexcept
{
    static constexpr auto poll_interval = std::chrono::milliseconds{1};
    static constexpr auto allocation_retry_interval =
        std::chrono::milliseconds{100};

    std::uint64_t observed_revision = 0;
    bool allocation_backoff = false;
    for (;;) {
        {
            std::unique_lock lock(mutex_);
            if (registration_revision_ == observed_revision) {
                if (registrations_.empty()) {
                    wake_.wait(lock, [&] {
                        return stop.stop_requested()
                            || registration_revision_ != observed_revision;
                    });
                } else {
                    auto const interval = allocation_backoff
                        ? allocation_retry_interval
                        : poll_interval;
                    wake_.wait_for(lock, interval, [&] {
                        return stop.stop_requested()
                            || registration_revision_ != observed_revision;
                    });
                }
            }
            if (stop.stop_requested()) return;
            observed_revision = registration_revision_;

            if ((failure_bits_.load(std::memory_order_acquire)
                    & unexpected_failure) == 0) {
                try {
                    for (auto const* registration : registrations_) {
                        static_cast<void>(maintain_locked(
                            *registration->reserve_, registration->policy_));
                    }
                    allocation_backoff = false;
                } catch (std::bad_alloc const&) {
                    failure_bits_.fetch_or(
                        allocation_failure, std::memory_order_release);
                    allocation_backoff = true;
                } catch (...) {
                    failure_bits_.fetch_or(
                        unexpected_failure, std::memory_order_release);
                }
            }

            // Returning committed blocks to their producer reserves is
            // independent of whether provisioning new storage succeeded. It
            // stays under the registration mutex so unregister() cannot race
            // reclamation for a reserve whose lifetime is ending.
            static_cast<void>(reclaim_locked(released_blocks_));
        }
    }
}

std::size_t AsyncCapacityManager::reclaim_locked(
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

std::size_t AsyncCapacityManager::reclaim(
    ReleasedBlockQueue& released) noexcept
{
    std::scoped_lock lock(mutex_);
    return reclaim_locked(released);
}

AsyncCapacityManagerFailures AsyncCapacityManager::failures() const noexcept
{
    auto const bits = failure_bits_.load(std::memory_order_acquire);
    return {
        .allocation_failed = (bits & allocation_failure) != 0,
        .unexpected_failure = (bits & unexpected_failure) != 0,
    };
}

} // namespace iv
