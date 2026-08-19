#pragma once

#include <intravenous/graph/builder/port_refs.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace iv {
    // This describes how a concrete public sample port participates in a
    // public declaration. It deliberately lives beside, rather than inside,
    // OutputConfig: OutputConfig describes one executable port, whose ordinal
    // is its position in the config array.
    struct PublicSamplePortMember {
        std::string family_name {};
        ChannelTypeId channel_type = ChannelTypeId::mono;
        size_t channel_index = 0;
        bool whole_stream = false;
    };

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
        PublicSamplePortMember public_member {};
        // When set, this declaration contributes only one semantic channel to
        // the wider destination port. Completion decides how to materialize
        // the aggregate for the current runtime representation.
        std::optional<size_t> target_channel_ordinal {};
    };

    struct EventOutputRefConfig {
        EventPortRef ref;
        EventOutputConfig config;
    };
}
