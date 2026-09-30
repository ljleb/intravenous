#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>
#include <vector>

namespace iv {

class AsyncCapacityManager;
class PendingQueue;
class ProducerReserve;
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
    [[nodiscard]] PinnedBlockPrefix pin() noexcept;

    // Called only after the domain operation using prefix commits. The final
    // selected block remains the SPSC consumer sentinel until later work makes
    // it releasable.
    [[nodiscard]] bool release(
        PinnedBlockPrefix&& prefix,
        ReleasedBlockQueue& released) noexcept;
};

struct ProducerCapacityPolicy {
    std::size_t maximum_burst = 0;
    std::size_t low_watermark = 0;
    std::size_t high_watermark = 0;
};

// Non-realtime owner of allocated block slabs. This first primitive exposes
// one maintenance operation; the GraphExecutor-owned worker will call it when
// producer endpoints are integrated into execution generations.
class AsyncCapacityManager {
    struct Slab;

    std::size_t slab_block_granularity_ = 0;
    std::vector<std::unique_ptr<Slab>> slabs_{};

    void allocate_slab(ProducerReserve& reserve, std::size_t block_count);

public:
    explicit AsyncCapacityManager(std::size_t slab_block_granularity);
    ~AsyncCapacityManager();

    AsyncCapacityManager(AsyncCapacityManager const&) = delete;
    AsyncCapacityManager& operator=(AsyncCapacityManager const&) = delete;

    // Does nothing at or above L. Below L, provisions a whole-granularity slab
    // sufficient to restore this producer's ready reserve to at least H.
    [[nodiscard]] std::size_t maintain(
        ProducerReserve& reserve, ProducerCapacityPolicy policy);

    // Returns every committed block currently published by the background
    // worker to the block's owning producer reserve.
    [[nodiscard]] std::size_t reclaim(
        ReleasedBlockQueue& released) noexcept;
};

} // namespace iv
