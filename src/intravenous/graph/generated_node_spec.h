#pragma once

#include <intravenous/graph/connection_node.h>

#include <array>
#include <string>
#include <variant>
#include <vector>

namespace iv {
struct RuntimeSampleInputNodeSpec {
    OutputConfig output {};
    Sample default_value = 0.0f;
    std::string binding_id {};

    constexpr auto outputs() const
    {
        return std::array<OutputConfig, 1>{ output };
    }
};

struct RuntimeEventInputNodeSpec {
    EventTypeId type = EventTypeId::empty;
    std::string binding_id {};

    constexpr auto event_outputs() const
    {
        return std::array<EventOutputConfig, 1>{{ { .type = type } }};
    }
};

struct RuntimeSampleOutputNodeSpec {
    InputConfig input {};
    std::string binding_id {};

    constexpr auto inputs() const
    {
        return std::array<InputConfig, 1>{ input };
    }
};

struct RuntimeEventOutputNodeSpec {
    EventTypeId type = EventTypeId::empty;
    std::string binding_id {};

    constexpr auto event_inputs() const
    {
        return std::array<EventInputConfig, 1>{{ { .type = type } }};
    }
};

struct RuntimeSampleOutputFamilyNodeSpec {
    std::vector<InputConfig> input_configs {};
    std::vector<std::string> member_binding_ids {};
    std::string aggregate_binding_id {};

    constexpr std::vector<InputConfig> const& inputs() const
    {
        return input_configs;
    }
};

struct RuntimeEventOutputFamilyNodeSpec {
    EventTypeId type = EventTypeId::empty;
    size_t member_count = 0;
    std::vector<std::string> member_binding_ids {};
    std::string aggregate_binding_id {};

    constexpr std::vector<EventInputConfig> event_inputs() const
    {
        return std::vector<EventInputConfig>(
            member_count,
            EventInputConfig{ .type = type });
    }
};

using GeneratedNodeSpec = std::variant<
    std::monostate,
    ConnectionNodeSpec,
    RuntimeSampleInputNodeSpec,
    RuntimeEventInputNodeSpec,
    RuntimeSampleOutputNodeSpec,
    RuntimeEventOutputNodeSpec,
    RuntimeSampleOutputFamilyNodeSpec,
    RuntimeEventOutputFamilyNodeSpec>;
}
