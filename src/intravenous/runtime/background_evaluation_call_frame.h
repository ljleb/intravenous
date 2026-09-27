#pragma once

#include <intravenous/coverage.h>
#include <intravenous/graph_jit/background_evaluation_call.h>
#include <intravenous/graph_jit/background_evaluation_plan.h>
#include <intravenous/runtime/background_storage_realization.h>

#include <cstddef>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace iv {

// Dynamic logical-port domains selected by the transaction coordinator. The
// coverage vector is aligned with BackgroundRuntimePlan::bindings rather than
// storage slots: direct aliases may shift a source storage domain by their
// planned read latency, and one logical output may fan out to several stores.
struct BackgroundEvaluationCallFrameSelection {
    std::vector<Coverage> binding_coverage{};
    std::size_t sample_rate = 48000;
};

// Address-stable backing for one generated BackgroundEvaluationCall. This is a
// transaction-local assembly layer, not the transaction coordinator: it binds
// the coordinator's already-selected logical coverage to a sealed storage
// realization, owns each node's operation frame and compact Tock facades, and
// owns the compiler-sized replay binding arrays and their isolated raw backing.
// Coverage propagation, activity selection and commit remain outside it.
class BackgroundEvaluationCallFrame {
    struct BindingAdapter;
    struct NodeFrameStorage;
    struct ReplayFrameStorage;

    graph_jit::BackgroundEvaluationPlan const* plan_ = nullptr;
    BackgroundStorageRealization* realization_ = nullptr;
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
    static void finalize_replay(void* opaque);

public:
    BackgroundEvaluationCallFrame(
        graph_jit::BackgroundEvaluationPlan const& plan,
        BackgroundStorageRealization& realization,
        BackgroundEvaluationCallFrameSelection selection);
    ~BackgroundEvaluationCallFrame();

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

    // Replay binding arrays have their final addresses at construction; seal()
    // populates them with isolated transaction-local backing derived from the
    // replay regions. Mutable access remains available for focused frame/ABI
    // validation before seal. Ordering is the corresponding replay plan's
    // input/output binding order, filtered by port kind.
    [[nodiscard]] std::span<ReflectedSampleInputPortBinding>
    replay_sample_inputs(graph_jit::BackgroundReplayInvocationSlot slot);
    [[nodiscard]] std::span<ReflectedSampleOutputPortBinding>
    replay_sample_outputs(graph_jit::BackgroundReplayInvocationSlot slot);
    [[nodiscard]] std::span<ReflectedEventInputPortBinding>
    replay_event_inputs(graph_jit::BackgroundReplayInvocationSlot slot);
    [[nodiscard]] std::span<ReflectedEventOutputPortBinding>
    replay_event_outputs(graph_jit::BackgroundReplayInvocationSlot slot);

    // Regions are accepted only as an explicit invocation schedule: sorted,
    // nonempty and no larger than the compiled replay block limit.
    [[nodiscard]] std::expected<void, std::string>
    set_replay_regions(graph_jit::BackgroundReplayInvocationSlot slot,
                       std::vector<IndexRegion> regions);

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
