#pragma once

#include <intravenous/graph/configured_graph.hpp>
#include <intravenous/runtime/node_definition_types.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace iv {
struct ResourceContext;

struct GraphJitConfig {
    std::size_t sample_rate = 48000;
    std::size_t block_size = 256;
};

// Immutable specialization facts for one project kernel. CPU/features are
// captured by GraphJit from the host used to create its persistent ORC domain.
struct GraphJitKernelSpecialization {
    std::size_t sample_rate = 0;
    std::size_t block_size = 0;
    std::string target_triple{};
    std::string target_cpu{};
    std::string target_features{};
};

enum class GraphJitDiagnosticStage : std::uint8_t {
    input_capture,
    package_llvm,
    lowering,
    optimization,
    materialization,
};

struct GraphJitDiagnostic {
    GraphJitDiagnosticStage stage = GraphJitDiagnosticStage::input_capture;
    std::string message{};
    std::size_t node_bundle = static_cast<std::size_t>(-1);
    std::string node_type_id{};
    std::string package_root{};
};

// The lowerer owns the concrete offsets. GraphExecutor owns allocations with
// these requirements. Immutable tables/constants used only by generated code
// belong in the JIT module rather than in a second host-side object graph.
struct CompiledGraphStorageRequirements {
    std::size_t size = 0;
    std::size_t alignment = 1;
};

// Persistent state correspondence retained outside generated code. This is the
// minimum host-visible state map needed for future activation/migration logic;
// all other execution-local layout is private to the lowerer-generated module.
struct CompiledGraphNodeStorageLayout {
    std::size_t node_bundle = 0;
    std::optional<std::size_t> state_offset{};
    std::size_t state_size = 0;
    std::size_t state_alignment = 1;
    std::optional<std::size_t> compiled_state_offset{};
    std::size_t compiled_state_size = 0;
    std::size_t compiled_state_alignment = 1;
};

struct CompiledGraphRuntimePlan {
    // Mutable bytes retained across calls/generations while this compiled graph
    // is active. Node State/CompiledState, history, feedback, activity, etc. live
    // here according to the lowerer's plan.
    CompiledGraphStorageRequirements persistent_storage{};

    // Caller-owned transient bytes reusable between block calls. They are raw
    // scratch: no generated initialize/release lifecycle is required.
    CompiledGraphStorageRequirements scratch_storage{};

    std::vector<CompiledGraphNodeStorageLayout> node_storage{};
};

// Stable native ABI between GraphExecutor and one generated project module.
// block_size remains an argument even though GraphJit specializes for its
// configured maximum/quantum; this keeps final partial-block execution legal.
using CompiledGraphLifecycleFunction =
    void (*)(std::byte* persistent_storage, ResourceContext const* resources);
using CompiledGraphBlockFunction =
    void (*)(
        std::byte* persistent_storage,
        std::byte* scratch_storage,
        ResourceContext const* resources,
        std::size_t sample_index,
        std::size_t block_size);

struct CompiledGraphEntrypoints {
    CompiledGraphLifecycleFunction initialize = nullptr;
    CompiledGraphLifecycleFunction release = nullptr;
    CompiledGraphBlockFunction tick_block = nullptr;
    CompiledGraphBlockFunction skip_block = nullptr;

    [[nodiscard]] bool valid() const noexcept
    {
        return initialize != nullptr && release != nullptr
            && tick_block != nullptr && skip_block != nullptr;
    }
};

// One immutable native project generation. Generated code and immutable
// lowering data are pinned by code_lifetime; GraphExecutor owns mutable storage
// satisfying runtime_plan and invokes only these resolved entrypoints.
struct CompiledGraph {
    std::uint64_t project_generation = 0;
    std::uint64_t definitions_generation = 0;
    GraphJitKernelSpecialization specialization{};
    std::shared_ptr<ConfiguredGraph const> configured_graph{};
    std::vector<std::shared_ptr<PackageRevision const>> package_revisions{};
    CompiledGraphRuntimePlan runtime_plan{};
    CompiledGraphEntrypoints entrypoints{};
    std::shared_ptr<void const> code_lifetime{};
};

struct GraphJitCompileRequest {
    std::uint64_t project_generation = 0;
    std::shared_ptr<ConfiguredGraph const> graph{};
    std::shared_ptr<NodeDefinitionsSnapshot const> definitions{};
};

struct GraphJitCompileResult {
    // False only for an unbound singleton bridge. Once GraphJit receives a
    // request, expected compiler failures are represented by diagnostics.
    bool attempted = false;
    std::shared_ptr<CompiledGraph const> compiled_graph{};
    std::vector<GraphJitDiagnostic> diagnostics{};

    [[nodiscard]] bool succeeded() const noexcept
    {
        return compiled_graph != nullptr;
    }
};

class GraphJit {
    class Impl;
    std::unique_ptr<Impl> impl_;

public:
    explicit GraphJit(GraphJitConfig config = {});
    ~GraphJit();
    GraphJit(GraphJit&&) noexcept;
    GraphJit& operator=(GraphJit&&) noexcept;

    GraphJit(GraphJit const&) = delete;
    GraphJit& operator=(GraphJit const&) = delete;

    [[nodiscard]] GraphJitKernelSpecialization const& specialization() const noexcept;
    [[nodiscard]] GraphJitCompileResult compile(GraphJitCompileRequest const& request);

    [[nodiscard]] GraphJitCompileResult handle_project_graph_transaction(
        GraphJitCompileRequest const& request);
};
} // namespace iv
