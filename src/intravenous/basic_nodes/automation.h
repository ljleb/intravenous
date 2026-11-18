#pragma once

#include <intravenous/dsl.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
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
    std::uint32_t order = 0;
    constexpr bool operator==(Point const&) const = default;
};

// An uncovered side supplies no limit. Crossed finite bounds collapse to
// their midpoint so the result stays continuous as the two signals cross.
inline float clamp_output_value(float value, std::optional<float> min, std::optional<float> max) noexcept {
    if (min && max && *min > *max) return std::midpoint(*min, *max);
    if (min) value = std::max(value, *min);
    if (max) value = std::min(value, *max);
    return value;
}

// Semantic events are reusable types, separate from the receiving node.
struct AddPoint { Point point; };
struct RemovePoint { std::uint32_t id = 0; };
struct MovePoint { std::uint32_t id = 0; double seconds = 0; float value = 0; SampleIndex sample_index{}; };
struct SetSegment { std::uint32_t id = 0; CurveInterpolation interpolation = CurveInterpolation::bezier; float tension = 1; };
struct PlacePoint {
    Point point;
    bool insert = false;
    std::vector<SetSegment> segments{};
    std::vector<std::uint32_t> ordered_ids{};
};

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

inline bool point_order_less(Point const& left, Point const& right) {
    return left.order != right.order ? left.order < right.order : left.id < right.id;
}

struct PointSource {
    ImmutableArray<Point> points;
    explicit PointSource(ImmutableArray<Point> points_) : points(points_) {
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
            return a != b ? a < b : point_order_less(*left.point, *right.point);
        });
        for (auto const& entry : ordered) output.write({.time = entry.time,
            .value = CurvePointEvent{Sample{entry.point->value}, entry.point->interpolation, entry.point->tension}});
    }
};

struct Curve {
    static constexpr auto inputs() {
        return std::tuple{
            event_input("points", EventTypeId::curve_point, InputAccess::region),
            input<Sample>("min", InputAccess::coverage, {.channel_layout = mono_planar_channel_layout}),
            input<Sample>("max", InputAccess::coverage, {.channel_layout = mono_planar_channel_layout})};
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
        (void)(co_await ctx.request_input_coverage<"min">());
        (void)(co_await ctx.request_input_coverage<"max">());
        auto const coverage = point_coverage(points);
        std::vector<GlobalIndexRange> regions(coverage.begin(), coverage.end());
        auto output = ctx.output<"value">();
        auto const previous = output.previous_coverage();
        output.publish(regions);
        auto const bound_changes = ctx.input<"min">().changed() | ctx.input<"max">().changed();
        if (ctx.local_state_changed || !ctx.input<"points">().changed().empty())
            output.change(previous | coverage);
        else
            output.change(bound_changes & coverage);
    }

    void propagate_reverse_coverage(PropagateReverseCoverageContext<Curve>& ctx) const {
        auto const& wanted = ctx.output<"value">().required();
        if (wanted.empty()) return;
        // The event Region is sparse. Bound samples are needed only over the
        // covered output positions that this invocation will render.
        auto const points = ctx.input<"points">();
        for (auto region : points.coverage()) points.require(region);
        ctx.input<"min">().require(wanted);
        ctx.input<"max">().require(wanted);
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
        auto value = [](TimedEvent const& event) { return std::get<CurvePointEvent>(event.value).value.value; };
        auto const neighbors = input.neighbors(position, EventNeighborMode::cyclic);
        IV_ASSERT(neighbors.at_or_before && neighbors.after, "curve has no points for a covered position");
        auto const& before = *neighbors.at_or_before;
        auto const& after = *neighbors.after;
        auto const& outgoing = std::get<CurvePointEvent>(before.value);
        Segment result{.begin = before.time, .end = after.time,
            .length = before.time == after.time ? 0x1p64
                : static_cast<double>(before.time.forward_distance_to(after.time)),
            .tension = outgoing.tension, .interpolation = outgoing.interpolation};
        if (outgoing.interpolation == CurveInterpolation::empty) return result;
        result.first = value(before);
        // All events at one index use the last point's outgoing value. The
        // first point at that same index is only an incoming endpoint.
        if (before.time == after.time || outgoing.interpolation == CurveInterpolation::hold) {
            result.last = result.first;
            return result;
        }
        result.last = value(after);
        if (outgoing.tension == 0) return result;
        auto const slope = (result.last - result.first) / result.length;
        auto const previous = input.neighbors(before.time.retreated_by(1), EventNeighborMode::cyclic);
        auto const next = input.neighbors(after.time, EventNeighborMode::cyclic);
        if (previous.at_or_before && previous.after) {
            auto const& prior = std::get<CurvePointEvent>(previous.at_or_before->value);
            if (prior.interpolation == CurveInterpolation::bezier) {
                auto const previous_value = value(*previous.at_or_before);
                auto const first_at_begin = value(*previous.after);
                auto const length = static_cast<double>(previous.at_or_before->time.forward_distance_to(before.time));
                if (length > 0 && first_at_begin == result.first) result.start_delta = tangent(
                    (result.first - previous_value) / length, slope, length, result.length) * result.length;
            }
        }
        if (next.at_or_before && next.after) {
            auto const& last_at_end = std::get<CurvePointEvent>(next.at_or_before->value);
            if (last_at_end.interpolation == CurveInterpolation::bezier) {
                auto const last_at_end_value = value(*next.at_or_before);
                auto const following = value(*next.after);
                auto const length = static_cast<double>(after.time.forward_distance_to(next.after->time));
                if (length > 0 && result.last == last_at_end_value) result.end_delta = tangent(slope,
                    (following - result.last) / length, result.length, length) * result.length;
            }
        }
        return result;
    }

    template<class Combine>
    static std::optional<float> bound_at(ContinuousCoverage<Sample> const& bound, GlobalIndex position, Combine combine) {
        if (!bound.coverage().contains(position)) return std::nullopt;
        std::optional<float> result;
        bool found = false;
        for (auto region : bound) {
            if (!region.range().contains(position)) continue;
            found = true;
            auto const value = region.at(position).value;
            if (std::isfinite(value)) result = result ? combine(*result, value) : value;
        }
        if (!found) throw std::logic_error("covered automation bound has no Region");
        return result;
    }

    void tock_coverage(TockCoverageContext<Curve>& ctx) const {
        auto const input = ctx.input<"points">().region();
        auto const min = ctx.input<"min">().regions();
        auto const max = ctx.input<"max">().regions();
        auto const bounded = !min.empty() || !max.empty();
        for (auto region : ctx.output<"value">().regions()) for (auto output : region.prepared_windows()) {
            Segment current;
            bool initialized = false;
            for (std::size_t frame = 0; frame < output.frame_count(); ++frame) {
                auto const position = output.range().begin.advanced_by(frame);
                if (!initialized || position == current.end) { current = segment(input, position); initialized = true; }
                auto value = current.value(position);
                if (bounded) value.value = clamp_output_value(value.value,
                    bound_at(min, position, [](float a, float b) { return std::max(a, b); }),
                    bound_at(max, position, [](float a, float b) { return std::min(a, b); }));
                output.value(frame) = value;
            }
        }
    }
};

// The module is the editable semantic node. Its two leaves retain fixed schemas.
class AutomationCurve {
    std::vector<Point> points_;

    void replace_points(ModuleMessageContext<AutomationCurve>& ctx, std::vector<Point> const& points) const {
        ctx.replace<"iv.builtin.automation.curve">(points);
    }
    static void set_segment(std::vector<Point>& points, SetSegment const& event) {
        auto const found = std::ranges::find(points, event.id, &Point::id);
        if (found == points.end()) throw std::invalid_argument("automation point does not exist");
        found->interpolation = event.interpolation;
        found->tension = event.tension;
        validate_point(*found);
    }
public:
    explicit AutomationCurve(std::vector<Point> points) : points_(std::move(points)) {
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
            return point_order_less(left, right);
        });
    }
    void build_graph(GraphBuilder& g) const {
        auto points = g.node<"iv.builtin.automation.point_source">(g.immutable_array<Point>(points_));
        auto curve = g.node<"iv.builtin.automation.interpreter">();

        auto const min = g.input<"min", Sample>(InputAccess::coverage, {
            .channel_layout = mono_planar_channel_layout, .default_value = 0, .enabled_by_default = false});
        auto const max = g.input<"max", Sample>(InputAccess::coverage, {
            .channel_layout = mono_planar_channel_layout, .default_value = 1, .enabled_by_default = false});
        curve("points"_F = points, "min"_P = min, "max"_P = max);

        g.outputs("value"_P = curve);
    }
    void on_message(ModuleMessageContext<AutomationCurve>& ctx, AddPoint const& event) const {
        auto point = event.point;
        validate_point(point);
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
        set_segment(points, event);
        replace_points(ctx, points);
    }
    void on_message(ModuleMessageContext<AutomationCurve>& ctx, PlacePoint const& event) const {
        auto point = event.point;
        validate_point(point);
        auto points = points_;
        auto const found = std::ranges::find(points, point.id, &Point::id);
        if (event.insert) {
            if (found != points.end()) throw std::invalid_argument("automation point already exists");
            points.push_back(point);
        } else {
            if (found == points.end()) throw std::invalid_argument("automation point does not exist");
            *found = point;
        }
        for (auto const& segment : event.segments) set_segment(points, segment);
        if (!event.ordered_ids.empty()) {
            std::unordered_map<std::uint32_t, std::uint32_t> ranks;
            for (auto const id : event.ordered_ids) {
                if (!ranks.emplace(id, static_cast<std::uint32_t>(ranks.size())).second)
                    throw std::invalid_argument("duplicate automation point in coincident order");
            }
            if (!ranks.contains(point.id))
                throw std::invalid_argument("placed automation point is missing from coincident order");
            for (auto& candidate : points) {
                if (auto const rank = ranks.find(candidate.id); rank != ranks.end()) {
                    candidate.order = rank->second;
                    ranks.erase(rank);
                }
            }
            if (!ranks.empty()) throw std::invalid_argument("automation point does not exist");
        }
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
        replace_points(ctx, points);
    }
};
} // namespace iv::automation

