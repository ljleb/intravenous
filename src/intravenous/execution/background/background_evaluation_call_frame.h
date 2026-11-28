#pragma once

#include <intravenous/coverage.h>
#include <intravenous/graph_jit/background_evaluation_call.h>
#include <intravenous/graph_jit/background_evaluation_plan.h>
#include <intravenous/node/layout.h>
#include <intravenous/execution/background/background_coverage_propagation.h>
#include <intravenous/execution/background/background_storage_realization.h>

#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace iv {

// Dynamic logical-port domains selected by the transaction coordinator. The
// coverage vector is aligned with BackgroundRuntimePlan::bindings rather than
// storage slots: direct aliases may shift a source storage domain by their
// planned read latency, and one logical output may fan out to several stores.
struct BackgroundEvaluationCallFrameSelection {
    struct IndexedEventRegion {
        RegionRef region{};
        GlobalIndexRange range{};
        std::vector<IndexRun> runs{};
        std::optional<Event> constant_value{};
        std::optional<graph_jit::BackgroundBindingSlot> source{};
        std::size_t source_region = 0;
    };
    std::vector<IndexCoverage> binding_coverage{};
    std::vector<BackgroundSampleDemand> sample_positions{};
    std::vector<SampleDemandByRegion> sample_region_positions{};
    std::uint64_t selection_snapshot = 0;
    std::vector<std::vector<GlobalIndexRange>> binding_regions{};
    std::vector<std::vector<BackgroundInputRegion>> binding_input_regions{};
    std::vector<std::vector<IndexedEventRegion>> indexed_events{};
    std::size_t indexed_event_budget = 0;
    // Per binding slot, for Sequential replay inputs: positions where an
    // upstream contribution exists. Replay resolves a scheduled position
    // outside it as known absent, by the input's fallback; a covered position
    // outside binding_coverage is unavailable data and fails the replay. When
    // empty, every scheduled position must be selected.
    std::vector<IndexCoverage> binding_upstream_coverage{};
    graph_jit::SequentialInputCombineFunction combine_sequential_input = nullptr;
    void* replay_input_owner = nullptr;
    void (*prepare_replay_inputs)(void*, std::span<InputCoverageRequest const>,
        std::span<BackgroundDiscoveredInput>) = nullptr;
    std::size_t sample_rate = 48000;
};

// Address-stable backing for one generated BackgroundEvaluationCall. This is a
// transaction-local assembly layer, not the transaction coordinator: it binds
// the coordinator's already-selected logical coverage to a sealed storage
// realization, owns each node's operation frame and compact Tock facades, and
// owns the compiler-sized replay binding arrays and their isolated raw backing.
// IndexCoverage propagation, activity selection and commit remain outside it.
class BackgroundEvaluationCallFrame {
    struct BindingAdapter;
    struct NodeFrameStorage;
    struct ReplayFrameStorage;

    graph_jit::BackgroundEvaluationPlan const* plan_ = nullptr;
    BackgroundStorageRealization* realization_ = nullptr;
    TockStateStorage* tock_states_ = nullptr;
    BackgroundEvaluationCallFrameSelection selection_{};
    std::vector<graph_jit::BackgroundNodeCall> nodes_{};
    std::vector<std::unique_ptr<BackgroundStorageOperationFrame>>
        operation_frames_{};
    std::vector<std::unique_ptr<BindingAdapter>> binding_adapters_{};
    std::vector<std::unique_ptr<NodeFrameStorage>> node_storage_{};
    std::vector<std::unique_ptr<ReplayFrameStorage>> replay_storage_{};
    graph_jit::BackgroundEvaluationCall call_{};
    bool sealed_ = false;

    static void prepare_replay(void* opaque);
    static void prepare_replay_region(void* opaque, std::uint64_t begin, std::uint64_t extent);
    static void finalize_replay(void* opaque);

public:
    BackgroundEvaluationCallFrame(
        graph_jit::BackgroundEvaluationPlan const& plan,
        BackgroundStorageRealization& realization,
        TockStateStorage& tock_states,
        BackgroundEvaluationCallFrameSelection selection);
    ~BackgroundEvaluationCallFrame();
    [[nodiscard]] std::span<EventRunStorageView const> indexed_events(graph_jit::BackgroundPortIndex) const noexcept;
    void prepare_indexed_input(graph_jit::BackgroundPortIndex);

    BackgroundEvaluationCallFrame(BackgroundEvaluationCallFrame const&) =
        delete;
    BackgroundEvaluationCallFrame&
    operator=(BackgroundEvaluationCallFrame const&) = delete;
    BackgroundEvaluationCallFrame(BackgroundEvaluationCallFrame&&) = delete;
    BackgroundEvaluationCallFrame&
    operator=(BackgroundEvaluationCallFrame&&) = delete;

    [[nodiscard]] bool sealed() const noexcept { return sealed_; }

    // The coordinator fills activity and F/R accumulator contexts through
    // these dense node slots before invoking the generated roots.
    [[nodiscard]] std::span<graph_jit::BackgroundNodeCall> nodes() noexcept
    {
        return nodes_;
    }
    [[nodiscard]] std::span<graph_jit::BackgroundNodeCall const>
    nodes() const noexcept
    {
        return nodes_;
    }

    // Read-only discovery facades for a selected input binding. Their backing
    // remains owned by this sealed frame and its storage realization.
    [[nodiscard]] RandomAccessContinuousInputPort discovery_continuous_input(
        graph_jit::BackgroundPortIndex) const;
    [[nodiscard]] RandomAccessEventInputPort discovery_event_input(
        graph_jit::BackgroundPortIndex) const;
    [[nodiscard]] std::optional<Sample> observe_sequential_sample_input(
        graph_jit::BackgroundPortIndex, GlobalIndex, std::size_t channel) const;

    // Replay binding arrays have their final addresses at construction; seal()
    // populates them with isolated transaction-local backing derived from the
    // replay regions. Mutable access remains available for focused frame/ABI
    // validation before seal. Ordering is the corresponding replay plan's
    // input/output binding order, filtered by port kind.
    [[nodiscard]] std::span<ReflectedContinuousInputPortBinding>
    replay_sample_inputs(graph_jit::BackgroundReplayInvocationSlot slot);
    [[nodiscard]] std::span<ReflectedContinuousOutputPortBinding>
    replay_sample_outputs(graph_jit::BackgroundReplayInvocationSlot slot);
    [[nodiscard]] std::span<ReflectedEventInputPortBinding>
    replay_event_inputs(graph_jit::BackgroundReplayInvocationSlot slot);
    [[nodiscard]] std::span<ReflectedEventOutputPortBinding>
    replay_event_outputs(graph_jit::BackgroundReplayInvocationSlot slot);

    // Regions are accepted only as an explicit invocation schedule: sorted,
    // nonempty and no larger than the compiled replay block limit.
    [[nodiscard]] std::expected<void, std::string>
    set_replay_regions(graph_jit::BackgroundReplayInvocationSlot slot,
                       std::vector<GlobalIndexRange> regions);

    // Freezes every facade/span after all external storage and replay records
    // have been populated. The underlying storage realization must already be
    // sealed. A successful call may be passed directly to generated code.
    [[nodiscard]] std::expected<void, std::string> seal();

    // Called after the generated evaluate root returns and before any candidate
    // publication. It rejects invalid callback writes and incomplete produced
    // sample storage so default-initialized transaction memory can never become
    // authoritative output.
    [[nodiscard]] std::expected<void, std::string>
    validate_evaluation() const;

    [[nodiscard]] graph_jit::BackgroundEvaluationCall& call() noexcept;
    [[nodiscard]] graph_jit::BackgroundEvaluationCall const&
    call() const noexcept;
};

} // namespace iv
