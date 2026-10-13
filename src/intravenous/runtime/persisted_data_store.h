#pragma once

#include <intravenous/channel_layout.h>
#include <intravenous/coverage.h>
#include <intravenous/graph_jit/background_evaluation_plan.h>
#include <intravenous/graph_jit/stable_graph_identity.h>
#include <intravenous/ports.h>

#include <atomic>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace iv {

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

// One immutable, contiguous sample contribution. Values use layout and contain
// exactly range.extent frames. A stored record never crosses the signed-order
// seam; Candidate accepts circular input ranges and splits them before storage.
struct PersistedSampleRegion {
    PersistedOutputHandle output{};
    GlobalIndexRange range{};
    ChannelLayout layout{};
    std::vector<Sample> values{};

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
};

namespace persisted_data_store_detail {

struct SampleOutputState;
struct EventOutputState;
template<typename State>
struct OutputDirectoryNode;
using SampleOutputNode = OutputDirectoryNode<SampleOutputState>;
using EventOutputNode = OutputDirectoryNode<EventOutputState>;

} // namespace persisted_data_store_detail

enum class PersistedDataPublishResult : std::uint8_t {
    published,
    stale_base,
};

// Executor-level canonical retained data for persisted sample and event
// outputs. Candidate mutation and reclamation are serialized background/control
// work. ReaderSlot::pin() and ReaderPin destruction are the only realtime APIs.
class PersistedDataStore {
public:
    class Candidate;
    class ReaderSlot;

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

    public:
        using VisitSampleRegion = void (*)(
            void*, PersistedSampleRegion const&, GlobalIndexRange);
        using VisitEventChunk = void (*)(
            void*, PersistedEventChunk const&, GlobalIndexRange);

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

        void for_each_event_chunk(
            PersistedOutputHandle output,
            GlobalIndexRange range,
            void* visitor_data,
            VisitEventChunk visitor) const;

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

        [[nodiscard]] IndexCoverage const* find_sample_coverage(
            PersistedOutputHandle output,
            ChannelLayout layout) const noexcept;
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
        Snapshot const* base_ = nullptr;
        PersistedDataVersion base_version_{};
        std::unique_ptr<Snapshot> successor_{};

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

        // Overwrite replaces the retained value throughout record.range. A
        // circular range is split at the signed-order seam without exposing
        // that storage detail to the caller.
        void overwrite(PersistedSampleRegion record);
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
    std::uint32_t output_owner_token_ = 0;
    std::unique_ptr<Snapshot const> published_owner_{};
    std::atomic<Snapshot const*> published_{nullptr};
    std::vector<std::unique_ptr<Snapshot const>> retired_{};
    mutable std::mutex output_slots_mutex_{};
    std::vector<std::pair<PersistedOutputId, PersistedOutputHandle>>
        output_slots_{};
    mutable std::mutex reader_slots_mutex_{};
    std::vector<std::unique_ptr<ReaderSlotState>> reader_slots_{};

    [[nodiscard]] static std::uint32_t output_owner_token(
        PersistedOutputHandle output) noexcept;
    [[nodiscard]] static std::uint32_t output_directory_index(
        PersistedOutputHandle output) noexcept;
    [[nodiscard]] bool owns_output(
        PersistedOutputHandle output) const noexcept;
    void unregister_reader(ReaderSlotState& state) noexcept;

public:
    PersistedDataStore();
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
        return published_.load(std::memory_order_seq_cst) == &snapshot;
    }

    [[nodiscard]] ReaderSlot register_reader();
    [[nodiscard]] std::size_t reclaim_retired();
    [[nodiscard]] std::size_t retired_snapshot_count() const noexcept
    {
        return retired_.size();
    }
};

} // namespace iv
