#pragma once

// Owning port descriptions belong to the retained graph.  This small type is
// also intentionally visible to typed node references for compatibility; it
// is not builder implementation state.

#include <intravenous/graph/port_ids.h>

#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace iv {

constexpr NodeBundlePortId input_port_at(
    NodeBundleHandle node_bundle_handle,
    std::span<PortKind const> order,
    size_t sample_input_count,
    size_t event_input_count,
    size_t position)
{
    if (order.empty()) {
        if (position < sample_input_count) {
            return {node_bundle_handle, PortKind::sample, position};
        }
        position -= sample_input_count;
        if (position < event_input_count) {
            return {node_bundle_handle, PortKind::event, position};
        }
        throw std::out_of_range("positional input is out of bounds");
    }

    if (position >= order.size()) {
        throw std::out_of_range("positional input is out of bounds");
    }
    auto const kind = order[position];
    if (kind != PortKind::sample && kind != PortKind::event) {
        throw std::out_of_range("positional input order is invalid");
    }
    size_t ordinal = 0;
    for (size_t index = 0; index < position; ++index) {
        ordinal += order[index] == kind;
    }
    if ((kind == PortKind::sample && ordinal >= sample_input_count)
        || (kind == PortKind::event && ordinal >= event_input_count)) {
        throw std::out_of_range("positional input order is invalid");
    }
    return {node_bundle_handle, kind, ordinal};
}

struct NodePorts {
    std::vector<SampleInputConfig> sample_inputs {};
    std::vector<SampleOutputConfig> sample_outputs {};
    std::vector<EventInputConfig> event_input_configs {};
    std::vector<EventOutputConfig> event_output_configs {};
    std::vector<PortKind> input_port_order {};

    constexpr NodeBundlePortId input_port_at(
        NodeBundleHandle node_bundle_handle, size_t position) const
    {
        return iv::input_port_at(
            node_bundle_handle,
            input_port_order,
            sample_inputs.size(),
            event_input_configs.size(),
            position);
    }

    constexpr std::vector<SampleInputConfig> const& inputs() const
    {
        return sample_inputs;
    }

    constexpr std::vector<SampleOutputConfig> const& outputs() const
    {
        return sample_outputs;
    }

    constexpr std::vector<EventInputConfig> const& event_inputs() const
    {
        return event_input_configs;
    }

    constexpr std::vector<EventOutputConfig> const& event_outputs() const
    {
        return event_output_configs;
    }
};

} // namespace iv
