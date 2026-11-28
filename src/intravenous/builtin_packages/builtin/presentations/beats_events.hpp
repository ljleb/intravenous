// Generated from the compiler's node-event protocol. Do not edit.
#pragma once
#include <intravenous/presentation/presentation.hpp>

namespace presentation_beats_events {
struct SetBeatGrid {
    double bpm;
    uint32_t beats_per_bar;
    ::iv::presentation::Span<uint32_t const> subdivisions;
};
}

namespace iv::presentation::detail {
template<> struct EventRecord<::presentation_beats_events::SetBeatGrid> {
    static bool retain(EditArena const& arena, ::presentation_beats_events::SetBeatGrid& value)
    {
        return arena.retain(value.subdivisions);
    }
};

template<> struct EventBinding<::presentation_beats_events::SetBeatGrid> {
    static constexpr uint32_t index = 0;
};
}

#if defined(__wasm32__)
__asm__(".section .custom_section.iv.presentation.events,\"\",@\n.ascii \"[{\\\"nominalId\\\":\\\"c:$@N@iv@S@SetBeatGrid\\\",\\\"definitionFingerprint\\\":\\\"b2a36c5cb2dc68f3\\\",\\\"payload\\\":{\\\"fields\\\":[{\\\"name\\\":\\\"bpm\\\",\\\"value\\\":{\\\"bitWidth\\\":64,\\\"kind\\\":\\\"floatingPoint\\\"}},{\\\"name\\\":\\\"beats_per_bar\\\",\\\"value\\\":{\\\"bitWidth\\\":32,\\\"kind\\\":\\\"unsignedInteger\\\"}},{\\\"name\\\":\\\"subdivisions\\\",\\\"value\\\":{\\\"element\\\":{\\\"bitWidth\\\":32,\\\"kind\\\":\\\"unsignedInteger\\\"},\\\"kind\\\":\\\"sequence\\\"}}],\\\"kind\\\":\\\"aggregate\\\"}}]\"\n.text\n");
#endif
