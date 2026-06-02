#pragma once

#include "port_refs.h"

#include <string_view>
#include <variant>

namespace iv {
    struct NamedRef {
        std::string_view name;
        std::variant<SamplePortRef, Sample, EventPortRef> value;

        NamedRef(std::string_view name, SamplePortRef sample_port): name(name), value(sample_port) {}
        NamedRef(std::string_view name, Sample sample): name(name), value(sample) {}
        NamedRef(std::string_view name, EventPortRef event): name(name), value(event) {}
    };

    struct OutputRefConfig {
        SamplePortRef ref;
        OutputConfig config;
    };

    struct EventOutputRefConfig {
        EventPortRef ref;
        EventOutputConfig config;
    };
}
