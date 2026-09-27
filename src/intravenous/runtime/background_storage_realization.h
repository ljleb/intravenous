#pragma once

#include <intravenous/compat.h>
#include <intravenous/coverage.h>
#include <intravenous/graph_jit/background_evaluation_plan.h>
#include <intravenous/runtime/persisted_page_store.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace iv {

// Explicit source/destination views used by the small runtime storage
// operations. The opaque owner remains address-stable for the realization's
// lifetime; operations never recover an executor or transaction through it.
struct BackgroundSampleReadView {
    void const* data = nullptr;
    Coverage const* coverage_value = nullptr;
    ChannelLayout layout{};
    std::span<std::size_t const> channels{};
    Sample (*read_sample)(
        void const*, SampleIndex, std::size_t) noexcept = nullptr;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return data != nullptr && coverage_value != nullptr
            && read_sample != nullptr;
    }

    [[nodiscard]] Coverage const& coverage() const noexcept;
    [[nodiscard]] bool has_channel(std::size_t channel) const noexcept;
    [[nodiscard]] Sample at(
        SampleIndex index,
        std::size_t channel) const noexcept;
};

struct BackgroundSampleWriteView {
    void* data = nullptr;
    Coverage const* coverage_value = nullptr;
    ChannelLayout layout{};
    std::span<std::size_t const> channels{};
    bool (*write_sample)(
        void*, SampleIndex, std::size_t, Sample) noexcept = nullptr;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return data != nullptr && coverage_value != nullptr
            && write_sample != nullptr;
    }

    [[nodiscard]] Coverage const& coverage() const noexcept;
    [[nodiscard]] bool has_channel(std::size_t channel) const noexcept;
    [[nodiscard]] bool write(
        SampleIndex index,
        std::size_t channel,
        Sample value) const noexcept;
};

struct BackgroundEventReadView {
    using VisitEvent = void (*)(void*, TimedEvent const&);
    using ForEachEvent = void (*)(
        void const*, IndexRegion, void*, VisitEvent);

    void const* data = nullptr;
    Coverage const* coverage_value = nullptr;
    EventTypeId type = EventTypeId::empty;
    ForEachEvent for_each_event = nullptr;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return data != nullptr && coverage_value != nullptr
            && for_each_event != nullptr;
    }

    [[nodiscard]] Coverage const& coverage() const noexcept;

    template<typename Fn>
    void for_each(IndexRegion region, Fn&& fn) const
    {
        IV_ASSERT(*this, "background event storage has no readable view");
        IV_ASSERT(region.valid() && coverage().contains(region),
            "background event storage read lies outside selected coverage");
        if (region.empty()) return;
        using Visitor = std::remove_reference_t<Fn>;
        for_each_event(
            data,
            region,
            std::addressof(fn),
            +[](void* opaque, TimedEvent const& event) {
                (*static_cast<Visitor*>(opaque))(event);
            });
    }
};

struct BackgroundEventWriteView {
    void* data = nullptr;
    Coverage const* coverage_value = nullptr;
    EventTypeId type = EventTypeId::empty;
    bool (*write_event)(void*, TimedEvent const&) noexcept = nullptr;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return data != nullptr && coverage_value != nullptr
            && write_event != nullptr;
    }

    [[nodiscard]] Coverage const& coverage() const noexcept;
    [[nodiscard]] bool write(TimedEvent const& event) const noexcept;
};

// Exact storage domains selected by the transaction coordinator. The vector is
// aligned with BackgroundStoragePlan::ports. The published snapshot is
// non-owning: the surrounding transaction keeps its reader pin alive.
struct BackgroundStorageSelection {
    std::uint64_t generation = 0;
    std::vector<Coverage> storage_coverage{};
    // Empty preserves the plan-derived default used by direct component tests.
    // A complete transaction supplies one flag per storage slot so a persisted
    // authored output can be read from the pinned base when it is still valid,
    // or privately regenerated when its node is scheduled.
    std::vector<bool> produce_storage{};
    PersistedPageStore::Snapshot const* published = nullptr;
};

// Interprets immutable PortStoragePlan indices into address-stable runtime
// owners and views. It owns transaction-local/materialized buffers, but it does
// not traverse nodes, decide demand, choose operation placement, or publish.
// Callers execute only the runtime operation indices already placed by the plan.
class BackgroundStorageRealization {
    struct Slot;

    graph_jit::BackgroundEvaluationPlan const* plan_ = nullptr;
    BackgroundStorageSelection selection_{};
    std::vector<std::unique_ptr<Slot>> slots_{};
    std::vector<bool> executed_operations_{};
    bool sealed_ = false;

    [[nodiscard]] Slot& slot(graph_jit::PortStorageIndex index);
    [[nodiscard]] Slot const& slot(graph_jit::PortStorageIndex index) const;

public:
    BackgroundStorageRealization(
        graph_jit::BackgroundEvaluationPlan const& plan,
        BackgroundStorageSelection selection);
    ~BackgroundStorageRealization();

    BackgroundStorageRealization(BackgroundStorageRealization const&) = delete;
    BackgroundStorageRealization& operator=(
        BackgroundStorageRealization const&) = delete;
    BackgroundStorageRealization(BackgroundStorageRealization&&) = delete;
    BackgroundStorageRealization& operator=(
        BackgroundStorageRealization&&) = delete;

    [[nodiscard]] std::size_t storage_count() const noexcept;
    [[nodiscard]] std::size_t operation_count() const noexcept;
    [[nodiscard]] bool operation_executed(
        graph_jit::BackgroundRuntimeOperationIndex index) const noexcept;
    [[nodiscard]] bool sealed() const noexcept { return sealed_; }
    [[nodiscard]] BackgroundStorageSelection const& selection() const noexcept
    {
        return selection_;
    }

    // External current-Tick or separately owned materialized storage is bound
    // before sealing. Owned transaction buffers and persisted readers are
    // installed by the constructor.
    void bind_sample(
        graph_jit::PortStorageIndex index,
        BackgroundSampleReadView read,
        BackgroundSampleWriteView write = {});
    void bind_event(
        graph_jit::PortStorageIndex index,
        BackgroundEventReadView read,
        BackgroundEventWriteView write = {});

    [[nodiscard]] BackgroundSampleReadView const* sample_read(
        graph_jit::PortStorageIndex index) const;
    [[nodiscard]] BackgroundSampleWriteView const* sample_write(
        graph_jit::PortStorageIndex index) const;
    [[nodiscard]] BackgroundEventReadView const* event_read(
        graph_jit::PortStorageIndex index) const;
    [[nodiscard]] BackgroundEventWriteView const* event_write(
        graph_jit::PortStorageIndex index) const;

    [[nodiscard]] PersistedOutputId const* persisted_output(
        graph_jit::PortStorageIndex index) const;

    // Freezes external bindings and verifies that every retained runtime
    // binding and storage operation has the explicit views it requires.
    [[nodiscard]] std::expected<void, std::string> seal();

    // Executes one compiler-placed operation against the sealed views. Direct
    // operations validate no-copy aliases; materializations perform
    // the planned conversion/projection/fan-in into their owned destination.
    // Each runtime operation may execute exactly once per realization.
    [[nodiscard]] std::expected<void, std::string> execute_operation(
        graph_jit::BackgroundRuntimeOperationIndex index);

    // Verifies that every selected sample/channel owned by this realization
    // was initialized by Tock/replay or a placed materialization operation.
    // Event storage needs no analogous "written" bitmap because an empty
    // sequence is a complete event result; rejected event writes are tracked by
    // the invocation frame instead.
    [[nodiscard]] std::expected<void, std::string>
    validate_produced_storage() const;

    // Serializes every selected runtime-produced persisted slot into the
    // transaction's private candidate. No page becomes visible here.
    [[nodiscard]] std::expected<void, std::string> stage_persisted_pages(
        PersistedPageStore::Candidate& candidate) const;
};

// Address-stable transaction-local frame passed opaquely through generated
// background code. The retained spans come directly from one node's immutable
// placement plan; this leaf never discovers or reorders operations.
class BackgroundStorageOperationFrame {
public:
    using LeafHook = void (*)(void*);

private:
    BackgroundStorageRealization* realization_ = nullptr;
    std::span<graph_jit::BackgroundRuntimeOperationIndex const> before_{};
    std::span<graph_jit::BackgroundRuntimeOperationIndex const> after_{};
    void* leaf_data_ = nullptr;
    LeafHook prepare_leaf_ = nullptr;
    LeafHook finalize_leaf_ = nullptr;

    void execute(
        std::span<graph_jit::BackgroundRuntimeOperationIndex const> operations);

public:
    BackgroundStorageOperationFrame() = default;
    BackgroundStorageOperationFrame(
        BackgroundStorageRealization& realization,
        std::span<graph_jit::BackgroundRuntimeOperationIndex const> before,
        std::span<graph_jit::BackgroundRuntimeOperationIndex const> after)
        noexcept;

    // Replay uses these transaction-local leaf hooks to move between sealed
    // addressable storage and isolated raw Tick buffers. Generated code still
    // sees only this opaque operation frame.
    void set_leaf_hooks(
        void* data, LeafHook prepare_leaf, LeafHook finalize_leaf) noexcept;

    void prepare();
    void finalize();

    static void prepare_callback(void* opaque);
    static void finalize_callback(void* opaque);
};

} // namespace iv
