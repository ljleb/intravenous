#pragma once

#include <intravenous/coverage.h>
#include <intravenous/node/resources.h>
#include <intravenous/runtime/graph_jit.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace iv {

enum class GraphExecutorStageResult : std::uint8_t {
    staged,
    ignored_stale,
};

// Generation-local requests for one exact coverage-propagation operation. Port
// and node indices come from the active CompiledGraph's BackgroundEvaluationPlan.
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

// Propagation deliberately returns coverage changes and requirements, not sample
// or event data. Background evaluation consumes these requirements before a
// candidate page version can be published.
struct CoveragePropagationResult {
    std::vector<PropagatedOutputChange> output_changes{};
    std::vector<RequiredInputCoverage> input_requirements{};
    std::vector<RequiredOutputCoverage> output_requirements{};
};

// Mutable runtime owner for immutable CompiledGraph generations. Staging and
// activation are control-path operations: callers must activate only at a legal
// whole-root boundary with no concurrent tick_block() invocation. The realtime
// call itself performs no generation selection, allocation, or lifecycle work.
class GraphExecutor {
    class CoveragePropagationState;

    struct InputChangeAccumulator {
        Coverage coverage{};
        Coverage changed{};
    };

    struct OutputChangeAccumulator {
        CoveragePropagationState* owner = nullptr;
        graph_jit::BackgroundPortIndex port = 0;
        Coverage previous_coverage{};
        Coverage changed{};
        bool touched = false;
    };

    struct InputRequirementAccumulator {
        CoveragePropagationState* owner = nullptr;
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

    class CoveragePropagationState {
        graph_jit::BackgroundEvaluationPlan const* plan_ = nullptr;
        std::size_t sample_rate_ = 0;
        std::vector<Coverage> propagated_output_coverages_{};
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

        [[nodiscard]] graph_jit::BackgroundPortPlan const& port(
            graph_jit::BackgroundPortIndex index) const;
        [[nodiscard]] std::size_t output_index(
            graph_jit::BackgroundPortIndex index) const;
        [[nodiscard]] std::size_t input_index(
            graph_jit::BackgroundPortIndex index) const;
        [[nodiscard]] Coverage input_coverage(
            graph_jit::BackgroundPortIndex input) const;
        void activate_node(
            graph_jit::BackgroundNodeIndex node,
            graph_jit::BackgroundNodeActivity activity);
        void add_input_change(
            graph_jit::BackgroundPortIndex input,
            Coverage const& changed,
            bool coverage_changed);
        void route_output_change(
            graph_jit::BackgroundPortIndex output,
            Coverage const& changed,
            bool coverage_changed);
        void set_input_change(
            graph_jit::BackgroundPortIndex input,
            Coverage const& coverage,
            Coverage const& changed);
        void publish_output_coverage(
            graph_jit::BackgroundPortIndex output,
            Coverage const& coverage);
        void publish_output_change(
            graph_jit::BackgroundPortIndex output,
            Coverage const& changed);
        void require_output(
            graph_jit::BackgroundPortIndex output,
            Coverage const& requested,
            bool activate_producer);
        void require_input(
            graph_jit::BackgroundPortIndex input,
            Coverage const& requested);
        void initialize_calls();
        void reset();

        static void publish_output_coverage_callback(
            void* accumulator,
            Coverage const& coverage);
        static void publish_output_change_callback(
            void* accumulator,
            Coverage const& changed);
        static void publish_input_requirement_callback(
            void* accumulator,
            Coverage const& required);
        static void replay_forward_coverage(
            void*, ReflectedNodeForwardCoverageContext const& context);
        static void replay_reverse_coverage(
            void*, ReflectedNodeReverseCoverageContext const& context);

    public:
        CoveragePropagationState() = default;
        CoveragePropagationState(
            graph_jit::BackgroundEvaluationPlan const& plan,
            std::size_t sample_rate);

        [[nodiscard]] CoveragePropagationResult propagate(
            CompiledGraphBackgroundOperations const& operations,
            std::byte* storage,
            CoveragePropagationRequest const& request);
    };

    struct Realization {
        std::shared_ptr<CompiledGraph const> graph{};
        NodeStorage storage{};
        CoveragePropagationState coverage{};
        bool initialized = false;

        Realization(
            std::shared_ptr<CompiledGraph const> graph,
            ResourceContext const& resources);
    };

    ResourceContext resources_{};
    std::array<std::optional<Realization>, 2> realizations_{};
    std::optional<std::size_t> active_{};
    std::optional<std::size_t> pending_{};

    [[nodiscard]] Realization& active_realization();
    [[nodiscard]] Realization const& active_realization() const;

public:
    explicit GraphExecutor(ResourceContext resources = {});
    ~GraphExecutor() = default;

    GraphExecutor(GraphExecutor const&) = delete;
    GraphExecutor& operator=(GraphExecutor const&) = delete;
    GraphExecutor(GraphExecutor&&) = delete;
    GraphExecutor& operator=(GraphExecutor&&) = delete;

    // Builds a pending runtime realization without reading mutable active
    // storage. A newer pending generation supersedes an older pending generation
    // without disturbing the active one.
    GraphExecutorStageResult stage(
        std::shared_ptr<CompiledGraph const> compiled_graph);

    // At the caller-provided quiescent boundary, snapshots/migrates the final
    // active state into the pending realization and publishes it. Returns false
    // when no generation is pending.
    bool activate_pending();

    [[nodiscard]] std::optional<std::uint64_t> active_generation() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> pending_generation() const noexcept;
    [[nodiscard]] std::shared_ptr<CompiledGraph const> active_graph() const noexcept;

    // Runs one background-only exact F/R batch against the active generation.
    // Calls are serialized by the owner and must not overlap activation. The
    // propagated semantic-coverage baseline advances only after both generated
    // traversals return successfully; no Tock/replay data evaluation happens
    // here and no published page version advances.
    [[nodiscard]] CoveragePropagationResult propagate_coverage(
        CoveragePropagationRequest const& request);

    // Executes only the already-active realization. Generation activation is
    // deliberately never hidden in this audio-thread entry point.
    void tick_block(std::size_t sample_index, std::size_t block_size);
};

} // namespace iv
