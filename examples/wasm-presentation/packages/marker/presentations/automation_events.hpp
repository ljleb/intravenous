// Generated from the compiler's node-event protocol. Do not edit.
#pragma once
#include <intravenous/presentation/presentation.hpp>

namespace automation_events {
enum class AddPointPointInterpolation : uint8_t {
    empty = static_cast<uint8_t>(0x0ULL),
    hold = static_cast<uint8_t>(0x1ULL),
    bezier = static_cast<uint8_t>(0x2ULL),
};

struct AddPointPoint {
    uint32_t id;
    double seconds;
    float value;
    AddPointPointInterpolation interpolation;
    float tension;
    IvConvertedScalar sample_index;
    uint32_t order;
};

struct AddPoint {
    AddPointPoint point;
};

struct MovePoint {
    uint32_t id;
    double seconds;
    float value;
    IvConvertedScalar sample_index;
};

enum class PlacePointPointInterpolation : uint8_t {
    empty = static_cast<uint8_t>(0x0ULL),
    hold = static_cast<uint8_t>(0x1ULL),
    bezier = static_cast<uint8_t>(0x2ULL),
};

struct PlacePointPoint {
    uint32_t id;
    double seconds;
    float value;
    PlacePointPointInterpolation interpolation;
    float tension;
    IvConvertedScalar sample_index;
    uint32_t order;
};

enum class PlacePointSegmentsElementInterpolation : uint8_t {
    empty = static_cast<uint8_t>(0x0ULL),
    hold = static_cast<uint8_t>(0x1ULL),
    bezier = static_cast<uint8_t>(0x2ULL),
};

struct PlacePointSegmentsElement {
    uint32_t id;
    PlacePointSegmentsElementInterpolation interpolation;
    float tension;
};

struct PlacePoint {
    PlacePointPoint point;
    bool insert;
    ::iv::presentation::Span<PlacePointSegmentsElement const> segments;
    ::iv::presentation::Span<uint32_t const> ordered_ids;
};

struct RemovePoint {
    uint32_t id;
};

enum class SetSegmentInterpolation : uint8_t {
    empty = static_cast<uint8_t>(0x0ULL),
    hold = static_cast<uint8_t>(0x1ULL),
    bezier = static_cast<uint8_t>(0x2ULL),
};

struct SetSegment {
    uint32_t id;
    SetSegmentInterpolation interpolation;
    float tension;
};

struct AcceptedPoints {
    using Value = ::iv::presentation::Span<PlacePointPoint const>;
    static constexpr uint32_t index = 0;
};
}

namespace iv::presentation::detail {
template<> struct EventRecord<::automation_events::AddPointPoint> {
    static bool retain(EditArena const& arena, ::automation_events::AddPointPoint& value)
    {
        return arena.retain(value.sample_index);
    }
};

template<> struct EventRecord<::automation_events::AddPoint> {
    static bool retain(EditArena const& arena, ::automation_events::AddPoint& value)
    {
        return arena.retain(value.point);
    }
};

template<> struct EventRecord<::automation_events::MovePoint> {
    static bool retain(EditArena const& arena, ::automation_events::MovePoint& value)
    {
        return arena.retain(value.sample_index);
    }
};

template<> struct EventRecord<::automation_events::PlacePointPoint> {
    static bool retain(EditArena const& arena, ::automation_events::PlacePointPoint& value)
    {
        return arena.retain(value.sample_index);
    }
};

template<> struct EventRecord<::automation_events::PlacePoint> {
    static bool retain(EditArena const& arena, ::automation_events::PlacePoint& value)
    {
        return arena.retain(value.point) &&
            arena.retain(value.segments) &&
            arena.retain(value.ordered_ids);
    }
};

template<> struct EventBinding<::automation_events::AddPoint> {
    static constexpr uint32_t index = 0;
};

template<> struct EventBinding<::automation_events::MovePoint> {
    static constexpr uint32_t index = 1;
};

template<> struct EventBinding<::automation_events::PlacePoint> {
    static constexpr uint32_t index = 2;
};

template<> struct EventBinding<::automation_events::RemovePoint> {
    static constexpr uint32_t index = 3;
};

template<> struct EventBinding<::automation_events::SetSegment> {
    static constexpr uint32_t index = 4;
};
}

#if defined(__wasm32__)
__asm__(".section .custom_section.iv.presentation.events,\"\",@\n.ascii \"[{\\\"nominalId\\\":\\\"c:$@N@iv@N@automation@S@AddPoint\\\",\\\"definitionFingerprint\\\":\\\"117f10bf8d2ad496\\\",\\\"payload\\\":{\\\"fields\\\":[{\\\"name\\\":\\\"point\\\",\\\"value\\\":{\\\"fields\\\":[{\\\"name\\\":\\\"id\\\",\\\"value\\\":{\\\"bitWidth\\\":32,\\\"kind\\\":\\\"unsignedInteger\\\"}},{\\\"name\\\":\\\"seconds\\\",\\\"value\\\":{\\\"bitWidth\\\":64,\\\"kind\\\":\\\"floatingPoint\\\"}},{\\\"name\\\":\\\"value\\\",\\\"value\\\":{\\\"bitWidth\\\":32,\\\"kind\\\":\\\"floatingPoint\\\"}},{\\\"name\\\":\\\"interpolation\\\",\\\"value\\\":{\\\"bitWidth\\\":8,\\\"enumerators\\\":[{\\\"name\\\":\\\"empty\\\",\\\"value\\\":\\\"0\\\"},{\\\"name\\\":\\\"hold\\\",\\\"value\\\":\\\"1\\\"},{\\\"name\\\":\\\"bezier\\\",\\\"value\\\":\\\"2\\\"}],\\\"kind\\\":\\\"enumeration\\\",\\\"signed\\\":false}},{\\\"name\\\":\\\"tension\\\",\\\"value\\\":{\\\"bitWidth\\\":32,\\\"kind\\\":\\\"floatingPoint\\\"}},{\\\"name\\\":\\\"sample_index\\\",\\\"value\\\":{\\\"kind\\\":\\\"convertedScalar\\\"}},{\\\"name\\\":\\\"order\\\",\\\"value\\\":{\\\"bitWidth\\\":32,\\\"kind\\\":\\\"unsignedInteger\\\"}}],\\\"kind\\\":\\\"aggregate\\\"}}],\\\"kind\\\":\\\"aggregate\\\"}},{\\\"nominalId\\\":\\\"c:$@N@iv@N@automation@S@MovePoint\\\",\\\"definitionFingerprint\\\":\\\"f9e1ca07f921f9ea\\\",\\\"payload\\\":{\\\"fields\\\":[{\\\"name\\\":\\\"id\\\",\\\"value\\\":{\\\"bitWidth\\\":32,\\\"kind\\\":\\\"unsignedInteger\\\"}},{\\\"name\\\":\\\"seconds\\\",\\\"value\\\":{\\\"bitWidth\\\":64,\\\"kind\\\":\\\"floatingPoint\\\"}},{\\\"name\\\":\\\"value\\\",\\\"value\\\":{\\\"bitWidth\\\":32,\\\"kind\\\":\\\"floatingPoint\\\"}},{\\\"name\\\":\\\"sample_index\\\",\\\"value\\\":{\\\"kind\\\":\\\"convertedScalar\\\"}}],\\\"kind\\\":\\\"aggregate\\\"}},{\\\"nominalId\\\":\\\"c:$@N@iv@N@automation@S@PlacePoint\\\",\\\"definitionFingerprint\\\":\\\"625a00abfdca9fc6\\\",\\\"payload\\\":{\\\"fields\\\":[{\\\"name\\\":\\\"point\\\",\\\"value\\\":{\\\"fields\\\":[{\\\"name\\\":\\\"id\\\",\\\"value\\\":{\\\"bitWidth\\\":32,\\\"kind\\\":\\\"unsignedInteger\\\"}},{\\\"name\\\":\\\"seconds\\\",\\\"value\\\":{\\\"bitWidth\\\":64,\\\"kind\\\":\\\"floatingPoint\\\"}},{\\\"name\\\":\\\"value\\\",\\\"value\\\":{\\\"bitWidth\\\":32,\\\"kind\\\":\\\"floatingPoint\\\"}},{\\\"name\\\":\\\"interpolation\\\",\\\"value\\\":{\\\"bitWidth\\\":8,\\\"enumerators\\\":[{\\\"name\\\":\\\"empty\\\",\\\"value\\\":\\\"0\\\"},{\\\"name\\\":\\\"hold\\\",\\\"value\\\":\\\"1\\\"},{\\\"name\\\":\\\"bezier\\\",\\\"value\\\":\\\"2\\\"}],\\\"kind\\\":\\\"enumeration\\\",\\\"signed\\\":false}},{\\\"name\\\":\\\"tension\\\",\\\"value\\\":{\\\"bitWidth\\\":32,\\\"kind\\\":\\\"floatingPoint\\\"}},{\\\"name\\\":\\\"sample_index\\\",\\\"value\\\":{\\\"kind\\\":\\\"convertedScalar\\\"}},{\\\"name\\\":\\\"order\\\",\\\"value\\\":{\\\"bitWidth\\\":32,\\\"kind\\\":\\\"unsignedInteger\\\"}}],\\\"kind\\\":\\\"aggregate\\\"}},{\\\"name\\\":\\\"insert\\\",\\\"value\\\":{\\\"kind\\\":\\\"boolean\\\"}},{\\\"name\\\":\\\"segments\\\",\\\"value\\\":{\\\"element\\\":{\\\"fields\\\":[{\\\"name\\\":\\\"id\\\",\\\"value\\\":{\\\"bitWidth\\\":32,\\\"kind\\\":\\\"unsignedInteger\\\"}},{\\\"name\\\":\\\"interpolation\\\",\\\"value\\\":{\\\"bitWidth\\\":8,\\\"enumerators\\\":[{\\\"name\\\":\\\"empty\\\",\\\"value\\\":\\\"0\\\"},{\\\"name\\\":\\\"hold\\\",\\\"value\\\":\\\"1\\\"},{\\\"name\\\":\\\"bezier\\\",\\\"value\\\":\\\"2\\\"}],\\\"kind\\\":\\\"enumeration\\\",\\\"signed\\\":false}},{\\\"name\\\":\\\"tension\\\",\\\"value\\\":{\\\"bitWidth\\\":32,\\\"kind\\\":\\\"floatingPoint\\\"}}],\\\"kind\\\":\\\"aggregate\\\"},\\\"kind\\\":\\\"sequence\\\"}},{\\\"name\\\":\\\"ordered_ids\\\",\\\"value\\\":{\\\"element\\\":{\\\"bitWidth\\\":32,\\\"kind\\\":\\\"unsignedInteger\\\"},\\\"kind\\\":\\\"sequence\\\"}}],\\\"kind\\\":\\\"aggregate\\\"}},{\\\"nominalId\\\":\\\"c:$@N@iv@N@automation@S@RemovePoint\\\",\\\"definitionFingerprint\\\":\\\"c1b7cfa2c11beeb0\\\",\\\"payload\\\":{\\\"fields\\\":[{\\\"name\\\":\\\"id\\\",\\\"value\\\":{\\\"bitWidth\\\":32,\\\"kind\\\":\\\"unsignedInteger\\\"}}],\\\"kind\\\":\\\"aggregate\\\"}},{\\\"nominalId\\\":\\\"c:$@N@iv@N@automation@S@SetSegment\\\",\\\"definitionFingerprint\\\":\\\"e8fdd3f15690b14c\\\",\\\"payload\\\":{\\\"fields\\\":[{\\\"name\\\":\\\"id\\\",\\\"value\\\":{\\\"bitWidth\\\":32,\\\"kind\\\":\\\"unsignedInteger\\\"}},{\\\"name\\\":\\\"interpolation\\\",\\\"value\\\":{\\\"bitWidth\\\":8,\\\"enumerators\\\":[{\\\"name\\\":\\\"empty\\\",\\\"value\\\":\\\"0\\\"},{\\\"name\\\":\\\"hold\\\",\\\"value\\\":\\\"1\\\"},{\\\"name\\\":\\\"bezier\\\",\\\"value\\\":\\\"2\\\"}],\\\"kind\\\":\\\"enumeration\\\",\\\"signed\\\":false}},{\\\"name\\\":\\\"tension\\\",\\\"value\\\":{\\\"bitWidth\\\":32,\\\"kind\\\":\\\"floatingPoint\\\"}}],\\\"kind\\\":\\\"aggregate\\\"}}]\"\n.text\n");
__asm__(".section .custom_section.iv.presentation.arguments,\"\",@\n.ascii \"[{\\\"definitionId\\\":\\\"builtin.automation.curve\\\",\\\"index\\\":0,\\\"value\\\":{\\\"kind\\\":\\\"sequence\\\",\\\"element\\\":{\\\"fields\\\":[{\\\"name\\\":\\\"id\\\",\\\"value\\\":{\\\"bitWidth\\\":32,\\\"kind\\\":\\\"unsignedInteger\\\"}},{\\\"name\\\":\\\"seconds\\\",\\\"value\\\":{\\\"bitWidth\\\":64,\\\"kind\\\":\\\"floatingPoint\\\"}},{\\\"name\\\":\\\"value\\\",\\\"value\\\":{\\\"bitWidth\\\":32,\\\"kind\\\":\\\"floatingPoint\\\"}},{\\\"name\\\":\\\"interpolation\\\",\\\"value\\\":{\\\"bitWidth\\\":8,\\\"enumerators\\\":[{\\\"name\\\":\\\"empty\\\",\\\"value\\\":\\\"0\\\"},{\\\"name\\\":\\\"hold\\\",\\\"value\\\":\\\"1\\\"},{\\\"name\\\":\\\"bezier\\\",\\\"value\\\":\\\"2\\\"}],\\\"kind\\\":\\\"enumeration\\\",\\\"signed\\\":false}},{\\\"name\\\":\\\"tension\\\",\\\"value\\\":{\\\"bitWidth\\\":32,\\\"kind\\\":\\\"floatingPoint\\\"}},{\\\"name\\\":\\\"sample_index\\\",\\\"value\\\":{\\\"kind\\\":\\\"convertedScalar\\\"}},{\\\"name\\\":\\\"order\\\",\\\"value\\\":{\\\"bitWidth\\\":32,\\\"kind\\\":\\\"unsignedInteger\\\"}}],\\\"kind\\\":\\\"aggregate\\\"}}}]\"\n.text\n");
#endif
