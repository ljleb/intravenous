#pragma once

#include <intravenous/channel_layout.h>
#include <intravenous/coverage.h>
#include <intravenous/event_sequence.h>
#include <intravenous/graph_jit/background_evaluation_plan.h>
#include <intravenous/graph_jit/stable_graph_identity.h>
#include <intravenous/node/continuous_port.h>
#include <intravenous/ports.h>

#include <atomic>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <map>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace iv {
struct CompiledGraph;
class TickMaterializationSnapshot;

namespace persisted_data_store_detail {
struct BufferedSampleBacking;
class TickSamplePool;
}

struct GenerationLocalPersistedOutputId {
    std::uint64_t generation = 0;
    graph_jit::BackgroundPortIndex port = 0;
    PortKind kind = PortKind::sample;

    bool operator==(GenerationLocalPersistedOutputId const&) const = default;
};

using PersistedOutputId = std::variant<
    graph_jit::StableOutputPortId,
    GenerationLocalPersistedOutputId>;

[[nodiscard]] PortKind persisted_output_kind(
    PersistedOutputId const& output) noexcept;

// Compact executor-lifetime key resolved from a semantic output identity on a
// control/background path. The store token is never reused, so a handle
// retained by an old generation cannot alias an output in another store.
class PersistedOutputHandle {
    friend class PersistedDataStore;

    static constexpr std::uint64_t invalid =
        std::numeric_limits<std::uint64_t>::max();
    static constexpr unsigned slot_shift = 1;
    static constexpr unsigned owner_shift = 33;
    std::uint64_t value_ = invalid;

    explicit constexpr PersistedOutputHandle(
        std::uint32_t owner,
        std::uint32_t slot,
        PortKind kind) noexcept
        : value_((static_cast<std::uint64_t>(owner) << owner_shift)
            | (static_cast<std::uint64_t>(slot) << slot_shift)
            | static_cast<std::uint64_t>(kind == PortKind::event))
    {}

public:
    PersistedOutputHandle() = default;

    [[nodiscard]] constexpr bool valid() const noexcept
    {
        return value_ != invalid;
    }

    [[nodiscard]] constexpr PortKind kind() const noexcept
    {
        return (value_ & 1) == 0 ? PortKind::sample : PortKind::event;
    }

    constexpr auto operator<=>(PersistedOutputHandle const&) const = default;
};

static_assert(sizeof(PersistedOutputHandle) == sizeof(std::uint64_t));
static_assert(std::is_trivially_copyable_v<PersistedOutputHandle>);

[[nodiscard]] constexpr PortKind persisted_output_kind(
    PersistedOutputHandle output) noexcept
{
    return output.kind();
}

[[nodiscard]] PersistedOutputId persisted_output_id(
    graph_jit::BackgroundEvaluationPlan const& plan,
    graph_jit::PortStorageIndex storage,
    std::uint64_t generation);


struct PersistedDataVersion {
    std::uint64_t semantic = 0;
    std::uint64_t revision = 0;

    auto operator<=>(PersistedDataVersion const&) const = default;
};

class PersistedContinuousStorage {
    struct Delete {
        std::size_t alignment = alignof(std::max_align_t);

        void operator()(std::byte* storage) const noexcept
        {
            if (storage) {
                ::operator delete(storage, std::align_val_t{alignment});
            }
        }
    };

    std::unique_ptr<std::byte, Delete> storage_{nullptr, Delete{}};
    std::shared_ptr<void const> external_owner_{};
    std::byte* external_storage_ = nullptr;
    std::size_t size_ = 0;
    std::size_t alignment_ = alignof(std::max_align_t);

public:
    PersistedContinuousStorage() = default;
    PersistedContinuousStorage(std::size_t size, std::size_t alignment);
    PersistedContinuousStorage(PersistedContinuousStorage const&);
    PersistedContinuousStorage& operator=(PersistedContinuousStorage const&);
    PersistedContinuousStorage(PersistedContinuousStorage&&) noexcept;
    PersistedContinuousStorage& operator=(
        PersistedContinuousStorage&&) noexcept;

    [[nodiscard]] static PersistedContinuousStorage mapped(
        std::shared_ptr<void const> owner,
        std::byte const* storage,
        std::size_t size,
        std::size_t alignment);

    [[nodiscard]] std::byte* data() noexcept
    {
        return external_storage_ ? external_storage_ : storage_.get();
    }
    [[nodiscard]] std::byte const* data() const noexcept
    {
        return external_storage_ ? external_storage_ : storage_.get();
    }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t alignment() const noexcept { return alignment_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
};

// One immutable, contiguous sample contribution. Values use layout and contain
// exactly range.extent frames. A stored record never crosses the signed-order
// seam; Candidate accepts circular input ranges and splits them before storage.
struct PersistedSampleRegion {
    PersistedOutputHandle output{};
    GlobalIndexRange range{};
    ChannelLayout layout{};
    std::vector<Sample> values{};
    ContinuousValueStorageGeometry geometry =
        *continuous_value_storage_geometry(
            continuous_value_type_id_v<Sample>, std::nullopt);
    PersistedContinuousStorage continuous_values{};
    // Logical Region metadata survives partial preparation. Each immutable
    // payload window is independently bounded and shared by later snapshots.
    std::shared_ptr<std::vector<PersistedSampleRegion> const> windows{};
    std::shared_ptr<std::vector<ContinuousRegionStorageView> const> window_views{};
    // Physical pages are independent of this Region's selected readable view.
    // Only the page-directory subset for this payload is retained.
    std::shared_ptr<persisted_data_store_detail::BufferedSampleBacking const> buffered_backing{};

    void set_prepared_windows(std::vector<PersistedSampleRegion> prepared);
    [[nodiscard]] IndexCoverage materialized_coverage() const;
    [[nodiscard]] PersistedSampleRegion const* prepared_window(GlobalIndex) const noexcept;
    // Cold-only mapping of a completely materialized page-backed Region.
    // Logical coverage is never broadened and all resource limits still apply.
    [[nodiscard]] PersistedSampleRegion contiguous_view() const;
    [[nodiscard]] PersistedSampleRegion selected_view(GlobalIndexRange selected) const;

    [[nodiscard]] std::byte* storage_data() noexcept;
    [[nodiscard]] std::byte const* storage_data() const noexcept;
    [[nodiscard]] std::size_t storage_size_bytes() const noexcept;

    [[nodiscard]] Sample at(
        GlobalIndex index,
        std::size_t channel) const noexcept;
};

// One immutable event checkpoint. range is authoritative even when events is
// empty. Events carry absolute GlobalIndex coordinates and are ordered forward
// from range.begin. Stored chunks never cross the signed-order seam.
struct PersistedEventChunk {
    PersistedOutputHandle output{};
    GlobalIndexRange range{};
    EventTypeId type = EventTypeId::empty;
    std::vector<TimedEvent> events{};
    std::optional<IndexCoverage> prepared_coverage{};
    // Accounting is attached only by the serialized background writer. A
    // copied vector is charged separately from the immutable vector it copied.
    mutable std::shared_ptr<void const> storage_accounting{};
    mutable void const* accounted_storage = nullptr;
    mutable std::size_t accounted_bytes = 0;

    [[nodiscard]] IndexCoverage materialized_coverage() const {
        return prepared_coverage ? *prepared_coverage : IndexCoverage{range};
    }
};

template<typename Payload>
struct PersistedRegionWrite {
    // Index in the logical Region list supplied with this write.
    std::size_t region_index = 0;
    Payload payload;
};

namespace persisted_data_store_detail {

class BufferedPageAllocator;
struct SampleOutputState;
struct EventOutputState;
template<typename State>
struct OutputDirectoryNode;
using SampleOutputNode = OutputDirectoryNode<SampleOutputState>;
using EventOutputNode = OutputDirectoryNode<EventOutputState>;

} // namespace persisted_data_store_detail

// Executor resources, not authored port semantics. Defaults are provisional
// limits for the initial shared backend and remain configurable by its owner.
struct BufferedStorageLimits {
    std::size_t max_contiguous_mapping_bytes = std::size_t{1} << 30;
    std::size_t max_total_mapping_bytes = std::size_t{4} << 30;
    std::size_t max_backing_bytes = std::size_t{1} << 30;
    std::size_t max_mapping_runs = 16384;
    std::size_t process_vma_headroom = 512;
    std::size_t max_resident_bytes = std::size_t{64} << 20;
    // Optional retained results share the hard backing budget with recordings
    // and pinned readers. This additional bound cannot evict either owner.
    std::size_t max_cached_payload_bytes = std::size_t{256} << 20;
};

struct BufferedStorageUsage {
    std::size_t page_size = 0;
    // Physical sample backing; event_bytes is charged alongside it.
    std::size_t backing_bytes = 0;
    // Logical file extent includes punched, reusable slots.
    std::size_t file_extent_bytes = 0;
    std::size_t live_page_bytes = 0;
    std::size_t event_bytes = 0;
    // All free offsets, including slots without physical backing.
    std::size_t reusable_page_bytes = 0;
    std::size_t cached_page_bytes = 0;
    std::size_t reserved_mapping_bytes = 0;
    std::size_t mapped_runs = 0;
    std::size_t resident_bytes = 0;
    std::uint64_t reused_pages = 0;
    std::uint64_t mapping_calls = 0;
    std::uint64_t cache_evictions = 0;
};

struct BufferedResidentRange { std::byte const* data = nullptr; std::size_t bytes = 0; };

// Background-selected readable payload. Logical Regions and semantic output
// versions survive when this selection releases their materialized contents.
struct PersistedPayloadSelection {
    PersistedOutputHandle output{};
    IndexCoverage coverage{};
};

enum class PersistedDataPublishResult : std::uint8_t {
    published,
    stale_base,
};

// Executor-level canonical recording data and optional sample/event caches.
// Candidate mutation and reclamation are serialized background/control
// work. ReaderSlot::pin() and ReaderPin destruction are the only realtime APIs.
class PersistedDataStore {
    friend class TickMaterializationSnapshot;

public:
    class Candidate;
    class ReaderSlot;
    class Snapshot;
    struct Reconstruction {
        std::shared_ptr<CompiledGraph const> executable{};
        std::shared_ptr<Snapshot const> inputs{};
        std::vector<PersistedOutputHandle> dependencies{};
    };

    class Snapshot {
        friend class PersistedDataStore;
        friend class Candidate;

        PersistedDataVersion version_{};
        std::uint32_t output_owner_token_ = 0;
        std::shared_ptr<
            persisted_data_store_detail::SampleOutputNode const>
            sample_outputs_{};
        std::uint8_t sample_output_levels_ = 0;
        std::shared_ptr<
            persisted_data_store_detail::EventOutputNode const>
            event_outputs_{};
        std::uint8_t event_output_levels_ = 0;
        std::size_t sample_region_count_ = 0;
        std::size_t event_chunk_count_ = 0;
        // Residency changes preserve this identity and version_. A semantic
        // candidate receives a fresh identity even when it reuses payload.
        std::shared_ptr<void const> logical_identity_{};

    public:
        // Cold-only lease selection. Unselected payload is released, while
        // logical coverage, Region order and semantic identities are retained.
        // Recording preservation is used for reconstruction dependency roots;
        // playback uses false and owns only the selected readable windows.
        void retain_payload(std::span<PersistedPayloadSelection const> selected,
            bool preserve_recordings = false);
        // Preallocated empty initial playback roots use only the version. No
        // payload or directory ownership is acquired on the audio thread.
        void copy_identity_from(Snapshot const& source) noexcept;
        void select_outputs(std::span<PersistedOutputHandle const> outputs);
        [[nodiscard]] std::shared_ptr<Reconstruction const> reconstruction(
            PersistedOutputHandle output) const;
        using VisitSampleRegion = void (*)(
            void*, PersistedSampleRegion const&, GlobalIndexRange);
        using VisitEventChunk = void (*)(
            void*, PersistedEventChunk const&, GlobalIndexRange);
        using VisitEvent = void (*)(void*, TimedEvent const&);

        [[nodiscard]] PersistedDataVersion version() const noexcept
        {
            return version_;
        }

        [[nodiscard]] std::size_t sample_region_count() const noexcept
        {
            return sample_region_count_;
        }

        [[nodiscard]] std::size_t event_chunk_count() const noexcept
        {
            return event_chunk_count_;
        }

        [[nodiscard]] PersistedSampleRegion const* find_sample_region(
            PersistedOutputHandle output,
            GlobalIndex index) const noexcept;

        [[nodiscard]] PersistedEventChunk const* find_event_chunk(
            PersistedOutputHandle output,
            GlobalIndex index) const noexcept;

        void for_each_sample_region(
            PersistedOutputHandle output,
            GlobalIndexRange range,
            void* visitor_data,
            VisitSampleRegion visitor) const;

        // Authoritative logical Region order. Unlike the range-tree point
        // lookup, these spans preserve overlap and exact duplicates.
        [[nodiscard]] std::span<PersistedSampleRegion const> sample_regions(
            PersistedOutputHandle output,
            ChannelLayout layout) const noexcept;
        [[nodiscard]] std::span<ContinuousRegionStorageView const>
        sample_region_views(
            PersistedOutputHandle output,
            ChannelLayout layout,
            ContinuousValueStorageGeometry geometry) const noexcept;

        void for_each_event_chunk(
            PersistedOutputHandle output,
            GlobalIndexRange range,
            void* visitor_data,
            VisitEventChunk visitor) const;

        // Authoritative logical event Region order. Records may overlap or
        // have identical ranges; each record owns its event sequence.
        [[nodiscard]] std::span<PersistedEventChunk const> event_regions(
            PersistedOutputHandle output,
            EventTypeId type) const noexcept;
        // Canonical public Region ranges for either authored Tock output or
        // positional Tick recording. For positional recording, read a range
        // with for_each_event_region(); event_regions() retains the internal
        // chunk records and is not the canonical Region list.
        [[nodiscard]] std::span<GlobalIndexRange const> event_region_ranges(
            PersistedOutputHandle output,
            EventTypeId type) const noexcept;
        // Timeline-ordered concatenation of all logical Regions. Events from
        // overlapping or duplicate Regions remain duplicated. Equal-time
        // events retain Region order.
        [[nodiscard]] std::span<TimedEvent const> event_timeline(
            PersistedOutputHandle output,
            EventTypeId type) const noexcept;

        // Visits one public event Region. Authored Tock Regions retain their
        // independent sequence identity. A positional Tick recording Region
        // may span several retained chunks and is visited as their merged
        // canonical IndexCoverage range.
        [[nodiscard]] EventNeighbors event_neighbors(
            PersistedOutputHandle, EventTypeId, std::size_t region, GlobalIndex,
            EventNeighborMode mode = EventNeighborMode::bounded) const;

        void for_each_event_region(
            PersistedOutputHandle output,
            EventTypeId type,
            std::size_t region_index,
            GlobalIndexRange requested,
            void* visitor_data,
            VisitEvent visitor) const;

        template<typename Fn>
        void for_each_sample_region(
            PersistedOutputHandle output,
            GlobalIndexRange range,
            Fn&& fn) const
        {
            using Visitor = std::remove_reference_t<Fn>;
            for_each_sample_region(
                output,
                range,
                std::addressof(fn),
                +[](void* opaque,
                    PersistedSampleRegion const& region,
                    GlobalIndexRange intersection) {
                    (*static_cast<Visitor*>(opaque))(region, intersection);
                });
        }

        template<typename Fn>
        void for_each_event_chunk(
            PersistedOutputHandle output,
            GlobalIndexRange range,
            Fn&& fn) const
        {
            using Visitor = std::remove_reference_t<Fn>;
            for_each_event_chunk(
                output,
                range,
                std::addressof(fn),
                +[](void* opaque,
                    PersistedEventChunk const& chunk,
                    GlobalIndexRange intersection) {
                    (*static_cast<Visitor*>(opaque))(chunk, intersection);
                });
        }

        template<typename Fn>
        void for_each_event_region(
            PersistedOutputHandle output,
            EventTypeId type,
            std::size_t region_index,
            GlobalIndexRange requested,
            Fn&& fn) const
        {
            using Visitor = std::remove_reference_t<Fn>;
            for_each_event_region(
                output,
                type,
                region_index,
                requested,
                std::addressof(fn),
                +[](void* opaque, TimedEvent const& event) {
                    (*static_cast<Visitor*>(opaque))(event);
                });
        }

        // Equality of immutable semantic output identity, not payload ownership
        // or a comparison of bytes.
        // False means dependencies must be invalidated even if values happen
        // to compare equal. Payload eviction preserves the semantic owner.
        // These operations run only on cold/background paths.
        [[nodiscard]] bool shares_output_with(Snapshot const& other,
            PersistedOutputHandle output) const noexcept;

        [[nodiscard]] IndexCoverage const* find_event_logical_coverage(
            PersistedOutputHandle output) const noexcept;

        [[nodiscard]] std::shared_ptr<void const> sample_output_owner(
            PersistedOutputHandle output) const;

        [[nodiscard]] IndexCoverage const* find_sample_logical_coverage(
            PersistedOutputHandle output) const noexcept;

        [[nodiscard]] IndexCoverage const* find_sample_coverage(
            PersistedOutputHandle output,
            ChannelLayout layout) const noexcept;
        [[nodiscard]] IndexCoverage const* find_sample_coverage(
            PersistedOutputHandle output,
            ChannelLayout layout,
            ContinuousValueStorageGeometry geometry) const noexcept;
        [[nodiscard]] IndexCoverage const* find_event_coverage(
            PersistedOutputHandle output,
            EventTypeId type) const noexcept;
    };

private:
    struct ReaderSlotState {
        std::atomic<bool> acquiring{false};
        std::atomic<Snapshot const*> pinned{nullptr};
    };

    static_assert(std::atomic<bool>::is_always_lock_free);
    static_assert(std::atomic<Snapshot const*>::is_always_lock_free);

public:
    class Candidate {
        friend class PersistedDataStore;

        PersistedDataStore* store_ = nullptr;
        PersistedDataVersion base_version_{};
        std::unique_ptr<Snapshot> successor_{};
        std::shared_ptr<void const> base_identity_{};
        std::vector<PersistedOutputHandle> erased_outputs_{};

        Candidate(
            PersistedDataStore& store,
            Snapshot const& base,
            std::unique_ptr<Snapshot> successor) noexcept;

    public:
        Candidate() = default;
        ~Candidate() = default;
        Candidate(Candidate const&) = delete;
        Candidate& operator=(Candidate const&) = delete;
        Candidate(Candidate&&) noexcept = default;
        Candidate& operator=(Candidate&&) noexcept = default;

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return successor_ != nullptr;
        }

        [[nodiscard]] PersistedDataVersion base_version() const noexcept
        {
            return base_version_;
        }

        [[nodiscard]] PersistedDataVersion target_version() const noexcept;
        [[nodiscard]] Snapshot const& working_snapshot() const;
        void retain_payload(std::span<PersistedPayloadSelection const> selected);
        // Attach the exact immutable producer/input selection after staging.
        // Equal reconstruction inputs preserve the logical output identity.
        void set_reconstruction(PersistedOutputHandle output,
            std::shared_ptr<Reconstruction const> context, Snapshot const& previous);

        // Reuse one output's immutable Region/chunk/page roots from a prior
        // selection belonging to the same store. Directory paths are copied;
        // sample/event payloads and logical Region identity stay shared.
        void reuse_output(Snapshot const& source, PersistedOutputHandle output);

        // Overwrite commits one positional Tick recording payload. Continuous
        // recording payloads are interleaved. A circular range is split at
        // the signed-order seam without exposing that storage detail to the
        // caller.
        void overwrite(PersistedSampleRegion record);
        // Invalidation applies to every contribution at the given positions.
        // Each write replaces content in its specified Region only.
        void replace_sample_regions(
            PersistedOutputHandle output,
            ChannelLayout layout,
            ContinuousValueStorageGeometry geometry,
            IndexCoverage const& invalidated,
            std::span<GlobalIndexRange const> logical_order,
            std::vector<PersistedRegionWrite<PersistedSampleRegion>> writes);
        void replace_event_regions(
            PersistedOutputHandle output,
            EventTypeId type,
            IndexCoverage const& invalidated,
            std::span<GlobalIndexRange const> logical_order,
            std::vector<PersistedRegionWrite<PersistedEventChunk>> writes);
        void overwrite(PersistedEventChunk record);

        void erase_sample(
            PersistedOutputHandle output,
            GlobalIndexRange range,
            ChannelLayout layout);
        void erase_events(
            PersistedOutputHandle output,
            GlobalIndexRange range,
            EventTypeId type);
        void erase_output(PersistedOutputHandle output);
    };

    class ReaderPin {
        friend class ReaderSlot;

        ReaderSlotState* slot_ = nullptr;
        Snapshot const* snapshot_ = nullptr;
        std::unique_ptr<Snapshot> owned_snapshot_{};

        explicit ReaderPin(
            ReaderSlotState& slot,
            std::atomic<Snapshot const*> const& published) noexcept;
        void reset() noexcept;

    public:
        ReaderPin() = default;
        ~ReaderPin();
        ReaderPin(ReaderPin const&) = delete;
        ReaderPin& operator=(ReaderPin const&) = delete;
        ReaderPin(ReaderPin&& other) noexcept;
        ReaderPin& operator=(ReaderPin&& other) noexcept;

        [[nodiscard]] Snapshot const& snapshot() const noexcept;
        // Cold-only detachment from the canonical cache root. Later selection
        // mutates this private lease, never a published reader's Snapshot.
        void retain_payload(std::span<PersistedPayloadSelection const> selected,
            bool preserve_recordings = false);
        void detach();
        void select_outputs(std::span<PersistedOutputHandle const> outputs);
        [[nodiscard]] Snapshot const* operator->() const noexcept
        {
            return snapshot_;
        }
    };

    class ReaderSlot {
        friend class PersistedDataStore;

        PersistedDataStore* store_ = nullptr;
        ReaderSlotState* state_ = nullptr;

        ReaderSlot(
            PersistedDataStore& store,
            ReaderSlotState& state) noexcept;
        void reset() noexcept;

    public:
        ReaderSlot() = default;
        ~ReaderSlot();
        ReaderSlot(ReaderSlot const&) = delete;
        ReaderSlot& operator=(ReaderSlot const&) = delete;
        ReaderSlot(ReaderSlot&& other) noexcept;
        ReaderSlot& operator=(ReaderSlot&& other) noexcept;

        [[nodiscard]] ReaderPin pin() noexcept;
    };

private:
    std::shared_ptr<persisted_data_store_detail::BufferedPageAllocator> buffered_pages_{};
    std::mutex tick_sample_pool_mutex_{};
    std::shared_ptr<persisted_data_store_detail::TickSamplePool> tick_sample_pool_{};
    std::atomic<bool> tick_sample_pool_disabled_{false};
    std::uint32_t output_owner_token_ = 0;
    std::unique_ptr<Snapshot const> published_owner_{};
    std::atomic<Snapshot const*> published_{nullptr};
    std::atomic<std::uint64_t> published_revision_{0};
    // Private preparations also need unique identities before publication.
    std::atomic<std::uint64_t> last_candidate_revision_{0};
    std::vector<std::unique_ptr<Snapshot const>> retired_{};
    mutable std::mutex output_slots_mutex_{};
    std::vector<std::pair<PersistedOutputId, PersistedOutputHandle>>
        output_slots_{};
    mutable std::mutex reader_slots_mutex_{};
    std::vector<std::unique_ptr<ReaderSlotState>> reader_slots_{};
    struct CachePolicy {
        OutputRealization realization = OutputRealization::buffered;
        std::uint64_t last_use = 0;
        std::uint64_t uses = 0;
    };
    std::map<PersistedOutputHandle, CachePolicy> cache_policy_{};
    std::uint64_t cache_clock_ = 0;
    std::atomic<std::uint64_t> cache_evictions_{0};
    bool pending_computed_reclamation_ = false;

    [[nodiscard]] static std::uint32_t output_owner_token(
        PersistedOutputHandle output) noexcept;
    [[nodiscard]] static std::uint32_t output_directory_index(
        PersistedOutputHandle output) noexcept;
    [[nodiscard]] bool owns_output(
        PersistedOutputHandle output) const noexcept;
    void unregister_reader(ReaderSlotState& state) noexcept;

public:
    explicit PersistedDataStore(BufferedStorageLimits limits = {});
    ~PersistedDataStore();

    PersistedDataStore(PersistedDataStore const&) = delete;
    PersistedDataStore& operator=(PersistedDataStore const&) = delete;
    PersistedDataStore(PersistedDataStore&&) = delete;
    PersistedDataStore& operator=(PersistedDataStore&&) = delete;

    [[nodiscard]] Candidate begin_candidate(
        std::uint64_t target_semantic_version);

    [[nodiscard]] PersistedOutputHandle resolve_output(
        PersistedOutputId const& output);

    [[nodiscard]] PersistedDataPublishResult publish(Candidate&& candidate);

    [[nodiscard]] bool is_current(Snapshot const& snapshot) const noexcept
    {
        return snapshot.output_owner_token_ == output_owner_token_
            && published_revision_.load(std::memory_order_acquire) == snapshot.version_.revision;
    }

    // Serialized background operation. Releases only reconstructible cached
    // payload outside protected selections; authoritative recordings survive.
    // Existing leases remain immutable. This is not a semantic publication.
    void reclaim_cached_payload(std::span<PersistedPayloadSelection const> protected_payload);
    void set_cache_realization(PersistedOutputHandle output, OutputRealization realization);
    // Preflight before producer allocation, on the serialized background actor.
    // release_leases removes optional ownership from the transaction's private
    // roots before cache roots retire. Required payload is always preserved.
    void prepare_cache(std::size_t incoming_bytes,
        std::span<PersistedPayloadSelection const> required,
        std::function<void(std::span<PersistedPayloadSelection const>)> const& release_leases);

    [[nodiscard]] ReaderSlot register_reader();
    [[nodiscard]] std::size_t reclaim_retired();
    [[nodiscard]] BufferedStorageUsage buffered_storage_usage() const;
    [[nodiscard]] std::size_t max_pooled_tick_sample_bytes() const noexcept;
    // Cold preparation. The lease must be destroyed before the payload owners;
    // it keeps overlapping selections locked until their last lease retires.
    [[nodiscard]] std::shared_ptr<void const> prepare_resident(std::span<BufferedResidentRange const> ranges);
    [[nodiscard]] std::size_t retired_snapshot_count() const noexcept
    {
        return retired_.size();
    }
};

} // namespace iv
