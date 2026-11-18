#pragma once

#include <intravenous/coverage.h>
#include <intravenous/runtime/graph_jit.h>

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace iv {

// Generation-local roots for one exact coverage-propagation operation. Port
// and node indices come from the bound CompiledGraph's immutable background
// plan.
struct OutputCoverageChangeRequest {
    graph_jit::BackgroundPortIndex port = 0;
    IndexCoverage coverage{};
    IndexCoverage changed{};
    std::vector<GlobalIndexRange> regions{};
    // Exact outputs supplied by another selected domain are immutable for
    // this forward traversal; no callback may replace their publication.
    bool finalized = false;
};

struct OutputCoverageRequest {
    graph_jit::BackgroundPortIndex port = 0;
    IndexCoverage required{};
};

struct InputCoverageChangeRequest {
    graph_jit::BackgroundPortIndex port = 0;
    std::vector<GlobalIndexRange> regions{};
    IndexCoverage coverage{};
    IndexCoverage changed{};
};

struct InputCoverageRequest {
    graph_jit::BackgroundPortIndex port = 0;
    IndexCoverage required{};
};

struct CoveragePropagationRequest {
    std::vector<graph_jit::BackgroundNodeIndex> locally_changed_nodes{};
    // Refresh forward metadata without claiming that node configuration or
    // semantic local state changed.
    std::vector<graph_jit::BackgroundNodeIndex> forward_nodes{};
    std::vector<InputCoverageChangeRequest> input_changes{};
    std::vector<OutputCoverageChangeRequest> output_changes{};
    std::vector<InputCoverageRequest> input_demands{};
    std::vector<OutputCoverageRequest> output_demands{};
};

struct PropagatedOutputChange {
    graph_jit::BackgroundPortIndex port = 0;
    IndexCoverage coverage{};
    IndexCoverage changed{};
};

struct RequiredOutputCoverage {
    graph_jit::BackgroundPortIndex port = 0;
    IndexCoverage required{};
};

struct RequiredInputCoverage {
    graph_jit::BackgroundPortIndex port = 0;
    IndexCoverage required{};
};

// Propagation deliberately returns coverage changes and requirements, not
// sample or event data. A complete background transaction consumes these
// requirements before it promotes semantic coverage or publishes retained data.
struct CoveragePropagationResult {
    std::vector<PropagatedOutputChange> output_changes{};
    std::vector<RequiredInputCoverage> input_requirements{};
    std::vector<RequiredOutputCoverage> output_requirements{};
};

// Optional transaction policy invoked after the generated forward traversal
// and before reverse propagation. Persisted-data planning uses this narrow
// seam to complete bounded output demands without teaching propagation about
// persisted-data storage. The callback may only add to `additional`.
// Changed coverage of every output after the forward traversal.
struct BackgroundForwardOutputChanges {
    void const* data = nullptr;
    IndexCoverage const& (*changed_value)(
        void const*, graph_jit::BackgroundPortIndex output) = nullptr;

    [[nodiscard]] IndexCoverage const& changed(
        graph_jit::BackgroundPortIndex output) const
    {
        return changed_value(data, output);
    }
};

struct BackgroundCoverageDemandExpansion {
    void* data = nullptr;
    void (*expand_output)(
        void*, graph_jit::BackgroundPortIndex, IndexCoverage const& available,
        std::span<GlobalIndexRange const> regions,
        IndexCoverage const& changed, IndexCoverage const& required,
        IndexCoverage& additional) = nullptr;
    // Optional policy for explicit input demand roots, invoked after the
    // forward traversal and before the root reaches reverse propagation. The
    // callback may only remove coverage from `required`.
    void (*restrict_input)(
        void*, graph_jit::BackgroundPortIndex,
        BackgroundForwardOutputChanges const& forward,
        IndexCoverage& required) = nullptr;
    // Runtime cache admission is independent of the authored realization.
    bool (*cached_output)(void*, graph_jit::BackgroundPortIndex) = nullptr;
};

class PreparedCoveragePropagation;

// Discovery views own their sealed evaluation frame and its storage. They are
// private to the selected candidate and never become playback publications.
struct BackgroundDiscoveredInput {
    std::shared_ptr<void const> owner{};
    RandomAccessContinuousInputPort continuous{};
    RandomAccessEventInputPort events{};
};

struct BackgroundCoverageDiscovery {
    void* data = nullptr;
    BackgroundDiscoveredInput (*materialize)(void*, PreparedCoveragePropagation&&,
        graph_jit::BackgroundPortIndex) = nullptr;
};

struct BackgroundCoverageReadDependency {
    graph_jit::BackgroundNodeIndex node = 0;
    bool reverse = false;
    graph_jit::BackgroundPortIndex input = 0;
    CoverageInformation information = CoverageInformation::coverage;
    IndexCoverage positions{};
    std::vector<GlobalIndexRange> regions{};
};

class BackgroundPropagationWorkspace;

// Long-lived semantic coverage committed for one compiled realization. It is
// intentionally separate from both reusable accumulator scratch and one
// transaction's prepared candidate.
class BackgroundCoverageState {
public:
    struct Migration;

private:
    std::vector<IndexCoverage> output_coverages_{};
    std::vector<std::vector<GlobalIndexRange>> output_region_ranges_{};
    std::vector<BackgroundCoverageReadDependency> read_dependencies_{};

    friend class BackgroundPropagationWorkspace;

public:
    BackgroundCoverageState() = default;
    explicit BackgroundCoverageState(std::size_t output_count);

    // Prepares an index-independent transfer between compatible stable output
    // identities. The descriptor may be built before the predecessor finishes
    // draining; commit() reads its final coverage after that drain.
    [[nodiscard]] Migration prepare_migration_from(
        BackgroundCoverageState& previous,
        graph_jit::BackgroundEvaluationPlan const& current_plan,
        graph_jit::BackgroundEvaluationPlan const& previous_plan,
        bool local_semantic_environment_changed = false);
};

struct BackgroundCoverageState::Migration {
    struct OutputTransfer {
        std::size_t current = 0;
        std::size_t previous = 0;
    };
    struct ChangedInput {
        graph_jit::BackgroundPortIndex current = 0;
        graph_jit::BackgroundPortIndex previous = 0;
    };

    BackgroundCoverageState* current = nullptr;
    BackgroundCoverageState* previous = nullptr;
    graph_jit::BackgroundEvaluationPlan const* current_plan = nullptr;
    graph_jit::BackgroundEvaluationPlan const* previous_plan = nullptr;
    std::vector<OutputTransfer> outputs{};
    std::vector<ChangedInput> changed_inputs{};
    CoveragePropagationRequest reconciliation_roots{};
    bool committed = false;

    Migration() = default;
    Migration(Migration&&) noexcept = default;
    Migration& operator=(Migration&&) noexcept = default;
    Migration(Migration const&) = delete;
    Migration& operator=(Migration const&) = delete;

    void commit();
    [[nodiscard]] CoveragePropagationRequest take_reconciliation_roots();
};

// Owns one successful F/R candidate until a surrounding operation either
// commits or discards it. The const views let the transaction coordinator plan
// storage and evaluation without mutating the committed baseline.
class PreparedCoveragePropagation {
    CoveragePropagationResult result_{};
    std::vector<IndexCoverage> output_coverages_{};
    std::vector<std::vector<GlobalIndexRange>> output_region_ranges_{};
    std::vector<BackgroundCoverageReadDependency> read_dependencies_{};
    std::vector<std::vector<GlobalIndexRange>> output_regions_by_port_{};
    std::vector<std::vector<GlobalIndexRange>> input_regions_by_port_{};
    std::vector<IndexCoverage> input_coverages_by_port_{};
    std::vector<std::vector<GlobalIndexRange>> requested_input_regions_by_port_{};
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

    // Exact semantic output coverage, indexed by output-change accumulator.
    // These values belong to this prepared transaction, independently of how
    // much of any output the transaction materializes.
    [[nodiscard]] std::span<IndexCoverage const> output_coverages() const noexcept
    {
        return output_coverages_;
    }

    [[nodiscard]] std::span<graph_jit::BackgroundNodeActivity const>
    node_activity() const noexcept
    {
        return node_activity_;
    }

    [[nodiscard]] std::span<GlobalIndexRange const> output_regions(
        graph_jit::BackgroundPortIndex port) const noexcept
    {
        if (port >= output_regions_by_port_.size()) return {};
        return output_regions_by_port_[port];
    }

    [[nodiscard]] std::span<BackgroundCoverageReadDependency const>
    read_dependencies() const noexcept { return read_dependencies_; }

    // Intrinsic logical input Regions, before the evaluation request selects
    // any of them. Storage planning may inspect these without exposing them.
    [[nodiscard]] std::span<GlobalIndexRange const> input_regions(
        graph_jit::BackgroundPortIndex port) const noexcept
    {
        if (port >= input_regions_by_port_.size()) return {};
        return input_regions_by_port_[port];
    }

    // Positions where at least one upstream contribution reaches the input,
    // with the routing rules of derive_input_coverage(). A Sequential input
    // resolves every other position by its declared fallback.
    [[nodiscard]] IndexCoverage const& input_coverage(
        graph_jit::BackgroundPortIndex port) const noexcept
    {
        static IndexCoverage const none{};
        if (port >= input_coverages_by_port_.size()) return none;
        return input_coverages_by_port_[port];
    }

    // Whole logical Regions selected by the original reverse request, retaining
    // their ordering and multiplicity independently of wider producer work.
    [[nodiscard]] std::span<GlobalIndexRange const> requested_input_regions(
        graph_jit::BackgroundPortIndex port) const noexcept
    {
        if (port >= requested_input_regions_by_port_.size()) return {};
        return requested_input_regions_by_port_[port];
    }

};

// Reusable propagation scratch and callback frames. Each preparation starts
// from BackgroundCoverageState and may mutate only a private candidate. This
// component knows accumulator topology and generated F/R roots, but owns no
// persisted data, materialized storage or transaction publication policy.
class BackgroundPropagationWorkspace {
    struct PlanningState;
    struct MetadataRead {
        BackgroundPropagationWorkspace* owner = nullptr;
        graph_jit::BackgroundPortIndex input = 0;
        bool reverse = false;
    };

    struct InputChangeAccumulator {
        std::vector<GlobalIndexRange> regions{};
        std::span<GlobalIndexRange const> region_view{};
        IndexCoverage coverage{};
        IndexCoverage changed{};
    };

    struct OutputChangeAccumulator {
        BackgroundPropagationWorkspace* owner = nullptr;
        graph_jit::BackgroundPortIndex port = 0;
        std::vector<GlobalIndexRange> previous_regions{};
        std::span<GlobalIndexRange const> previous_region_view{};
        IndexCoverage previous_coverage{};
        IndexCoverage changed{};
        bool touched = false;
    };

    struct InputRequirementAccumulator {
        BackgroundPropagationWorkspace* owner = nullptr;
        graph_jit::BackgroundPortIndex port = 0;
        IndexCoverage coverage{};
    };

    struct OutputRequirementAccumulator {
        IndexCoverage required{};
    };

    struct NodeCoverageCallData {
        std::vector<graph_jit::BackgroundPortIndex> sample_inputs{};
        std::vector<graph_jit::BackgroundPortIndex> event_inputs{};
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
    std::vector<IndexCoverage> candidate_output_coverages_{};
    std::vector<std::vector<GlobalIndexRange>> candidate_output_region_ranges_{};
    std::vector<InputChangeAccumulator> input_changes_{};
    std::vector<OutputChangeAccumulator> output_changes_{};
    std::vector<InputRequirementAccumulator> input_requirements_{};
    std::vector<OutputRequirementAccumulator> output_requirements_{};
    std::vector<IndexCoverage> input_requirements_by_port_{};
    // Original request positions select logical Regions. Their normalized
    // coverage can overlap other available Regions without selecting them.
    std::vector<IndexCoverage> input_selection_requests_by_port_{};
    std::vector<InputCoverageChange> callback_input_changes_{};
    std::vector<OutputCoverageChange> callback_output_changes_{};
    std::vector<InputCoverageRequirement> callback_input_requirements_{};
    std::vector<OutputCoverageRequirement> callback_output_requirements_{};
    std::vector<NodeCoverageCallData> node_call_data_{};
    std::vector<graph_jit::BackgroundNodeCall> node_calls_{};
    graph_jit::BackgroundEvaluationCall call_{};
    PlanningState* planning_state_ = nullptr;
    std::vector<MetadataRead> forward_reads_{};
    std::vector<MetadataRead> reverse_reads_{};
    std::vector<bool> output_ready_{};
    std::vector<bool> published_outputs_{};
    std::vector<bool> finalized_outputs_{};

    static void before_metadata_read(void*);
    void refresh_input(graph_jit::BackgroundPortIndex);
    [[nodiscard]] bool input_information_ready(graph_jit::BackgroundPortIndex) const;
    [[nodiscard]] CoverageRequestId request_information(void* operation, bool event,
        std::size_t input, CoverageInformation, IndexCoverage const*);
    void initialize_planning();
    void run_planning(bool reverse);
    void mark_forward_pending(graph_jit::BackgroundNodeIndex);
    [[nodiscard]] PreparedCoveragePropagation prepare_discovery(
        graph_jit::BackgroundPortIndex, IndexCoverage const&);


    [[nodiscard]] graph_jit::BackgroundPortPlan const&
    port(graph_jit::BackgroundPortIndex index) const;
    [[nodiscard]] std::size_t
    output_index(graph_jit::BackgroundPortIndex index) const;
    [[nodiscard]] std::size_t
    input_index(graph_jit::BackgroundPortIndex index) const;
    [[nodiscard]] IndexCoverage
    input_coverage(graph_jit::BackgroundPortIndex input) const;
    [[nodiscard]] std::vector<GlobalIndexRange>
    input_regions(graph_jit::BackgroundPortIndex input) const;
    void activate_node(graph_jit::BackgroundNodeIndex node,
                       graph_jit::BackgroundNodeActivity activity);
    void add_input_change(graph_jit::BackgroundPortIndex input,
                          IndexCoverage const& changed, bool coverage_changed);
    void route_output_change(graph_jit::BackgroundPortIndex output,
                             IndexCoverage const& changed, bool coverage_changed);
    void set_input_change(
        graph_jit::BackgroundPortIndex input,
        std::span<GlobalIndexRange const> regions,
        IndexCoverage const& coverage,
        IndexCoverage const& changed);
    void publish_output_coverage(graph_jit::BackgroundPortIndex output,
                                 IndexCoverage const& coverage);
    void publish_output_regions(
        graph_jit::BackgroundPortIndex output,
        std::span<GlobalIndexRange const> regions);
    void publish_output_change(graph_jit::BackgroundPortIndex output,
                               IndexCoverage const& changed);
    void require_output(graph_jit::BackgroundPortIndex output,
                        IndexCoverage const& requested, bool activate_producer);
    void require_input(graph_jit::BackgroundPortIndex input,
                       IndexCoverage const& requested);
    void initialize_calls();
    void reset(BackgroundCoverageState const& coverage);

    static void publish_output_coverage_callback(void* accumulator,
                                                 IndexCoverage const& coverage);
    static void publish_output_regions_callback(
        void* accumulator, GlobalIndexRange const* regions, std::size_t count);
    static void publish_output_change_callback(void* accumulator,
                                               IndexCoverage const& changed);
    static void publish_input_requirement_callback(void* accumulator,
                                                   IndexCoverage const& required);
    // Generated input-union and full forward propagation. Both may also
    // describe live Tick coverage without replaying Tick sample values.
    static void replay_forward_coverage(void*, ReflectedNodeForwardCoverageContext const&);
    static void
    replay_full_forward_coverage(void*,
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
            BackgroundCoverageState const& coverage,
            CoveragePropagationRequest const& request,
            BackgroundCoverageDemandExpansion expansion = {},
            BackgroundCoverageDiscovery discovery = {});
    [[nodiscard]] CoveragePropagationResult
    commit(BackgroundCoverageState& coverage,
           PreparedCoveragePropagation&& prepared) noexcept;
    void discard(PreparedCoveragePropagation&& prepared) noexcept;
};

} // namespace iv
