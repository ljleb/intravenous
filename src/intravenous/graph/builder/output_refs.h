#pragma once

#include <intravenous/graph/builder/port_refs.h>

#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
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

    // Module-facing output declarations carry only borrowed views and fixed
    // size handles. libiv_builder copies the strings and materializes the
    // owning OutputConfig records before the call returns.
    struct SampleOutputRequest {
        SamplePortRef ref;
        std::string_view name;
        ChannelLayout channel_layout;
        std::string_view family_name;
        ChannelTypeId family_channel_type = ChannelTypeId::mono;
        bool whole_stream = true;
        size_t target_channel_ordinal = std::numeric_limits<size_t>::max();

        constexpr bool targets_single_channel() const {
            return target_channel_ordinal != std::numeric_limits<size_t>::max();
        }
    };

    struct EventOutputRequest {
        EventPortRef ref;
        std::string_view name;
    };

    static_assert(std::is_trivially_copyable_v<SampleOutputRequest>);
    static_assert(std::is_trivially_copyable_v<EventOutputRequest>);
}
