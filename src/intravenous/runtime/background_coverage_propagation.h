#pragma once

#include <intravenous/coverage.h>
#include <intravenous/runtime/graph_jit.h>

#include <cstddef>
#include <span>
#include <vector>

namespace iv {

// Generation-local roots for one exact coverage-propagation operation. Port
// and node indices come from the bound CompiledGraph's immutable background
// plan.
struct OutputCoverageChangeRequest {
    graph_jit::BackgroundPortIndex port = 0;
    Coverage coverage{};
    Coverage changed{};
};

struct OutputCoverageRequest {
    graph_jit::BackgroundPortIndex port = 0;
    Coverage required{};
};

struct InputCoverageChangeRequest {
    graph_jit::BackgroundPortIndex port = 0;
    Coverage coverage{};
    Coverage changed{};
};

struct InputCoverageRequest {
    graph_jit::BackgroundPortIndex port = 0;
    Coverage required{};
};

struct CoveragePropagationRequest {
    std::vector<graph_jit::BackgroundNodeIndex> locally_changed_nodes{};
    std::vector<InputCoverageChangeRequest> input_changes{};
    std::vector<OutputCoverageChangeRequest> output_changes{};
    std::vector<InputCoverageRequest> input_demands{};
    std::vector<OutputCoverageRequest> output_demands{};
};

struct PropagatedOutputChange {
    graph_jit::BackgroundPortIndex port = 0;
    Coverage coverage{};
    Coverage changed{};
};

struct RequiredOutputCoverage {
    graph_jit::BackgroundPortIndex port = 0;
    Coverage required{};
};

struct RequiredInputCoverage {
    graph_jit::BackgroundPortIndex port = 0;
    Coverage required{};
};

// Propagation deliberately returns coverage changes and requirements, not
// sample or event data. A complete background transaction consumes these
// requirements before it promotes semantic coverage or publishes pages.
struct CoveragePropagationResult {
    std::vector<PropagatedOutputChange> output_changes{};
    std::vector<RequiredInputCoverage> input_requirements{};
    std::vector<RequiredOutputCoverage> output_requirements{};
};

// Optional transaction policy invoked after the generated forward traversal
// and before reverse propagation. Persisted-page planning uses this narrow
// seam to complete page-sized output demands without teaching propagation
// about page stores. The callback may only add to `additional`.
struct BackgroundCoverageDemandExpansion {
    void* data = nullptr;
    void (*expand_output)(
        void*, graph_jit::BackgroundPortIndex, Coverage const& available,
        Coverage const& changed, Coverage const& required,
        Coverage& additional) = nullptr;
};

class BackgroundPropagationWorkspace;

// Long-lived semantic coverage committed for one compiled realization. It is
// intentionally separate from both reusable accumulator scratch and one
// transaction's prepared candidate.
class BackgroundCoverageState {
    std::vector<Coverage> output_coverages_{};

    friend class BackgroundPropagationWorkspace;

public:
    BackgroundCoverageState() = default;
    explicit BackgroundCoverageState(std::size_t output_count);
};

// Owns one successful F/R candidate until a surrounding operation either
// commits or discards it. The const views let the transaction coordinator plan
// storage and evaluation without mutating the committed baseline.
class PreparedCoveragePropagation {
    CoveragePropagationResult result_{};
    std::vector<Coverage> output_coverages_{};
    std::vector<graph_jit::BackgroundNodeActivity> node_activity_{};

    PreparedCoveragePropagation() = default;

    friend class BackgroundPropagationWorkspace;

public:
    PreparedCoveragePropagation(PreparedCoveragePropagation const&) = delete;
    PreparedCoveragePropagation&
    operator=(PreparedCoveragePropagation const&) = delete;
    PreparedCoveragePropagation(PreparedCoveragePropagation&&) noexcept =
        default;
    PreparedCoveragePropagation&
    operator=(PreparedCoveragePropagation&&) noexcept = default;
    ~PreparedCoveragePropagation() = default;

    [[nodiscard]] CoveragePropagationResult const& result() const noexcept
    {
        return result_;
    }

    [[nodiscard]] std::span<graph_jit::BackgroundNodeActivity const>
    node_activity() const noexcept
    {
        return node_activity_;
    }

};

// Reusable propagation scratch and callback frames. Each preparation starts
// from BackgroundCoverageState and may mutate only a private candidate. This
// component knows accumulator topology and generated F/R roots, but owns no
// persisted pages, materialized storage or transaction publication policy.
class BackgroundPropagationWorkspace {
    struct InputChangeAccumulator {
        Coverage coverage{};
        Coverage changed{};
    };

    struct OutputChangeAccumulator {
        BackgroundPropagationWorkspace* owner = nullptr;
        graph_jit::BackgroundPortIndex port = 0;
        Coverage previous_coverage{};
        Coverage changed{};
        bool touched = false;
    };

    struct InputRequirementAccumulator {
        BackgroundPropagationWorkspace* owner = nullptr;
        graph_jit::BackgroundPortIndex port = 0;
        Coverage coverage{};
    };

    struct OutputRequirementAccumulator {
        Coverage required{};
    };

    struct NodeCoverageCallData {
        std::vector<InputCoverageChange> sample_input_changes{};
        std::vector<InputCoverageChange> event_input_changes{};
        std::vector<OutputCoverageChange> sample_output_changes{};
        std::vector<OutputCoverageChange> event_output_changes{};
        std::vector<InputCoverageRequirement> sample_input_requirements{};
        std::vector<InputCoverageRequirement> event_input_requirements{};
        std::vector<OutputCoverageRequirement> sample_output_requirements{};
        std::vector<OutputCoverageRequirement> event_output_requirements{};
    };

    graph_jit::BackgroundEvaluationPlan const* plan_ = nullptr;
    std::size_t sample_rate_ = 0;
    std::vector<Coverage> candidate_output_coverages_{};
    std::vector<InputChangeAccumulator> input_changes_{};
    std::vector<OutputChangeAccumulator> output_changes_{};
    std::vector<InputRequirementAccumulator> input_requirements_{};
    std::vector<OutputRequirementAccumulator> output_requirements_{};
    std::vector<Coverage> input_requirements_by_port_{};
    std::vector<InputCoverageChange> callback_input_changes_{};
    std::vector<OutputCoverageChange> callback_output_changes_{};
    std::vector<InputCoverageRequirement> callback_input_requirements_{};
    std::vector<OutputCoverageRequirement> callback_output_requirements_{};
    std::vector<NodeCoverageCallData> node_call_data_{};
    std::vector<graph_jit::BackgroundNodeCall> node_calls_{};
    graph_jit::BackgroundEvaluationCall call_{};

    [[nodiscard]] graph_jit::BackgroundPortPlan const&
    port(graph_jit::BackgroundPortIndex index) const;
    [[nodiscard]] std::size_t
    output_index(graph_jit::BackgroundPortIndex index) const;
    [[nodiscard]] std::size_t
    input_index(graph_jit::BackgroundPortIndex index) const;
    [[nodiscard]] Coverage
    input_coverage(graph_jit::BackgroundPortIndex input) const;
    void activate_node(graph_jit::BackgroundNodeIndex node,
                       graph_jit::BackgroundNodeActivity activity);
    void add_input_change(graph_jit::BackgroundPortIndex input,
                          Coverage const& changed, bool coverage_changed);
    void route_output_change(graph_jit::BackgroundPortIndex output,
                             Coverage const& changed, bool coverage_changed);
    void set_input_change(graph_jit::BackgroundPortIndex input,
                          Coverage const& coverage, Coverage const& changed);
    void publish_output_coverage(graph_jit::BackgroundPortIndex output,
                                 Coverage const& coverage);
    void publish_output_change(graph_jit::BackgroundPortIndex output,
                               Coverage const& changed);
    void require_output(graph_jit::BackgroundPortIndex output,
                        Coverage const& requested, bool activate_producer);
    void require_input(graph_jit::BackgroundPortIndex input,
                       Coverage const& requested);
    void initialize_calls();
    void reset(BackgroundCoverageState const& coverage);

    static void publish_output_coverage_callback(void* accumulator,
                                                 Coverage const& coverage);
    static void publish_output_change_callback(void* accumulator,
                                               Coverage const& changed);
    static void publish_input_requirement_callback(void* accumulator,
                                                   Coverage const& required);
    static void
    replay_forward_coverage(void*,
                            ReflectedNodeForwardCoverageContext const& context);
    static void
    replay_reverse_coverage(void*,
                            ReflectedNodeReverseCoverageContext const& context);

public:
    BackgroundPropagationWorkspace(
        graph_jit::BackgroundEvaluationPlan const& plan,
        std::size_t sample_rate);
    BackgroundPropagationWorkspace(
        BackgroundPropagationWorkspace const&) = delete;
    BackgroundPropagationWorkspace& operator=(
        BackgroundPropagationWorkspace const&) = delete;
    BackgroundPropagationWorkspace(
        BackgroundPropagationWorkspace&&) = delete;
    BackgroundPropagationWorkspace& operator=(
        BackgroundPropagationWorkspace&&) = delete;
    ~BackgroundPropagationWorkspace() = default;

    [[nodiscard]] PreparedCoveragePropagation
    prepare(CompiledGraphBackgroundOperations const& operations,
            std::byte* storage, BackgroundCoverageState const& coverage,
            CoveragePropagationRequest const& request,
            BackgroundCoverageDemandExpansion expansion = {});
    [[nodiscard]] CoveragePropagationResult
    commit(BackgroundCoverageState& coverage,
           PreparedCoveragePropagation&& prepared) noexcept;
    void discard(PreparedCoveragePropagation&& prepared) noexcept;
};

} // namespace iv
