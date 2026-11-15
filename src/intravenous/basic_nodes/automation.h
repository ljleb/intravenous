#pragma once

#include <intravenous/dsl.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace iv::automation {
struct Point {
    std::uint32_t id = 0;
    double seconds = 0;
    float value = 0;
    constexpr bool operator==(Point const&) const = default;
};

// Semantic events are reusable types, separate from the receiving node.
struct AddPoint { Point point; };
struct RemovePoint { std::uint32_t id = 0; };
struct MovePoint { std::uint32_t id = 0; double seconds = 0; float value = 0; };

// Seconds are a signed authoring coordinate, not a boundary of the timeline.
// Convert only after checking the rounded sample coordinate fits its signed view.
[[nodiscard]] inline GlobalIndex point_index(double seconds, std::size_t rate) {
    auto const frames = std::round(static_cast<long double>(seconds) * rate);
    auto const half = std::ldexp(1.0L, 63);
    if (!std::isfinite(frames) || frames < -half || frames >= half)
        throw std::length_error("automation point is outside the signed timeline coordinate range");
    return GlobalIndex::from_signed(static_cast<std::int64_t>(frames));
}

struct PointSource {
    ImmutableArray<Point> points;
    explicit PointSource(ImmutableArray<Point> points_) : points(points_) {
        auto previous = -std::numeric_limits<double>::infinity();
        for (auto const& point : points) {
            if (!std::isfinite(point.seconds) || point.seconds < previous || !std::isfinite(point.value))
                throw std::invalid_argument("automation source points must be valid and sorted by time");
            previous = point.seconds;
        }
    }

    static constexpr auto outputs() {
        return std::array{event_output("points", EventTypeId::sample,
            OutputProduction::region, OutputRetention::persisted)};
    }
    void constrain_ports(ConstrainPortsContext<PointSource>& ctx) const {
        ctx.output<"points">().max_event_count() = points.size();
    }
    [[nodiscard]] GlobalIndexRange range(std::size_t rate) const {
        if (points.empty()) return {};
        auto const first = point_index(points.values().front().seconds, rate);
        auto previous = point_index(points.values().back().seconds, rate);
        if (first == previous) return {first, 1};
        // Exclude the largest gap. The remaining finite Region contains every
        // event, including when the closest points straddle a storage seam.
        GlobalIndexRange result;
        std::uint64_t largest_gap = 0;
        for (auto const& point : points) {
            auto const current = point_index(point.seconds, rate);
            auto const gap = previous.forward_distance_to(current);
            if (gap > largest_gap) {
                largest_gap = gap;
                result = {current, current.forward_distance_to(previous) + 1};
            }
            previous = current;
        }
        if (result.empty()) throw std::length_error("automation point storage occupies the entire timeline");
        return result;
    }
    void propagate_forward_coverage(PropagateForwardCoverageContext<PointSource>& ctx) const {
        auto region = range(ctx.sample_rate);
        if (!region.empty()) ctx.output<"points">().publish(region);
        if (ctx.local_state_changed) ctx.output<"points">().change(IndexCoverage{region});
    }
    void tock_coverage(TockCoverageContext<PointSource>& ctx) const {
        auto output = ctx.output<"points">().region();
        auto const selected = output.range();
        auto const values = points.values();
        auto const pivot = std::ranges::lower_bound(values, selected.begin.signed_value(), {},
            [rate = ctx.sample_rate](Point const& point) { return point_index(point.seconds, rate).signed_value(); });
        auto write = [&](auto first, auto last) {
            for (; first != last; ++first) {
                auto const time = point_index(first->seconds, ctx.sample_rate);
                if (selected.contains(time)) output.write({.time = time, .value = SampleEvent{Sample{first->value}}});
            }
        };
        write(pivot, values.end());
        write(values.begin(), pivot);
    }
};

struct LinearCurve {
    // Replayable; its authored forward propagation publishes its coverage.
    static constexpr TickReplayMode replay_mode = TickReplayMode::input_union;
    static constexpr auto inputs() {
        return std::array{event_input("points", EventTypeId::sample, InputAccess::region)};
    }
    static constexpr auto outputs() {
        return std::array{output<Sample>("value")};
    }
    void propagate_forward_coverage(PropagateForwardCoverageContext<LinearCurve>& ctx) const {
        auto input = ctx.input<"points">();
        // A full circle needs two ranges because one extent cannot hold 2^64.
        constexpr auto half = std::uint64_t{1} << 63;
        IndexCoverage const full{{GlobalIndex::from_bits(0), half},
                                 {GlobalIndex::from_bits(half), half}};
        ctx.output<"value">().publish(full);
        if (ctx.local_state_changed || !input.changed().empty())
            ctx.output<"value">().change(full);
    }
    void propagate_reverse_coverage(PropagateReverseCoverageContext<LinearCurve>& ctx) const {
        // Point storage is sparse. Retaining the logical Region provides the
        // neighboring points even when no point falls in the requested window.
        auto input = ctx.input<"points">();
        for (auto region : input.coverage()) input.require(region);
    }
    void tick_block(TickBlockContext<LinearCurve> const& ctx) const {
        auto port = ctx.input<"points">();
        auto output = ctx.output<"value">().block();
        if (port.coverage().empty()) {
            for (std::size_t frame = 0; frame < output.size(); ++frame)
                output[frame] = Sample{0};
            return;
        }
        auto input = port.region();
        auto const begin = ctx.index.whole();
        EventNeighbors neighbors;
        double first_value = 0, last_value = 0, inverse_distance = 0;
        bool initialized = false;
        for (std::size_t frame = 0; frame < output.size(); ++frame) {
            auto const position = begin.advanced_by(frame);
            if (!initialized || (neighbors.after && position == neighbors.after->time)) {
                neighbors = input.neighbors(position, EventNeighborMode::cyclic);
                if (!neighbors.at_or_before || !neighbors.after) {
                    for (; frame < output.size(); ++frame) output[frame] = Sample{0};
                    return;
                }
                first_value = static_cast<float>(std::get<SampleEvent>(neighbors.at_or_before->value).value);
                last_value = static_cast<float>(std::get<SampleEvent>(neighbors.after->value).value);
                auto const distance = neighbors.at_or_before->time.forward_distance_to(neighbors.after->time);
                // Equal timestamps span the full circle in cyclic mode. This
                // also handles a single point, whose values are identical.
                inverse_distance = 1.0 / (distance ? static_cast<double>(distance) : 0x1p64);
                initialized = true;
            }
            auto const elapsed = neighbors.at_or_before->time.forward_distance_to(position);
            auto const remaining = position.forward_distance_to(neighbors.after->time);
            // Use the nearer endpoint to retain precision beside the wrap.
            auto const value = elapsed <= remaining
                ? first_value + (last_value - first_value) * (static_cast<double>(elapsed) * inverse_distance)
                : last_value + (first_value - last_value) * (static_cast<double>(remaining) * inverse_distance);
            output[frame] = Sample{static_cast<float>(value)};
        }
    }
};

// The module is the editable semantic node. Its two leaves retain fixed schemas.
class AutomationCurve {
    std::vector<Point> points_;
public:
    explicit AutomationCurve(std::vector<Point> points) : points_(std::move(points)) {
        for (auto const& point : points_) {
            if (!std::isfinite(point.seconds) || !std::isfinite(point.value)) throw std::invalid_argument("invalid automation point");
        }
        std::vector<std::uint32_t> ids;
        for (auto const& point : points_) ids.push_back(point.id);
        std::ranges::sort(ids);
        if (std::adjacent_find(ids.begin(), ids.end()) != ids.end())
            throw std::invalid_argument("automation point IDs must be unique");
        std::ranges::stable_sort(points_, {}, &Point::seconds);
    }
    void build_graph(GraphBuilder& g) const {
        auto points = g.node<"iv.builtin.automation.point_source">(g.immutable_array<Point>(points_));
        auto curve = g.node<"iv.builtin.automation.linear_curve">();

        curve("points"_F = points);

        g.outputs("value"_P = curve, "points"_F = points);
    }
    void on_message(ModuleMessageContext<AutomationCurve>& ctx, AddPoint const& event) const {
        auto points = points_;
        points.push_back(event.point);
        ctx.replace<"iv.builtin.automation.curve">(points);
    }
    void on_message(ModuleMessageContext<AutomationCurve>& ctx, RemovePoint const& event) const {
        auto points = points_;
        auto const found = std::ranges::find(points, event.id, &Point::id);
        if (found == points.end()) throw std::invalid_argument("automation point does not exist");
        points.erase(found);
        ctx.replace<"iv.builtin.automation.curve">(points);
    }
    void on_message(ModuleMessageContext<AutomationCurve>& ctx, MovePoint const& event) const {
        auto points = points_;
        auto const found = std::ranges::find(points, event.id, &Point::id);
        if (found == points.end()) throw std::invalid_argument("automation point does not exist");
        found->seconds = event.seconds;
        found->value = event.value;
        ctx.replace<"iv.builtin.automation.curve">(points);
    }
};
} // namespace iv::automation

