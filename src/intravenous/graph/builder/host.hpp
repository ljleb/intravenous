#pragma once

// Host-only inspection of an in-progress builder session. Do not include
// this from dsl.h or an iv module: module code only needs GraphBuilder's
// forwarding surface.

#include <intravenous/graph/builder/state.h>

namespace iv::host {
inline GraphBuilderState::VacantInputs vacant_inputs(GraphBuilder const& builder)
{
    return details::builder_graph_state(
        const_cast<GraphBuilder&>(builder)).vacant_inputs();
}

inline GraphBuilderState::VirtualInputs virtual_inputs(GraphBuilder const& builder)
{
    return details::builder_graph_state(
        const_cast<GraphBuilder&>(builder)).virtual_inputs();
}

inline GraphBuilderState::VirtualSampleInputFamilies
virtual_sample_input_families(GraphBuilder const& builder)
{
    return details::builder_graph_state(
        const_cast<GraphBuilder&>(builder)).virtual_sample_input_families();
}

inline GraphBuilderState::VirtualOutputs virtual_outputs(GraphBuilder const& builder)
{
    return details::builder_graph_state(
        const_cast<GraphBuilder&>(builder)).virtual_outputs();
}

inline GraphBuilderState::VirtualSampleOutputFamilies
virtual_sample_output_families(GraphBuilder const& builder)
{
    return details::builder_graph_state(
        const_cast<GraphBuilder&>(builder)).virtual_sample_output_families();
}

inline GraphBuilderState::VirtualPorts virtual_ports(GraphBuilder const& builder)
{
    return details::builder_graph_state(
        const_cast<GraphBuilder&>(builder)).virtual_ports();
}

inline GraphBuilderPublicSamplePortFamilies
public_sample_input_families(GraphBuilder const& builder)
{
    return details::builder_graph_state(
        const_cast<GraphBuilder&>(builder)).public_sample_input_families();
}

inline bool public_sample_input_is_connected(
    GraphBuilder const& builder, size_t ordinal)
{
    return details::builder_graph_state(
        const_cast<GraphBuilder&>(builder)).public_sample_input_is_connected(ordinal);
}

inline std::vector<GraphBuilderPublicEventInput>
public_event_inputs(GraphBuilder const& builder)
{
    return details::builder_graph_state(
        const_cast<GraphBuilder&>(builder)).public_event_inputs();
}

inline bool public_event_input_is_connected(
    GraphBuilder const& builder, size_t ordinal)
{
    return details::builder_graph_state(
        const_cast<GraphBuilder&>(builder)).public_event_input_is_connected(ordinal);
}

inline std::span<SourceInfo const>
public_event_input_source_infos(GraphBuilder const& builder, size_t ordinal)
{
    return details::builder_graph_state(
        const_cast<GraphBuilder&>(builder)).public_event_input_source_infos(ordinal);
}

inline GraphBuilderPublicSamplePortFamilies
public_sample_output_families(GraphBuilder const& builder)
{
    return details::builder_graph_state(
        const_cast<GraphBuilder&>(builder)).public_sample_output_families();
}

inline std::vector<GraphBuilderPublicEventOutput>
public_event_outputs(GraphBuilder const& builder)
{
    return details::builder_graph_state(
        const_cast<GraphBuilder&>(builder)).public_event_outputs();
}
} // namespace iv::host
