#pragma once

#include <intravenous/runtime/persisted_page_store.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>

namespace iv {

using CaptureSequence = std::uint64_t;

enum class TickCapturePayloadKind : std::uint8_t {
    samples,
    events,
};

class TickCaptureStore;

// Control-path-interned identity. Generated/audio code treats this as an opaque
// token and never copies the string-bearing persisted identity behind it.
class TickCaptureOutputHandle {
    friend class TickCaptureStore;

    TickCaptureStore const* owner_ = nullptr;
    PersistedOutputId const* output_ = nullptr;

    TickCaptureOutputHandle(
        TickCaptureStore const& owner,
        PersistedOutputId const& output) noexcept
        : owner_(&owner), output_(&output)
    {}

public:
    TickCaptureOutputHandle() = default;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return owner_ != nullptr && output_ != nullptr;
    }

    [[nodiscard]] PersistedOutputId const& output() const noexcept;
};

static_assert(std::is_trivially_copyable_v<TickCaptureOutputHandle>);

struct TickCaptureRecordView {
    CaptureSequence sequence = 0;
    PersistedOutputId const* output = nullptr;
    SampleIndex begin = 0;
    std::size_t sample_count = 0;
    TickCapturePayloadKind payload_kind = TickCapturePayloadKind::samples;
    ChannelLayout sample_layout{};
    EventTypeId event_type = EventTypeId::empty;
    std::size_t event_count = 0;
    std::span<std::byte const> payload{};
};

// Executor-owned slab-backed transport between generated Tick production and a
// background transaction. Provisioning and identity registration are control-
// path operations. acquire()/Writer::seal_*() and CallbackScope destruction are
// the only audio-thread operations and perform no allocation, locking or owner
// destruction.
class TickCaptureStore {
    struct Block;
    class Impl;
    std::unique_ptr<Impl> impl_{};

    void abandon(Block& block) noexcept;
    [[nodiscard]] bool seal_samples(
        Block& block,
        TickCaptureOutputHandle output,
        SampleIndex begin,
        std::size_t sample_count,
        ChannelLayout layout) noexcept;
    [[nodiscard]] bool seal_events(
        Block& block,
        TickCaptureOutputHandle output,
        SampleIndex begin,
        std::size_t sample_count,
        EventTypeId type,
        std::size_t event_count) noexcept;
    void end_callback() noexcept;

public:
    class Writer {
        friend class TickCaptureStore;

        TickCaptureStore* store_ = nullptr;
        Block* block_ = nullptr;

        Writer(TickCaptureStore& store, Block& block) noexcept
            : store_(&store), block_(&block)
        {}
        void reset() noexcept;

    public:
        Writer() = default;
        ~Writer();
        Writer(Writer const&) = delete;
        Writer& operator=(Writer const&) = delete;
        Writer(Writer&& other) noexcept;
        Writer& operator=(Writer&& other) noexcept;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return block_ != nullptr;
        }

        [[nodiscard]] std::span<std::byte> payload() noexcept;

        [[nodiscard]] bool seal_samples(
            TickCaptureOutputHandle output,
            SampleIndex begin,
            std::size_t sample_count,
            ChannelLayout layout) noexcept;
        [[nodiscard]] bool seal_events(
            TickCaptureOutputHandle output,
            SampleIndex begin,
            std::size_t sample_count,
            EventTypeId type,
            std::size_t event_count) noexcept;
    };

    class CallbackScope {
        friend class TickCaptureStore;

        TickCaptureStore* store_ = nullptr;
        explicit CallbackScope(TickCaptureStore& store) noexcept
            : store_(&store)
        {}

    public:
        CallbackScope() = default;
        ~CallbackScope();
        CallbackScope(CallbackScope const&) = delete;
        CallbackScope& operator=(CallbackScope const&) = delete;
        CallbackScope(CallbackScope&& other) noexcept;
        CallbackScope& operator=(CallbackScope&& other) noexcept;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return store_ != nullptr;
        }
    };

    class Batch {
        friend class TickCaptureStore;

        TickCaptureStore* store_ = nullptr;
        Block* first_ = nullptr;
        CaptureSequence begin_ = 0;
        CaptureSequence cutoff_ = 0;

        Batch(
            TickCaptureStore& store,
            Block* first,
            CaptureSequence begin,
            CaptureSequence cutoff) noexcept
            : store_(&store)
            , first_(first)
            , begin_(begin)
            , cutoff_(cutoff)
        {}

    public:
        using VisitRecord = void(*)(void*, TickCaptureRecordView const&);

        Batch() = default;
        ~Batch() = default;
        Batch(Batch const&) = delete;
        Batch& operator=(Batch const&) = delete;
        Batch(Batch&& other) noexcept;
        Batch& operator=(Batch&& other) noexcept;

        [[nodiscard]] CaptureSequence begin() const noexcept { return begin_; }
        [[nodiscard]] CaptureSequence cutoff() const noexcept { return cutoff_; }
        [[nodiscard]] bool empty() const noexcept { return begin_ == cutoff_; }
        [[nodiscard]] std::size_t size() const noexcept;

        void for_each(void* data, VisitRecord visitor) const;

        template<class Fn>
        void for_each(Fn&& fn) const
        {
            using Function = std::remove_reference_t<Fn>;
            for_each(
                std::addressof(fn),
                +[](void* opaque, TickCaptureRecordView const& record) {
                    (*static_cast<Function*>(opaque))(record);
                });
        }
    };

    explicit TickCaptureStore(std::size_t block_payload_capacity);
    ~TickCaptureStore();

    TickCaptureStore(TickCaptureStore const&) = delete;
    TickCaptureStore& operator=(TickCaptureStore const&) = delete;
    TickCaptureStore(TickCaptureStore&&) = delete;
    TickCaptureStore& operator=(TickCaptureStore&&) = delete;

    [[nodiscard]] TickCaptureOutputHandle register_output(
        PersistedOutputId output);

    // Adds one append-only slab and publishes all of its blocks to the audio
    // thread only after their payload addresses and capacities are final.
    void provision(std::size_t block_count);

    [[nodiscard]] CallbackScope begin_callback() noexcept;
    [[nodiscard]] Writer acquire() noexcept;

    // Background/control path. snapshot_pending() fixes one immutable sequence
    // cutoff. Only commit() advances the processed frontier; destroying a batch
    // without commit leaves every record pending for the next transaction.
    [[nodiscard]] Batch snapshot_pending() noexcept;
    [[nodiscard]] bool commit(Batch&& batch) noexcept;

    // Returns committed blocks to the free pool on the non-audio path. Blocks
    // sealed at or after the active callback's starting sequence remain retired
    // until a later call. The queue's current consumer sentinel is deliberately
    // retained until a later record is committed.
    [[nodiscard]] std::size_t reclaim_committed() noexcept;

    [[nodiscard]] std::size_t block_payload_capacity() const noexcept;
    [[nodiscard]] std::size_t free_block_count() const noexcept;
    [[nodiscard]] std::size_t retired_block_count() const noexcept;
    [[nodiscard]] CaptureSequence processed_sequence() const noexcept;
    [[nodiscard]] CaptureSequence published_sequence() const noexcept;
};

static_assert(std::is_nothrow_destructible_v<TickCaptureStore::Writer>);
static_assert(std::is_nothrow_destructible_v<TickCaptureStore::CallbackScope>);

} // namespace iv
