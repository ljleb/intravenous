#pragma once

#include <intravenous/graph_jit/graph_jit.h>

#include <span>

namespace iv::graph_jit {

// Metadata shared by a steady lowering plan and materialized Tick code.
struct RealtimeCodeConfiguration {
    std::optional<std::uint64_t> configured_generation;
    std::uint64_t definitions_generation;
    GraphJitKernelSpecialization const& specialization;
    ConfiguredGraph const* configured_graph;
    ConfiguredBundleInventory configured_bundles;
};

struct RealtimeCodeLayout {
    RealtimeCodeConfiguration configuration;
    NodeLayout const& node_layout;
    BackgroundEvaluationPlan const& background_evaluation_plan;
    RealtimePortStateRequirements const& realtime_port_state_requirements;
    RealtimePortStateRealizations const& realtime_port_state_realizations;
    std::span<ManualControlRuntimeBinding const> manual_controls;
};

[[nodiscard]] RealtimeCodeConfiguration realtime_code_configuration(CompiledGraph const&) noexcept;
[[nodiscard]] RealtimeCodeLayout realtime_code_layout(CompiledGraph const&) noexcept;
[[nodiscard]] bool same_audio_configuration(
    CompiledGraph const& previous, RealtimeCodeConfiguration const& successor) noexcept;
[[nodiscard]] bool compatible_steady_realtime_layout(
    CompiledGraph const& structural, RealtimeCodeLayout const& replacement) noexcept;
[[nodiscard]] bool can_reuse_observation_layout(
    CompiledGraph const& structural, CompiledGraph const& active_code,
    RealtimeCodeLayout const& replacement) noexcept;

} // namespace iv::graph_jit
