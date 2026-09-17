#pragma once

#include <intravenous/graph/configured_graph.hpp>
#include <intravenous/node/layout.h>
#include <intravenous/runtime/node_definition_types.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace iv {

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

// Generated zero-input/zero-output root-node execution ABI. Mutable bytes are
// owned by GraphExecutor through NodeStorage created from CompiledGraph::node_layout.
// Generated code receives only that storage base plus execution coordinates;
// lifecycle remains entirely in the ordinary NodeStorage machinery.
using CompiledGraphBlockFunction =
    void (*)(std::byte* storage_base, std::size_t sample_index, std::size_t block_size);

struct CompiledGraphRootOperations {
    CompiledGraphBlockFunction tick_block = nullptr;
    // Null means the generated root cannot legally skip a block.
    CompiledGraphBlockFunction skip_block = nullptr;

    [[nodiscard]] bool valid() const noexcept
    {
        return tick_block != nullptr;
    }

    [[nodiscard]] bool can_skip_block() const noexcept
    {
        return skip_block != nullptr;
    }
};

// One immutable native project generation. GraphJit owns code/layout/planning
// metadata only; GraphExecutor creates NodeStorage from node_layout and owns all
// mutable state plus initialize/move/release lifecycle. Generated code remains
// pinned by code_lifetime.
struct CompiledGraph {
    std::uint64_t project_generation = 0;
    std::uint64_t definitions_generation = 0;
    GraphJitKernelSpecialization specialization{};
    std::shared_ptr<ConfiguredGraph const> configured_graph{};
    std::vector<std::shared_ptr<PackageRevision const>> package_revisions{};
    NodeLayout node_layout{};
    CompiledGraphRootOperations root_operations{};
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
