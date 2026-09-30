#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <type_traits>
#include <vector>

namespace iv {

class AsyncCapacityManager;
class PendingQueue;
class ProducedBlockWriter;
class ProducerReserve;
class ProducerCapacityRegistration;
class ReleasedBlockQueue;

// One stable, fixed-capacity unit of realtime-to-background transport. Queue
// clients own the bytes and used-size while the block is producer-private or
// selected background work; the linkage and owning reserve remain transport
// metadata.
class AsyncQueueBlock {
    friend class AsyncCapacityManager;
    friend class PendingQueue;
    friend class PinnedBlockPrefix;
    friend class ProducedBlockChain;
    friend class ProducedBlockWriter;
    friend class ProducerReserve;
    friend class ReleasedBlockQueue;

    ProducerReserve* owner_ = nullptr;
    AsyncQueueBlock* reserve_next_ = nullptr;
    std::atomic<AsyncQueueBlock*> pending_next_{nullptr};
    AsyncQueueBlock* released_next_ = nullptr;
    std::byte* storage_ = nullptr;
    std::size_t storage_size_ = 0;
    std::size_t used_size_ = 0;

public:
    AsyncQueueBlock() = default;
    AsyncQueueBlock(AsyncQueueBlock const&) = delete;
    AsyncQueueBlock& operator=(AsyncQueueBlock const&) = delete;

    [[nodiscard]] std::span<std::byte> storage() noexcept
    {
        return {storage_, storage_size_};
    }

    [[nodiscard]] std::span<std::byte const> storage() const noexcept
    {
        return {storage_, storage_size_};
    }

    [[nodiscard]] std::span<std::byte> used_storage() noexcept
    {
        return {storage_, used_size_};
    }

    [[nodiscard]] std::span<std::byte const> used_storage() const noexcept
    {
        return {storage_, used_size_};
    }

    [[nodiscard]] std::size_t used_size() const noexcept
    {
        return used_size_;
    }

    [[nodiscard]] bool set_used_size(std::size_t size) noexcept
    {
        if (size > storage_size_) return false;
        used_size_ = size;
        return true;
    }
};

// Realtime-owned chain assembled entirely before publication. Destroying an
// unpublished chain returns its blocks to the originating producer reserve and
// performs no allocation, locking, or owner destruction.
class ProducedBlockChain {
    friend class PendingQueue;
    friend class ProducedBlockWriter;
    friend class ProducerReserve;

    ProducerReserve* owner_ = nullptr;
    AsyncQueueBlock* first_ = nullptr;
    AsyncQueueBlock* last_ = nullptr;

    ProducedBlockChain(
        ProducerReserve& owner,
        AsyncQueueBlock& first,
        AsyncQueueBlock& last) noexcept;
    void reset() noexcept;

public:
    ProducedBlockChain() = default;
    ~ProducedBlockChain();
    ProducedBlockChain(ProducedBlockChain const&) = delete;
    ProducedBlockChain& operator=(ProducedBlockChain const&) = delete;
    ProducedBlockChain(ProducedBlockChain&& other) noexcept;
    ProducedBlockChain& operator=(ProducedBlockChain&& other) noexcept;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return first_ != nullptr;
    }

    // Joins another unpublished chain from the same producer reserve without
    // publishing either chain or allocating linkage storage.
    [[nodiscard]] bool append(ProducedBlockChain&& other) noexcept;

    void for_each(
        void* data,
        void(*visitor)(void*, AsyncQueueBlock&));

    template<class Fn>
    void for_each(Fn&& fn)
    {
        using Function = std::remove_reference_t<Fn>;
        for_each(
            std::addressof(fn),
            +[](void* opaque, AsyncQueueBlock& block) {
                (*static_cast<Function*>(opaque))(block);
            });
    }

    // Creates one monotonic writer over the private chain. The writer never
    // rescans earlier blocks as bytes are appended.
    [[nodiscard]] ProducedBlockWriter writer() noexcept;
};

// Realtime-only cursor over one unpublished ProducedBlockChain. append() is
// all-or-nothing for each supplied byte span and advances monotonically across
// the chain's fixed-capacity blocks.
class ProducedBlockWriter {
    AsyncQueueBlock* current_ = nullptr;
    AsyncQueueBlock* last_ = nullptr;
    std::size_t block_offset_ = 0;
    std::size_t remaining_ = 0;
    std::size_t written_ = 0;

    explicit ProducedBlockWriter(ProducedBlockChain& chain) noexcept;
    friend class ProducedBlockChain;

public:
    ProducedBlockWriter() = default;

    [[nodiscard]] bool append(std::span<std::byte const> bytes) noexcept;
    [[nodiscard]] std::size_t remaining_capacity() const noexcept
    {
        return remaining_;
    }
    [[nodiscard]] std::size_t bytes_written() const noexcept
    {
        return written_;
    }
};

// Blocks already assigned to one realtime producer. acquire() either obtains
// the complete requested chain or leaves the reserve with no net change.
class ProducerReserve {
    friend class AsyncCapacityManager;
    friend class ProducedBlockChain;

    std::size_t block_storage_size_ = 0;
    std::atomic<AsyncQueueBlock*> ready_head_{nullptr};
    std::atomic<std::size_t> ready_count_{0};

    void return_block(AsyncQueueBlock& block) noexcept;
    void return_chain(AsyncQueueBlock* first) noexcept;
    [[nodiscard]] AsyncQueueBlock* take_block() noexcept;

public:
    explicit ProducerReserve(std::size_t block_storage_size);

    ProducerReserve(ProducerReserve const&) = delete;
    ProducerReserve& operator=(ProducerReserve const&) = delete;
    ProducerReserve(ProducerReserve&&) = delete;
    ProducerReserve& operator=(ProducerReserve&&) = delete;

    [[nodiscard]] ProducedBlockChain acquire(
        std::size_t block_count) noexcept;
    [[nodiscard]] std::size_t ready_block_count() const noexcept;
    [[nodiscard]] std::size_t block_storage_size() const noexcept
    {
        return block_storage_size_;
    }
};

// One finite background selection from one producer queue. Appending later
// work cannot change first/last, so iteration deliberately stops at last.
class PinnedBlockPrefix {
    friend class PendingQueue;

    PendingQueue* queue_ = nullptr;
    AsyncQueueBlock* first_ = nullptr;
    AsyncQueueBlock* last_ = nullptr;

    PinnedBlockPrefix(
        PendingQueue& queue,
        AsyncQueueBlock& first,
        AsyncQueueBlock& last) noexcept;

public:
    PinnedBlockPrefix() = default;
    PinnedBlockPrefix(PinnedBlockPrefix const&) = delete;
    PinnedBlockPrefix& operator=(PinnedBlockPrefix const&) = delete;
    PinnedBlockPrefix(PinnedBlockPrefix&& other) noexcept;
    PinnedBlockPrefix& operator=(PinnedBlockPrefix&& other) noexcept;

    [[nodiscard]] bool empty() const noexcept { return first_ == nullptr; }

    void for_each(
        void* data,
        void(*visitor)(void*, AsyncQueueBlock const&)) const;

    template<class Fn>
    void for_each(Fn&& fn) const
    {
        using Function = std::remove_reference_t<Fn>;
        for_each(
            std::addressof(fn),
            +[](void* opaque, AsyncQueueBlock const& block) {
                (*static_cast<Function*>(opaque))(block);
            });
    }
};

// The single background-producer/capacity-manager-consumer return path. Block
// order is immaterial after domain commit, so publication transfers whole
// released chains without retaining a queue sentinel block.
class ReleasedBlockQueue {
    friend class AsyncCapacityManager;
    friend class PendingQueue;

    std::atomic<AsyncQueueBlock*> released_head_{nullptr};

    void publish(
        AsyncQueueBlock& first, AsyncQueueBlock& last) noexcept;
    [[nodiscard]] AsyncQueueBlock* take_all() noexcept;

public:
    ReleasedBlockQueue() = default;
    ReleasedBlockQueue(ReleasedBlockQueue const&) = delete;
    ReleasedBlockQueue& operator=(ReleasedBlockQueue const&) = delete;
};

// Producer-specific pending work. One realtime producer publishes complete
// chains; one background consumer independently pins and releases prefixes.
class PendingQueue {
    ProducerReserve* owner_ = nullptr;
    std::unique_ptr<AsyncQueueBlock> sentinel_{};
    AsyncQueueBlock* producer_tail_ = nullptr;
    AsyncQueueBlock* consumer_head_ = nullptr;
    std::atomic<AsyncQueueBlock*> published_tail_{nullptr};
    std::atomic<bool> closed_{false};
    bool closed_sentinel_released_ = false;

public:
    explicit PendingQueue(ProducerReserve& owner);
    ~PendingQueue();

    PendingQueue(PendingQueue const&) = delete;
    PendingQueue& operator=(PendingQueue const&) = delete;
    PendingQueue(PendingQueue&&) = delete;
    PendingQueue& operator=(PendingQueue&&) = delete;

    // One release publication makes every initialized block and private link
    // in chain visible to the background consumer.
    [[nodiscard]] bool publish(ProducedBlockChain&& chain) noexcept;

    // Realtime-generation-cutover operation. The single producer calls close()
    // only after publishing its final chain. The release/acquire pair makes
    // closure proof that the queue's published tail is final.
    void close() noexcept;
    [[nodiscard]] bool is_closed() const noexcept;

    [[nodiscard]] PinnedBlockPrefix pin() noexcept;

    // Called only after the domain operation using prefix commits. The final
    // selected block remains the SPSC consumer sentinel until later work makes
    // it releasable.
    [[nodiscard]] bool release(
        PinnedBlockPrefix&& prefix,
        ReleasedBlockQueue& released) noexcept;

    // Background-only retirement operations. A queue is closed and drained
    // only when no published block remains beyond its consumer sentinel.
    // release_closed_sentinel() transfers that final retained block to the
    // capacity manager; an initially empty queue has no block to transfer.
    [[nodiscard]] bool is_closed_and_drained() const noexcept;
    [[nodiscard]] bool release_closed_sentinel(
        ReleasedBlockQueue& released) noexcept;
};

struct ProducerCapacityPolicy {
    std::size_t maximum_burst = 0;
    std::size_t low_watermark = 0;
    std::size_t high_watermark = 0;
};

struct AsyncCapacityManagerFailures {
    bool allocation_failed = false;
    bool unexpected_failure = false;

    [[nodiscard]] bool any() const noexcept
    {
        return allocation_failed || unexpected_failure;
    }
};

// Stable off-realtime registration of one producer reserve and its immutable
// C/L/H policy. Destroying the registration first removes the reserve from the
// manager worker, after which the reserve may be destroyed safely.
class ProducerCapacityRegistration {
    friend class AsyncCapacityManager;

    AsyncCapacityManager* manager_ = nullptr;
    ProducerReserve* reserve_ = nullptr;
    ProducerCapacityPolicy policy_{};

    ProducerCapacityRegistration(
        AsyncCapacityManager& manager,
        ProducerReserve& reserve,
        ProducerCapacityPolicy policy) noexcept;

public:
    ~ProducerCapacityRegistration();

    ProducerCapacityRegistration(ProducerCapacityRegistration const&) = delete;
    ProducerCapacityRegistration& operator=(
        ProducerCapacityRegistration const&) = delete;
    ProducerCapacityRegistration(ProducerCapacityRegistration&&) = delete;
    ProducerCapacityRegistration& operator=(
        ProducerCapacityRegistration&&) = delete;
};

// Non-realtime owner of allocated block slabs and the worker that replenishes
// registered producer reserves and reclaims the shared released-block stream.
class AsyncCapacityManager {
    friend class ProducerCapacityRegistration;

    struct Slab;

    static constexpr std::uint32_t allocation_failure = 1u << 0;
    static constexpr std::uint32_t unexpected_failure = 1u << 1;

    std::size_t slab_block_granularity_ = 0;
    std::vector<std::unique_ptr<Slab>> slabs_{};
    ReleasedBlockQueue released_blocks_{};
    std::mutex mutex_{};
    std::condition_variable wake_{};
    std::vector<ProducerCapacityRegistration*> registrations_{};
    std::uint64_t registration_revision_ = 0;
    std::atomic<std::uint32_t> failure_bits_{0};
    // Declared last so destruction can stop and join the worker before any
    // state used by its run loop is destroyed.
    std::jthread worker_{};

    void allocate_slab(ProducerReserve& reserve, std::size_t block_count);
    [[nodiscard]] std::size_t maintain_locked(
        ProducerReserve& reserve, ProducerCapacityPolicy policy);
    [[nodiscard]] std::size_t reclaim_locked(
        ReleasedBlockQueue& released) noexcept;
    void unregister(ProducerCapacityRegistration& registration) noexcept;
    void run(std::stop_token stop) noexcept;

public:
    explicit AsyncCapacityManager(std::size_t slab_block_granularity);
    ~AsyncCapacityManager();

    AsyncCapacityManager(AsyncCapacityManager const&) = delete;
    AsyncCapacityManager& operator=(AsyncCapacityManager const&) = delete;

    // Synchronously provisions the initial reserve before publishing the
    // registration to the worker. The returned stable object must not outlive
    // this manager.
    [[nodiscard]] std::unique_ptr<ProducerCapacityRegistration>
    register_producer(
        ProducerReserve& reserve, ProducerCapacityPolicy policy);

    [[nodiscard]] ReleasedBlockQueue& released_blocks() noexcept
    {
        return released_blocks_;
    }

    // Does nothing at or above L. Below L, provisions a whole-granularity slab
    // sufficient to restore this producer's ready reserve to at least H. This
    // synchronous form is a deterministic control/test seam; registered
    // reserves are maintained automatically by the worker.
    [[nodiscard]] std::size_t maintain(
        ProducerReserve& reserve, ProducerCapacityPolicy policy);

    // Returns every committed block currently published by the background
    // worker to the block's owning producer reserve.
    [[nodiscard]] std::size_t reclaim(
        ReleasedBlockQueue& released) noexcept;

    [[nodiscard]] AsyncCapacityManagerFailures failures() const noexcept;
};

} // namespace iv
