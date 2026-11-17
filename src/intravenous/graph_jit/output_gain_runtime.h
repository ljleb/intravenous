#pragma once

#include <intravenous/graph/reflected_node_operations.h>
#include <intravenous/node/layout.h>

namespace iv::graph_jit::detail {
struct OutputGainConfiguration {
    Sample gain = 1.0f;
};

inline void forward_output_gain(void const*, ReflectedNodeForwardCoverageContext const& context)
{
    auto const& input = context.inputs.data()[0];
    auto const& output = context.outputs.data()[0];
    output.publish_regions(input.regions());
    output.change(context.local_state_changed ? input.coverage() : input.changed());
}
inline void reverse_output_gain(void const*, ReflectedNodeReverseCoverageContext const& context)
{
    context.inputs.data()[0].require(context.outputs.data()[0].required());
}
inline void tock_output_gain(void const* data, ReflectedNodeTockCoverageContext const& context)
{
    auto const factor = static_cast<OutputGainConfiguration const*>(data)->gain;
    auto const& input = context.inputs.data()[0];
    auto const& output = context.outputs.data()[0];
    auto const scale = continuous_value_type_info(output.geometry.value_type).apply_output_gain;
    auto const sources = input.region_views(), targets = output.region_views();
    IV_ASSERT(sources.size() == targets.size(), "gain lost the logical Region correspondence");
    for (std::size_t region = 0; region < targets.size(); ++region) {
        auto process = [&](auto&& self, ContinuousRegionStorageView const& window) -> void {
            if (!window.storage) { for (auto const& part : window.windows) self(self, part); return; }
            for (std::uint64_t frame = 0; frame < window.range.extent; ++frame) {
                auto const position = window.range.begin.advanced_by(frame);
                for (std::size_t channel = 0; channel < channel_count(window.channel_layout); ++channel) {
                    auto const* source = sources[region].value_storage(position, channel);
                    auto* target = window.value_storage(position, channel, true);
                    IV_ASSERT(source && target, "gain read lies outside selected prepared contents");
                    scale(target, source, output.geometry.elements_per_value, factor);
                }
            }
        };
        process(process, targets[region]);
    }
}
inline std::size_t declare_output_gain(void const*, NodeStateStructures const*, NodeLayoutBuilder& builder,
    ReflectedNodePortRealization const&)
{
    return iv::details::register_node(builder, {.node_type_name = "Output gain"});
}
inline void tock_output_gain_batch(ReflectedNodeTockCoverageBatchContext const& batch)
{
    for (auto const& lane : std::span<ReflectedNodeTockCoverageBatchLane const>{batch.lanes})
        tock_output_gain(lane.node_data, lane.context);
}
inline void forward_output_gain_batch(ReflectedNodeForwardCoverageBatchContext const& batch)
{
    for (auto const& lane : std::span<ReflectedNodeForwardCoverageBatchLane const>{batch.lanes})
        forward_output_gain(lane.node_data, lane.context);
}
inline void reverse_output_gain_batch(ReflectedNodeReverseCoverageBatchContext const& batch)
{
    for (auto const& lane : std::span<ReflectedNodeReverseCoverageBatchLane const>{batch.lanes})
        reverse_output_gain(lane.node_data, lane.context);
}
} // namespace iv::graph_jit::detail
