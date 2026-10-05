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
#include <memory>
#include <mutex>
#include <span>
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

// Resolve the canonical store identity already selected by one persisted
// storage slot. Callers do this while constructing control-path workspaces,
// never from the audio callback.
[[nodiscard]] PersistedOutputId persisted_output_id(
    graph_jit::BackgroundEvaluationPlan const& plan,
    graph_jit::PortStorageIndex storage,
    std::uint64_t generation);

struct PersistedPageSnapshotVersion {
    std::uint64_t semantic = 0;
    std::uint64_t page = 0;

    auto operator<=>(PersistedPageSnapshotVersion const&) const = default;
};

enum class PersistedSamplePacking : std::uint8_t {
    dense,
    coverage_packed,
};

// Event times are page-relative. Sample data is either one complete dense page
// or the covered positions packed in increasing absolute-index order.
struct PersistedSamplePage {
    PersistedOutputId output{};
    std::uint64_t page_index = 0;
    Coverage domain{};
    ChannelLayout layout{};
    PersistedSamplePacking packing = PersistedSamplePacking::dense;
    std::vector<Sample> values{};
};

struct PersistedEventPage {
    PersistedOutputId output{};
    std::uint64_t page_index = 0;
    Coverage domain{};
    EventTypeId type = EventTypeId::empty;
    std::vector<TimedEvent> events{};
};

enum class PersistedPagePublishResult : std::uint8_t {
    published,
    stale_base,
};

// One executor-level canonical store for every persisted sample and event
// output. Candidate and reclamation operations are serialized control/background
// work. Only ReaderSlot::pin() and ReaderPin destruction are audio-thread APIs.
class PersistedPageStore {
public:
    class Candidate;
    class ReaderSlot;

    class Snapshot {
        friend class PersistedPageStore;
        friend class Candidate;

        PersistedPageSnapshotVersion version_{};
        std::size_t page_width_ = 0;
        std::vector<std::shared_ptr<PersistedSamplePage const>> sample_pages_{};
        std::vector<std::shared_ptr<PersistedEventPage const>> event_pages_{};
        struct SampleOutputMetadata {
            PersistedOutputId output{};
            Coverage coverage{};
            ChannelLayout layout{};
        };
        struct EventOutputMetadata {
            PersistedOutputId output{};
            Coverage coverage{};
            EventTypeId type = EventTypeId::empty;
        };
        std::vector<SampleOutputMetadata> sample_outputs_{};
        std::vector<EventOutputMetadata> event_outputs_{};

        void rebuild_output_metadata();

    public:
        [[nodiscard]] PersistedPageSnapshotVersion version() const noexcept
        {
            return version_;
        }

        [[nodiscard]] std::size_t page_width() const noexcept
        {
            return page_width_;
        }

        [[nodiscard]] std::size_t sample_page_count() const noexcept
        {
            return sample_pages_.size();
        }

        [[nodiscard]] std::size_t event_page_count() const noexcept
        {
            return event_pages_.size();
        }

        [[nodiscard]] PersistedSamplePage const* find_sample_page(
            PersistedOutputId const& output,
            std::uint64_t page_index) const noexcept;

        [[nodiscard]] PersistedEventPage const* find_event_page(
            PersistedOutputId const& output,
            std::uint64_t page_index) const noexcept;

        // Coverage and format metadata are assembled before publication. Tick
        // readers only select immutable records and never allocate.
        [[nodiscard]] Coverage const* find_sample_coverage(
            PersistedOutputId const& output,
            ChannelLayout layout) const noexcept;
        [[nodiscard]] Coverage const* find_event_coverage(
            PersistedOutputId const& output,
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
        friend class PersistedPageStore;

        PersistedPageStore* store_ = nullptr;
        Snapshot const* base_ = nullptr;
        PersistedPageSnapshotVersion base_version_{};
        std::unique_ptr<Snapshot> successor_{};

        Candidate(
            PersistedPageStore& store,
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

        [[nodiscard]] PersistedPageSnapshotVersion base_version() const noexcept
        {
            return base_version_;
        }

        [[nodiscard]] PersistedPageSnapshotVersion target_version() const noexcept;
        [[nodiscard]] std::size_t page_width() const noexcept;

        // Read-only access to the transaction-private successor. Background
        // realizations may consume pages already staged into this candidate;
        // the snapshot does not become globally visible until publish().
        [[nodiscard]] Snapshot const& working_snapshot() const;

        void put(PersistedSamplePage page);
        void put(PersistedEventPage page);
        void erase_page(
            PersistedOutputId const& output,
            std::uint64_t page_index);
        void erase_output(PersistedOutputId const& output);
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
        friend class PersistedPageStore;

        PersistedPageStore* store_ = nullptr;
        ReaderSlotState* state_ = nullptr;

        ReaderSlot(
            PersistedPageStore& store,
            ReaderSlotState& state) noexcept;
        void reset() noexcept;

    public:
        ReaderSlot() = default;
        ~ReaderSlot();
        ReaderSlot(ReaderSlot const&) = delete;
        ReaderSlot& operator=(ReaderSlot const&) = delete;
        ReaderSlot(ReaderSlot&& other) noexcept;
        ReaderSlot& operator=(ReaderSlot&& other) noexcept;

        // At most one ReaderPin may be live for a slot. Slot registration,
        // movement and destruction remain non-realtime operations; registry
        // mutation is synchronized with background reclamation.
        [[nodiscard]] ReaderPin pin() noexcept;
    };

private:
    std::unique_ptr<Snapshot const> published_owner_{};
    std::atomic<Snapshot const*> published_{nullptr};
    std::vector<std::unique_ptr<Snapshot const>> retired_{};
    mutable std::mutex reader_slots_mutex_{};
    std::vector<std::unique_ptr<ReaderSlotState>> reader_slots_{};

    void unregister_reader(ReaderSlotState& state) noexcept;

public:
    PersistedPageStore();
    ~PersistedPageStore();

    PersistedPageStore(PersistedPageStore const&) = delete;
    PersistedPageStore& operator=(PersistedPageStore const&) = delete;
    PersistedPageStore(PersistedPageStore&&) = delete;
    PersistedPageStore& operator=(PersistedPageStore&&) = delete;

    [[nodiscard]] Candidate begin_candidate(
        std::uint64_t target_semantic_version,
        std::size_t page_width);

    [[nodiscard]] PersistedPagePublishResult publish(Candidate&& candidate);

    // Background/control transactions use this immediately before committing
    // state derived from a pinned snapshot when they have no page candidate to
    // publish. Candidate publication performs the same base-identity check.
    [[nodiscard]] bool is_current(Snapshot const& snapshot) const noexcept
    {
        return published_.load(std::memory_order_seq_cst) == &snapshot;
    }

    // Registration and reclamation are control/background operations and may
    // allocate or destroy storage. Acquiring/releasing a pin does neither.
    [[nodiscard]] ReaderSlot register_reader();
    [[nodiscard]] std::size_t reclaim_retired();
    [[nodiscard]] std::size_t retired_snapshot_count() const noexcept
    {
        return retired_.size();
    }
};

} // namespace iv
