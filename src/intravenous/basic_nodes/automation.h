#pragma once

#include <intravenous/dsl.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace iv::automation {
// Configuration reads a signed decimal string. Keep the scalar pointer-free
// so Point remains safe in the graph-owned ImmutableArray.
class SampleIndex {
    std::array<char, 20> text_{};
    std::int64_t value_ = 0;
    std::uint8_t size_ = 0;
public:
    SampleIndex() = default;
    explicit SampleIndex(std::string_view text) {
        if (text.empty()) return;
        if (text.size() > text_.size()) throw std::invalid_argument("invalid automation sample index");
        auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), value_);
        if (error != std::errc{} || end != text.data() + text.size())
            throw std::invalid_argument("invalid automation sample index");
        auto const result = std::to_chars(text_.data(), text_.data() + text_.size(), value_);
        size_ = static_cast<std::uint8_t>(result.ptr - text_.data());
    }
    explicit operator std::string_view() const noexcept { return {text_.data(), size_}; }
    [[nodiscard]] bool present() const noexcept { return size_ != 0; }
    [[nodiscard]] GlobalIndex index() const noexcept { return GlobalIndex::from_signed(value_); }
    constexpr bool operator==(SampleIndex const&) const = default;
};

struct Point {
    std::uint32_t id = 0;
    double seconds = 0;
    float value = 0;
    CurveInterpolation interpolation = CurveInterpolation::bezier;
    float tension = 1;
    SampleIndex sample_index{};
    constexpr bool operator==(Point const&) const = default;
};

struct ValueBound {
    float value = 0;
    bool enabled = false;
    constexpr bool operator==(ValueBound const&) const = default;
};

struct ValueBounds {
    ValueBound min{};
    ValueBound max{.value = 1};
    constexpr bool operator==(ValueBounds const&) const = default;

    void validate() const {
        if (!std::isfinite(min.value) || !std::isfinite(max.value)
            || (min.enabled && max.enabled && min.value > max.value))
            throw std::invalid_argument("invalid automation value bounds");
    }
    [[nodiscard]] float clamp(float value) const noexcept {
        if (min.enabled) value = std::max(min.value, value);
        if (max.enabled) value = std::min(max.value, value);
        return value;
    }
};

// Semantic events are reusable types, separate from the receiving node.
struct AddPoint { Point point; };
struct RemovePoint { std::uint32_t id = 0; };
struct MovePoint { std::uint32_t id = 0; double seconds = 0; float value = 0; SampleIndex sample_index{}; };
struct SetSegment { std::uint32_t id = 0; CurveInterpolation interpolation = CurveInterpolation::bezier; float tension = 1; };
enum class BoundSide : std::uint8_t { min, max };
struct SetBound { BoundSide side = BoundSide::min; ValueBound bound{}; };

inline void validate_point(Point const& point) {
    if (!std::isfinite(point.seconds) || !std::isfinite(point.value)
        || !std::isfinite(point.tension) || point.tension < 0 || point.tension > 1
        || static_cast<unsigned>(point.interpolation) > static_cast<unsigned>(CurveInterpolation::bezier))
        throw std::invalid_argument("invalid automation point");
}


// Seconds are a signed authoring coordinate, not a boundary of the timeline.
// Convert only after checking the rounded sample coordinate fits its signed view.
[[nodiscard]] inline GlobalIndex point_index(double seconds, std::size_t rate) {
    auto const frames = std::round(static_cast<long double>(seconds) * rate);
    auto const half = std::ldexp(1.0L, 63);
    if (!std::isfinite(frames) || frames < -half || frames >= half)
        throw std::length_error("automation point is outside the signed timeline coordinate range");
    return GlobalIndex::from_signed(static_cast<std::int64_t>(frames));
}

[[nodiscard]] inline GlobalIndex point_index(Point const& point, std::size_t rate) {
    return point.sample_index.present() ? point.sample_index.index() : point_index(point.seconds, rate);
}

inline bool point_time_less(Point const& left, Point const& right) {
    if (left.sample_index.present() != right.sample_index.present()) return !left.sample_index.present();
    if (left.sample_index.present())
        return left.sample_index.index().signed_value() < right.sample_index.index().signed_value();
    return left.seconds < right.seconds;
}

struct PointSource {
    ImmutableArray<Point> points;
    ValueBounds bounds;
    explicit PointSource(ImmutableArray<Point> points_, ValueBounds bounds_ = {}) : points(points_), bounds(bounds_) {
        bounds.validate();
        for (auto const& point : points) validate_point(point);
        if (!std::ranges::is_sorted(points, point_time_less))
            throw std::invalid_argument("automation source points must be sorted within each time representation");
    }

    static constexpr auto outputs() {
        return std::array{event_output("points", EventTypeId::curve_point,
            OutputProduction::region, OutputRealization::buffered)};
    }
    void constrain_ports(ConstrainPortsContext<PointSource>& ctx) const {
        ctx.output<"points">().max_event_count() = points.size();
    }
    [[nodiscard]] GlobalIndexRange range(std::size_t rate) const {
        // A Region output has one nonempty logical range even when its event
        // group has no events. This storage range never defines curve coverage.
        if (points.empty()) return {GlobalIndex::from_bits(0), 1};
        // Old seconds and exact sample indices can coexist in a project.
        // Merge their sorted positions once the sampling rate is known.
        auto const split = std::ranges::partition_point(points, [](Point const& point) { return !point.sample_index.present(); });
        auto legacy = points.begin(), exact = split;
        auto const end = points.end();
        auto next = [&] {
            if (exact == end || (legacy != split && point_index(*legacy, rate).signed_value() <= exact->sample_index.index().signed_value()))
                return point_index(*legacy++, rate);
            return (exact++)->sample_index.index();
        };
        auto const first = next();
        auto previous = point_index(points.values().back(), rate);
        if (split != points.begin()) {
            auto const last_legacy = point_index(*(split - 1), rate);
            if (last_legacy.signed_value() > previous.signed_value()) previous = last_legacy;
        }
        if (first == previous) return {first, 1};
        // Exclude the largest gap. The remaining finite Region contains every
        // event, including when the closest points straddle a storage seam.
        GlobalIndexRange result;
        std::uint64_t largest_gap = 0;
        auto current = first;
        for (;;) {
            auto const gap = previous.forward_distance_to(current);
            if (gap > largest_gap) {
                largest_gap = gap;
                result = {current, current.forward_distance_to(previous) + 1};
            }
            previous = current;
            if (legacy == split && exact == end) break;
            current = next();
        }
        if (result.empty()) throw std::length_error("automation point storage occupies the entire timeline");
        return result;
    }
    void propagate_forward_coverage(PropagateForwardCoverageContext<PointSource>& ctx) const {
        auto region = range(ctx.sample_rate);
        ctx.output<"points">().publish(region);
        if (ctx.local_state_changed) ctx.output<"points">().change(IndexCoverage{region});
    }
    void tock_coverage(TockCoverageContext<PointSource>& ctx) const {
        auto output = ctx.output<"points">().region();
        auto const selected = output.range();
        struct Entry { GlobalIndex time; Point const* point; };
        std::vector<Entry> ordered;
        for (auto const& point : points) {
            auto const time = point_index(point, ctx.sample_rate);
            if (ctx.output<"points">().requested_coverage().contains(time)) ordered.push_back({time, &point});
        }
        std::ranges::sort(ordered, [&](auto const& left, auto const& right) {
            auto const a = selected.begin.forward_distance_to(left.time);
            auto const b = selected.begin.forward_distance_to(right.time);
            return a != b ? a < b : left.point->id < right.point->id;
        });
        for (auto const& entry : ordered) output.write({.time = entry.time,
            .value = CurvePointEvent{Sample{bounds.clamp(entry.point->value)}, entry.point->interpolation, entry.point->tension}});
    }
};

struct Curve {
    static constexpr auto inputs() {
        return std::array{event_input("points", EventTypeId::curve_point, InputAccess::region)};
    }
    static constexpr auto outputs() {
        return std::array{output<Sample>("value", OutputProduction::coverage)};
    }

    static IndexCoverage point_coverage(std::span<TimedEvent const> points) {
        IndexCoverage coverage;
        if (points.empty()) return coverage;
        bool const single_position = std::ranges::all_of(points,
            [&](auto const& point) { return point.time == points.front().time; });
        if (single_position) {
            if (std::get<CurvePointEvent>(points.back().value).interpolation != CurveInterpolation::empty) {
                constexpr auto half = std::uint64_t{1} << 63;
                coverage.include({GlobalIndex::from_bits(0), half});
                coverage.include({GlobalIndex::from_bits(half), half});
            }
            return coverage;
        }
        for (std::size_t i = 0; i < points.size(); ++i) {
            auto const& point = points[i];
            auto const& next = points[(i + 1) % points.size()];
            if (point.time != next.time
                && std::get<CurvePointEvent>(point.value).interpolation != CurveInterpolation::empty)
                coverage.include({point.time, point.time.forward_distance_to(next.time)});
        }
        return coverage;
    }

    CoverageTask propagate_forward_coverage(PropagateForwardCoverageContext<Curve>& ctx) const {
        auto const& metadata = co_await ctx.request_input_coverage<"points">();
        std::vector<TimedEvent> points;
        if (!metadata.coverage.empty()) {
            auto input = co_await ctx.request_input<"points">(metadata.coverage);
            input.region().for_each([&](TimedEvent const& point) { points.push_back(point); });
        }
        auto const coverage = point_coverage(points);
        std::vector<GlobalIndexRange> regions(coverage.begin(), coverage.end());
        auto output = ctx.output<"value">();
        auto const previous = output.previous_coverage();
        output.publish(regions);
        if (ctx.local_state_changed || !ctx.input<"points">().changed().empty())
            output.change(previous | coverage);
    }
    void propagate_reverse_coverage(PropagateReverseCoverageContext<Curve>& ctx) const {
        // Tangents and equal-time endpoints require complete point contents,
        // whose size follows the point count rather than the sample duration.
        auto input = ctx.input<"points">();
        for (auto region : input.coverage()) input.require(region);
    }

    // Weighted harmonic endpoint derivative (PCHIP). Opposing secants produce
    // a flat tangent. A derivative never exceeds three times either secant.
    static double tangent(double previous_slope, double next_slope,
                          double previous_length, double next_length) {
        if (previous_slope == 0 || next_slope == 0
            || std::signbit(previous_slope) != std::signbit(next_slope)) return 0;
        auto const first_weight = 2 * next_length + previous_length;
        auto const second_weight = next_length + 2 * previous_length;
        return (first_weight + second_weight)
            / (first_weight / previous_slope + second_weight / next_slope);
    }

    struct Segment {
        GlobalIndex begin{}, end{};
        double length = 0x1p64;
        double first = 0, last = 0, start_delta = 0, end_delta = 0;
        float tension = 1;
        CurveInterpolation interpolation = CurveInterpolation::empty;

        Sample value(GlobalIndex position) const {
            IV_ASSERT(interpolation != CurveInterpolation::empty, "curve read lies in an empty segment");
            if (interpolation == CurveInterpolation::hold) return Sample{static_cast<float>(first)};
            auto const elapsed = begin.forward_distance_to(position);
            auto const remaining = position.forward_distance_to(end);
            // A coincident begin/end denotes one point group over a full cycle.
            auto const t = static_cast<double>(elapsed) / length;
            auto const u = begin == end ? 1 - t : static_cast<double>(remaining) / length;
            auto const difference = last - first;
            auto const linear = elapsed <= remaining
                ? first + difference * t : last - difference * u;
            auto const automatic = t * u * ((start_delta - difference) * u + (difference - end_delta) * t);
            return Sample{static_cast<float>(std::clamp(linear + tension * automatic,
                std::min(first, last), std::max(first, last)))};
        }
    };

    static Segment segment(EventRegion const& input, GlobalIndex position) {
        auto const neighbors = input.neighbors(position, EventNeighborMode::cyclic);
        IV_ASSERT(neighbors.at_or_before && neighbors.after, "curve has no points for a covered position");
        auto const& before = *neighbors.at_or_before;
        auto const& after = *neighbors.after;
        auto const& outgoing = std::get<CurvePointEvent>(before.value);
        auto const& incoming = std::get<CurvePointEvent>(after.value);
        Segment result{.begin = before.time, .end = after.time,
            .length = before.time == after.time ? 0x1p64
                : static_cast<double>(before.time.forward_distance_to(after.time)),
            .first = static_cast<float>(outgoing.value), .last = static_cast<float>(incoming.value),
            .tension = outgoing.tension, .interpolation = outgoing.interpolation};
        // All events at one index use the last point's outgoing value. The
        // first point at that same index is only an incoming endpoint.
        if (before.time == after.time) { result.last = result.first; return result; }
        if (outgoing.interpolation != CurveInterpolation::bezier || outgoing.tension == 0) return result;
        auto const slope = (result.last - result.first) / result.length;
        auto const previous = input.neighbors(before.time.retreated_by(1), EventNeighborMode::cyclic);
        auto const next = input.neighbors(after.time, EventNeighborMode::cyclic);
        if (previous.at_or_before && previous.after) {
            auto const& prior = std::get<CurvePointEvent>(previous.at_or_before->value);
            auto const& first_at_begin = std::get<CurvePointEvent>(previous.after->value);
            if (prior.interpolation == CurveInterpolation::bezier && first_at_begin.value == outgoing.value) {
                auto const length = static_cast<double>(previous.at_or_before->time.forward_distance_to(before.time));
                if (length > 0) result.start_delta = tangent(
                    (result.first - static_cast<float>(prior.value)) / length, slope, length, result.length) * result.length;
            }
        }
        if (next.at_or_before && next.after) {
            auto const& last_at_end = std::get<CurvePointEvent>(next.at_or_before->value);
            auto const& following = std::get<CurvePointEvent>(next.after->value);
            if (last_at_end.interpolation == CurveInterpolation::bezier && incoming.value == last_at_end.value) {
                auto const length = static_cast<double>(after.time.forward_distance_to(next.after->time));
                if (length > 0) result.end_delta = tangent(slope,
                    (static_cast<float>(following.value) - result.last) / length, result.length, length) * result.length;
            }
        }
        return result;
    }

    void tock_coverage(TockCoverageContext<Curve>& ctx) const {
        auto const input = ctx.input<"points">().region();
        for (auto region : ctx.output<"value">().regions()) for (auto output : region.prepared_windows()) {
            Segment current;
            bool initialized = false;
            for (std::size_t frame = 0; frame < output.frame_count(); ++frame) {
                auto const position = output.range().begin.advanced_by(frame);
                if (!initialized || position == current.end) { current = segment(input, position); initialized = true; }
                output.value(frame) = current.value(position);
            }
        }
    }
};

// The module is the editable semantic node. Its two leaves retain fixed schemas.
class AutomationCurve {
    std::vector<Point> points_;
    ValueBounds bounds_;

    void replace_points(ModuleMessageContext<AutomationCurve>& ctx, std::vector<Point> const& points) const {
        if (bounds_ == ValueBounds{}) ctx.replace<"iv.builtin.automation.curve">(points);
        else ctx.replace<"iv.builtin.automation.curve">(points, bounds_);
    }
public:
    explicit AutomationCurve(std::vector<Point> points, ValueBounds bounds = {})
        : points_(std::move(points)), bounds_(bounds) {
        bounds_.validate();
        for (auto const& point : points_) {
            validate_point(point);
        }
        std::vector<std::uint32_t> ids;
        for (auto const& point : points_) ids.push_back(point.id);
        std::ranges::sort(ids);
        if (std::adjacent_find(ids.begin(), ids.end()) != ids.end())
            throw std::invalid_argument("automation point IDs must be unique");
        std::ranges::sort(points_, [](Point const& left, Point const& right) {
            if (point_time_less(left, right)) return true;
            if (point_time_less(right, left)) return false;
            return left.id < right.id;
        });
    }
    void build_graph(GraphBuilder& g) const {
        auto points = g.node<"iv.builtin.automation.point_source">(g.immutable_array<Point>(points_), bounds_);
        auto curve = g.node<"iv.builtin.automation.interpreter">();

        curve("points"_F = points);

        g.outputs("value"_P = curve);
    }
    void on_message(ModuleMessageContext<AutomationCurve>& ctx, AddPoint const& event) const {
        auto point = event.point;
        validate_point(point);
        point.value = bounds_.clamp(point.value);
        auto points = points_;
        points.push_back(point);
        replace_points(ctx, points);
    }
    void on_message(ModuleMessageContext<AutomationCurve>& ctx, RemovePoint const& event) const {
        auto points = points_;
        auto const found = std::ranges::find(points, event.id, &Point::id);
        if (found == points.end()) throw std::invalid_argument("automation point does not exist");
        points.erase(found);
        replace_points(ctx, points);
    }
    void on_message(ModuleMessageContext<AutomationCurve>& ctx, SetSegment const& event) const {
        auto points = points_;
        auto const found = std::ranges::find(points, event.id, &Point::id);
        if (found == points.end()) throw std::invalid_argument("automation point does not exist");
        found->interpolation = event.interpolation;
        found->tension = event.tension;
        validate_point(*found);
        replace_points(ctx, points);
    }
    void on_message(ModuleMessageContext<AutomationCurve>& ctx, MovePoint const& event) const {
        auto points = points_;
        auto const found = std::ranges::find(points, event.id, &Point::id);
        if (found == points.end()) throw std::invalid_argument("automation point does not exist");
        found->seconds = event.seconds;
        found->sample_index = event.sample_index;
        found->value = event.value;
        validate_point(*found);
        found->value = bounds_.clamp(found->value);
        replace_points(ctx, points);
    }
    void on_message(ModuleMessageContext<AutomationCurve>& ctx, SetBound const& event) const {
        if (!std::isfinite(event.bound.value))
            throw std::invalid_argument("invalid automation bound value");
        auto bounds = bounds_;
        switch (event.side) {
        case BoundSide::min:
            bounds.min = event.bound;
            if (bounds.min.enabled && bounds.max.enabled)
                bounds.min.value = std::min(bounds.min.value, bounds.max.value);
            break;
        case BoundSide::max:
            bounds.max = event.bound;
            if (bounds.max.enabled && bounds.min.enabled)
                bounds.max.value = std::max(bounds.max.value, bounds.min.value);
            break;
        default:
            throw std::invalid_argument("invalid automation bound side");
        }
        ctx.replace<"iv.builtin.automation.curve">(points_, bounds);
    }
};
} // namespace iv::automation

