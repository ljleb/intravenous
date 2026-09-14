#pragma once

// Owning port descriptions belong to the retained graph.  This small type is
// also intentionally visible to typed node references for compatibility; it
// is not builder implementation state.

#include <intravenous/ports.h>

#include <vector>

namespace iv {

struct NodePorts {
    std::vector<SampleInputConfig> sample_inputs {};
    std::vector<SampleOutputConfig> sample_outputs {};
    std::vector<EventInputConfig> event_input_configs {};
    std::vector<EventOutputConfig> event_output_configs {};

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
