#pragma once

#include "automation_events.hpp"
#include <intravenous/presentation/state_json.h>

namespace automation_presentation {
using namespace iv::presentation;
using namespace automation_events;
using Interpolation = PlacePointPointInterpolation;

constexpr double pi = 3.14159265358979323846;
constexpr double sample_cycle = 18446744073709551616.0;

template<class T> constexpr T smaller(T a, T b) { return a < b ? a : b; }
template<class T> constexpr T larger(T a, T b) { return a > b ? a : b; }
template<class T> constexpr T clamp(T value, T low, T high) { return larger(low, smaller(high, value)); }
inline uint32_t prior(uint32_t slot, uint32_t size) { return slot ? slot - 1 : size - 1; }

template<class T> void swap_values(T& a, T& b)
{
    T value = a;
    a = b;
    b = value;
}

// Presentation artifacts are freestanding. Session storage uses the same
// allocator as the guest runtime and retains capacity across frames.
template<class T>
class Array {
    static_assert(__is_trivially_copyable(T) && alignof(T) <= 8);
    T* storage_ = nullptr;
    uint32_t size_ = 0;
    uint32_t capacity_ = 0;

public:
    Array() = default;
    ~Array() { iv_session_free(storage_); }
    Array(Array const&) = delete;
    Array& operator=(Array const&) = delete;

    uint32_t size() const { return size_; }
    T* begin() { return storage_; }
    T const* begin() const { return storage_; }
    T* end() { return size_ ? storage_ + size_ : storage_; }
    T const* end() const { return size_ ? storage_ + size_ : storage_; }
    T& operator[](uint32_t i) { return storage_[i]; }
    T const& operator[](uint32_t i) const { return storage_[i]; }
    void clear() { size_ = 0; }

    bool reserve(uint32_t count)
    {
        if (count <= capacity_) return true;
        if (count > UINT32_MAX / sizeof(T)) return false;
        auto const next_capacity = larger(count, smaller<uint32_t>(UINT32_MAX / sizeof(T), larger(16u, capacity_ + capacity_ / 2)));
        auto* next = static_cast<T*>(iv_session_allocate(next_capacity * sizeof(T)));
        if (!next) return false;
        for (uint32_t i = 0; i < size_; ++i) next[i] = storage_[i];
        iv_session_free(storage_);
        storage_ = next;
        capacity_ = next_capacity;
        return true;
    }

    bool push(T const& value)
    {
        if (!reserve(size_ + 1)) return false;
        storage_[size_++] = value;
        return true;
    }

    void pop() { --size_; }

    template<class Less>
    void sort(Less less)
    {
        // In-place heap sort keeps an upper bound even for reverse-ordered
        // inputs and does not use the guest stack to store partitions.
        auto sift = [&](uint32_t root, uint32_t size) {
            while (root < size / 2) {
                uint32_t child = 2 * root + 1;
                if (child + 1 < size && less(storage_[child], storage_[child + 1])) ++child;
                if (!less(storage_[root], storage_[child])) break;
                swap_values(storage_[root], storage_[child]);
                root = child;
            }
        };
        for (uint32_t i = size_ / 2; i; --i) sift(i - 1, size_);
        for (uint32_t i = size_; i > 1; --i) {
            swap_values(storage_[0], storage_[i - 1]);
            sift(0, i - 1);
        }
    }
};

template<size_t N> constexpr IvString text(char const (&value)[N]) { return {value, N - 1}; }
inline bool equal(IvString left, IvString right)
{
    if (left.size != right.size) return false;
    for (uint32_t i = 0; i < left.size; ++i)
        if (left.data[i] != right.data[i]) return false;
    return true;
}

struct Label {
    char bytes[112]{};
    uint32_t size = 0;

    void append(IvString value)
    {
        for (uint32_t i = 0; i < value.size && size < sizeof(bytes); ++i) bytes[size++] = value.data[i];
    }
    void integer(int64_t value)
    {
        char reverse[21];
        uint32_t count = 0;
        uint64_t remaining = static_cast<uint64_t>(value);
        if (value < 0) { append(text("-")); remaining = 0 - remaining; }
        do {
            reverse[count++] = static_cast<char>('0' + remaining % 10);
            remaining /= 10;
        } while (remaining);
        while (count) append({&reverse[--count], 1});
    }
    void decimal(double value, uint32_t places = 3)
    {
        if (!__builtin_isfinite(value)) return;
        if (value < 0) { append(text("-")); value = -value; }
        int exponent = 0;
        while (value >= 1e12) { value /= 10; ++exponent; }
        while (value > 0 && value < .001) { value *= 10; --exponent; }
        uint32_t scale = 1;
        for (uint32_t i = 0; i < places; ++i) scale *= 10;
        auto const scaled = static_cast<int64_t>(value * scale + .5);
        integer(scaled / scale);
        auto fraction = static_cast<uint32_t>(scaled % scale);
        if (fraction) {
            append(text("."));
            for (uint32_t place = scale / 10; place && fraction; place /= 10) {
                char const digit = static_cast<char>('0' + fraction / place);
                append({&digit, 1});
                fraction %= place;
            }
        }
        if (exponent) { append(text("e")); integer(exponent); }
    }
    IvString view() const { return {bytes, size}; }
};

inline uint64_t sample_from_double(double value)
{
    if (!__builtin_isfinite(value)) return 0;
    auto const magnitude = __builtin_fabs(value);
    auto const remainder = magnitude >= sample_cycle
        ? magnitude - __builtin_floor(magnitude / sample_cycle) * sample_cycle : magnitude;
    auto const bits = static_cast<uint64_t>(remainder);
    return value < 0 ? 0 - bits : bits;
}

inline double round_sample(double value)
{
    auto const whole = __builtin_trunc(value);
    auto const fraction = value - whole;
    return whole + (fraction >= .5 ? 1 : fraction <= -.5 ? -1 : 0);
}

inline uint64_t point_sample(PlacePointPoint const& point, uint32_t sample_rate)
{
    auto const sample = point.sample_index.string;
    if (point.sample_index.kind == IV_SCALAR_STRING && sample.size) {
        bool const negative = sample.data[0] == '-';
        uint64_t result = 0;
        for (uint32_t i = negative; i < sample.size; ++i) result = result * 10 + (sample.data[i] - '0');
        return negative ? 0 - result : result;
    }
    auto const whole = __builtin_trunc(point.seconds);
    auto const fraction = round_sample((point.seconds - whole) * sample_rate);
    return sample_from_double(whole) * sample_rate + sample_from_double(fraction);
}

struct Point {
    uint64_t sample;
    uint32_t id, order;
    float value, tension;
    Interpolation interpolation;
    uint32_t handle;
};

inline bool point_less(Point const& left, Point const& right)
{
    if (left.sample != right.sample) return static_cast<int64_t>(left.sample) < static_cast<int64_t>(right.sample);
    if (left.order != right.order) return left.order < right.order;
    return left.id < right.id;
}

struct Timeline {
    uint64_t origin;
    double fraction, span, width;
    uint32_t rate;

    double forward_offset(uint64_t sample) const
    {
        if (sample == origin && fraction && span == sample_cycle) return span - fraction;
        return static_cast<double>(sample - origin) - fraction;
    }
    double offset(uint64_t sample) const
    {
        auto const distance = static_cast<double>(sample - origin);
        if (span < sample_cycle && distance >= (sample_cycle + span) / 2)
            return -static_cast<double>(origin - sample) - fraction;
        return forward_offset(sample);
    }
    double x(double offset_samples) const { return offset_samples / span * width; }
    uint64_t sample_at(double x) const
    {
        auto const displacement = x / width * span;
        auto const whole = __builtin_floor(displacement);
        auto const carry = __builtin_floor(displacement - whole + fraction + .5);
        return origin + sample_from_double(whole) + sample_from_double(carry);
    }
};

inline double arctangent(double value)
{
    auto const magnitude = __builtin_fabs(value);
    auto x = magnitude > 1 ? 1 / magnitude : magnitude;
    bool const reduced = x > .41421356237309505;
    if (reduced) x = (x - 1) / (x + 1);
    double term = x, result = x;
    auto const ratio = -x * x;
    for (int denominator = 3; denominator <= 41; denominator += 2) {
        term *= ratio;
        result += term / denominator;
    }
    if (reduced) result += pi / 4;
    if (magnitude > 1) result = pi / 2 - result;
    return value < 0 ? -result : result;
}

inline double tangent(double radians)
{
    // Callers restrict the domain to the principal interval. The viewport
    // uses finite pixel centers when converting a pointer position.
    auto const squared = -radians * radians;
    double sine = radians, cosine = 1, s = sine, c = cosine;
    for (int i = 2; i <= 24; i += 2) {
        s *= squared / (i * (i + 1));
        c *= squared / (i * (i - 1));
        sine += s;
        cosine += c;
    }
    return sine / cosine;
}

inline double power_of_two(int exponent)
{
    return __builtin_bit_cast(double, static_cast<uint64_t>(1023 + exponent) << 52);
}

enum class Ruler : uint32_t { automatic, whole, half, off };
struct ValueAxis {
    double pan = 0, zoom = 1, center = .5;
    bool cropped = false;
    Ruler ruler = Ruler::automatic;
    bool operator==(ValueAxis const&) const = default;

    double cropped_span() const { return 2 * larger(__builtin_fabs(pan) * 0x1p-23, 1 / zoom); }
    double fraction(double value) const
    {
        if (cropped) return .5 + (value - pan) / cropped_span();
        auto const fraction = .5 + arctangent((value - pan) * smaller(1., zoom)) / pi;
        return zoom > 1 ? .5 + (fraction - center) * zoom : fraction;
    }
    double value_at(double fraction) const
    {
        if (cropped) return pan + (fraction - .5) * cropped_span();
        if (zoom > 1) fraction = center + (fraction - .5) / zoom;
        return pan + tangent((fraction - .5) * pi) / smaller(1., zoom);
    }
    double units_per_pixel(double height) const
    {
        if (cropped) return cropped_span() / height;
        if (zoom == 1) return pi / height;
        auto centered = *this;
        centered.pan = 0;
        return centered.value_at(.5 + .5 / height) - centered.value_at(.5 - .5 / height);
    }
    double y(double value, double height) const { return (1 - fraction(value)) * height; }
    void reset() { pan = 0; zoom = 1; center = .5; }
    void pan_by(double fraction)
    {
        if (!cropped && zoom > 1) { center = crop_center(center + fraction / zoom, zoom); return; }
        auto const next = pan + (cropped ? cropped_span() : pi / smaller(1., zoom)) * fraction;
        if (__builtin_isfinite(static_cast<float>(next))) pan = next;
    }
    void zoom_by(double multiplier, double anchor)
    {
        auto const factor = clamp(zoom * multiplier, 0x1p-20, 0x1p20);
        if (factor == zoom) return;
        auto const offset = anchor - .5;
        if (cropped) {
            auto const value = value_at(anchor);
            zoom = factor;
            center = .5;
            pan = value - offset * cropped_span();
            return;
        }
        auto const wide_change = 1 / smaller(1., zoom) - 1 / smaller(1., factor);
        auto const inner = clamp(offset, -.25, .25);
        auto const next = pan + (tangent(pi * inner) + (offset - inner) * (2 * pi)) * wide_change;
        if (__builtin_isfinite(next)) pan = next;
        auto const crop = larger(1., factor);
        center = crop_center(center + offset * (1 / larger(1., zoom) - 1 / crop), crop);
        zoom = factor;
    }
    static double crop_center(double value, double factor)
    {
        auto const half = .5 / factor;
        return clamp(value, half, 1 - half);
    }
};

struct ValueGuide { double value, y; };
inline void value_guides(Array<ValueGuide>& guides, ValueAxis const& axis, double height)
{
    guides.clear();
    if (axis.ruler == Ruler::off) return;
    auto add = [&](double candidate, double spacing) {
        double const value = static_cast<float>(candidate);
        if (!__builtin_isfinite(value)) return;
        auto const y = axis.y(value, height);
        if (!(y >= 0 && y <= height)) return;
        for (auto const& guide : guides)
            if (__builtin_fabs(guide.y - y) < spacing) return;
        if (guides.size() < 100) guides.push({value, y});
    };
    add(0, 10); add(-1, 10); add(1, 10);
    auto const count = static_cast<uint32_t>(smaller(100., __builtin_floor(height / 18)));
    auto const minimum = axis.ruler == Ruler::whole ? 1 : axis.ruler == Ruler::half ? .5 : axis.units_per_pixel(height);
    double visible[201];
    visible[0] = axis.value_at(.5);
    for (uint32_t i = 1; i <= count; ++i) {
        auto const offset = static_cast<double>(i) / (2 * count + 1);
        visible[2 * i - 1] = axis.value_at(.5 - offset);
        visible[2 * i] = axis.value_at(.5 + offset);
    }
    for (double step = 1; step >= 0x1p-149; step *= .5) {
        for (uint32_t i = 0; i <= 2 * count; ++i) add(__builtin_floor(visible[i] / step + .5) * step, 18);
        if (step <= minimum || guides.size() == 100) break;
    }
    guides.sort([](auto const& a, auto const& b) { return a.value < b.value; });
}

struct DetentValue { double value, plateau; bool snapped; uint32_t index; };

template<class Values>
DetentValue at_detents(double travel, double anchor, double scale, double anchor_plateau, Values const& targets)
{
    constexpr double half = 12;
    bool const forward = travel >= 0;
    auto const direction = forward ? 1 : -1;
    DetentValue result{anchor + travel * scale, 0, false, 0};
    for (uint32_t step = 0; step < targets.size(); ++step) {
        auto const i = forward ? step : targets.size() - 1 - step;
        auto const target = targets[i].value;
        if (direction * (target - anchor) < 0) continue;
        auto const distance = (target - anchor) / scale;
        if (direction * (distance - travel) > 0) return {anchor + travel * scale, 0, false, 0};
        auto const start = target > anchor ? -distance - half : target < anchor ? -distance + half : anchor_plateau;
        auto const finish = start + travel;
        auto const plateau = clamp(finish, -half, half);
        result = {target + (finish - plateau) * scale, plateau, finish == plateau, i};
        if (direction * (result.value - target) <= 0) break;
        travel -= direction * (target == anchor ? half - direction * anchor_plateau : 2 * half);
    }
    return result;
}

} // namespace automation_presentation
