#pragma once

#include <intravenous/channel_layout.h>
#include <intravenous/ports.h>

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
    void_value,
};

class TickCaptureStore;

// Read-only logical payload backed by one or more store blocks. Physical block
// boundaries are deliberately hidden from record ordering and identity.
class TickCapturePayloadView {
    friend class TickCaptureStore;

    void const* first_ = nullptr;
    std::size_t size_ = 0;

    TickCapturePayloadView(void const* first, std::size_t size) noexcept
        : first_(first), size_(size)
    {}

public:
    using VisitSegment = void(*)(void*, std::span<std::byte const>);

    TickCapturePayloadView() = default;

    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    void for_each_segment(void* data, VisitSegment visitor) const;

    template<class Fn>
    void for_each_segment(Fn&& fn) const
    {
        using Function = std::remove_reference_t<Fn>;
        for_each_segment(
            std::addressof(fn),
            +[](void* opaque, std::span<std::byte const> segment) {
                (*static_cast<Function*>(opaque))(segment);
            });
    }

    [[nodiscard]] bool copy_to(std::span<std::byte> destination) const noexcept;
};

// Store-local identity for one capture-backed output. Generated/audio code treats
// this as an opaque token; retention-specific identities live in registries above
// the shared transport.
class TickCaptureOutputHandle {
    friend class TickCaptureStore;

    TickCaptureStore const* owner_ = nullptr;
    std::uint64_t id_ = 0;
    PortKind kind_ = PortKind::sample;

    TickCaptureOutputHandle(
        TickCaptureStore const& owner,
        std::uint64_t id,
        PortKind kind) noexcept
        : owner_(&owner), id_(id), kind_(kind)
    {}

public:
    TickCaptureOutputHandle() = default;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return owner_ != nullptr && id_ != 0;
    }

    [[nodiscard]] PortKind kind() const noexcept { return kind_; }

    bool operator==(TickCaptureOutputHandle const&) const = default;
};

static_assert(std::is_trivially_copyable_v<TickCaptureOutputHandle>);

struct TickCaptureRecordView {
    CaptureSequence sequence = 0;
    TickCaptureOutputHandle output{};
    SampleIndex begin = 0;
    std::size_t sample_count = 0;
    TickCapturePayloadKind payload_kind = TickCapturePayloadKind::samples;
    ChannelLayout sample_layout{};
    EventTypeId event_type = EventTypeId::empty;
    std::size_t event_count = 0;
    TickCapturePayloadView payload{};
};

// Sticky transport failures observed while attempting to reserve one complete
// capture record. A true member means at least one such failure has occurred
// during this store's lifetime; later provisioning cannot repair the record
// that was not captured.
struct TickCaptureReservationFailures {
    bool attempted_outside_callback = false;
    bool insufficient_free_blocks = false;

    [[nodiscard]] bool any() const noexcept
    {
        return attempted_outside_callback || insufficient_free_blocks;
    }
};

// Non-audio capture allocator policy. maximum_blocks_per_callback (C) is a
// graph-derived structural bound. low_watermark (L), high_watermark (H), and
// slab_allocation_granularity (G) are operational allocator policy, with
// C <= L < H and G > 0.
struct TickCaptureReservePolicy {
    std::size_t maximum_blocks_per_callback = 0;
    std::size_t low_watermark = 0;
    std::size_t high_watermark = 0;
    std::size_t slab_allocation_granularity = 1;
};

// Executor-owned slab-backed transport between generated Tick production and a
// background transaction. Provisioning and identity registration are control-
// path operations. reserve_record()/RecordWriter::seal_*() and CallbackScope
// destruction are the only audio-thread operations and perform no allocation,
// locking or owner destruction. One logical record may own several physical
// blocks, but only its head participates in the published sequence.
class TickCaptureStore {
    friend class TickCapturePayloadView;

    struct Block;
    class Impl;
    std::unique_ptr<Impl> impl_{};

    void abandon_record(Block& head) noexcept;
    [[nodiscard]] bool publish_record(Block& head) noexcept;
    [[nodiscard]] TickCaptureRecordView view(Block const& head) const noexcept;
    [[nodiscard]] bool seal_samples(
        Block& head,
        TickCaptureOutputHandle output,
        SampleIndex begin,
        std::size_t sample_count,
        ChannelLayout layout) noexcept;
    [[nodiscard]] bool seal_events(
        Block& head,
        TickCaptureOutputHandle output,
        SampleIndex begin,
        std::size_t sample_count,
        EventTypeId type,
        std::size_t event_count) noexcept;
    [[nodiscard]] bool seal_void(
        Block& head,
        TickCaptureOutputHandle output,
        SampleIndex begin,
        std::size_t sample_count) noexcept;
    void end_callback() noexcept;

public:
    class RecordWriter {
        friend class TickCaptureStore;

        TickCaptureStore* store_ = nullptr;
        Block* head_ = nullptr;
        Block* write_block_ = nullptr;
        std::size_t write_block_offset_ = 0;
        std::size_t written_ = 0;

        RecordWriter(TickCaptureStore& store, Block& head) noexcept
            : store_(&store), head_(&head), write_block_(&head)
        {}
        void reset() noexcept;

    public:
        RecordWriter() = default;
        ~RecordWriter();
        RecordWriter(RecordWriter const&) = delete;
        RecordWriter& operator=(RecordWriter const&) = delete;
        RecordWriter(RecordWriter&& other) noexcept;
        RecordWriter& operator=(RecordWriter&& other) noexcept;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return head_ != nullptr;
        }

        [[nodiscard]] std::size_t payload_size() const noexcept;
        [[nodiscard]] std::size_t written_size() const noexcept
        {
            return written_;
        }
        [[nodiscard]] bool append(
            std::span<std::byte const> source) noexcept;

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
        // Seals an authoritative erasure of one sample/event output range. The
        // reserved record must have a zero-byte payload.
        [[nodiscard]] bool seal_void(
            TickCaptureOutputHandle output,
            SampleIndex begin,
            std::size_t sample_count) noexcept;
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

    [[nodiscard]] TickCaptureOutputHandle register_output(PortKind kind);

    // Allocator primitive: appends exactly one slab and publishes its blocks
    // only after all payload addresses and capacities are final.
    void allocate_free_block_slab(std::size_t block_count);

    // Allocator policy operation: does nothing at or above L; below L, appends
    // one G-rounded slab that restores free capacity to at least H. Returns the
    // number of capture blocks added.
    [[nodiscard]] std::size_t maintain_free_block_reserve(
        TickCaptureReservePolicy policy);

    [[nodiscard]] CallbackScope begin_callback() noexcept;
    // Reserves every physical block for one logical payload or returns an empty
    // writer without changing published state. A zero-byte record still owns a
    // head block so it can carry metadata and participate in sequence order.
    [[nodiscard]] RecordWriter reserve_record(
        std::size_t payload_size) noexcept;

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
    // Returns currently claimable free-block credits. Concurrent publication
    // may temporarily undercount reachable blocks, but the value never counts a
    // block before the audio thread can reserve it.
    [[nodiscard]] std::size_t free_block_count() const noexcept;
    [[nodiscard]] std::size_t retired_block_count() const noexcept;
    [[nodiscard]] CaptureSequence processed_sequence() const noexcept;
    [[nodiscard]] CaptureSequence published_sequence() const noexcept;
    [[nodiscard]] TickCaptureReservationFailures reservation_failures()
        const noexcept;
};

static_assert(std::is_nothrow_destructible_v<TickCaptureStore::RecordWriter>);
static_assert(std::is_nothrow_destructible_v<TickCaptureStore::CallbackScope>);

} // namespace iv
