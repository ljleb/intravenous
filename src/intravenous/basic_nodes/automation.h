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

struct PointSource {
    ImmutableArray<Point> points;
    double duration;
    explicit PointSource(ImmutableArray<Point> points_, double duration_)
        : points(points_), duration(duration_) {
        if (!std::isfinite(duration) || duration <= 0)
            throw std::invalid_argument("automation duration must be positive and finite");
        double previous = 0;
        for (auto const& point : points) {
            if (!std::isfinite(point.seconds) || point.seconds < previous
                || point.seconds > duration || !std::isfinite(point.value))
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
        auto const frames = static_cast<long double>(duration) * rate;
        if (!std::isfinite(frames) || frames < 0
            || std::round(frames) >= static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
            throw std::length_error("automation duration exceeds the supported timeline span");
        return {GlobalIndex::from_bits(0), static_cast<std::uint64_t>(std::round(frames)) + 1};
    }
    void propagate_forward_coverage(PropagateForwardCoverageContext<PointSource>& ctx) const {
        auto region = range(ctx.sample_rate);
        ctx.output<"points">().publish(region);
        if (ctx.local_state_changed) ctx.output<"points">().change(IndexCoverage{region});
    }
    void tock_coverage(TockCoverageContext<PointSource>& ctx) const {
        auto output = ctx.output<"points">().region();
        auto const selected = output.range();
        auto const index = [rate = ctx.sample_rate](Point const& point) {
            return static_cast<std::uint64_t>(std::round(static_cast<long double>(point.seconds) * rate));
        };
        auto const first = std::ranges::lower_bound(points.values(), selected.begin.bits(), {}, index);
        auto const last = std::ranges::lower_bound(points.values(), selected.end().bits(), {}, index);
        for (auto point = first; point != last; ++point) {
            output.write({.time = GlobalIndex::from_bits(index(*point)),
                .value = SampleEvent{Sample{point->value}}});
        }
    }
};

struct LinearCurve {
    static constexpr bool intrinsically_replayable = true;
    static constexpr auto inputs() {
        return std::array{event_input("points", EventTypeId::sample, InputAccess::region)};
    }
    static constexpr auto outputs() {
        return std::array{output<Sample>("value")};
    }
    void propagate_forward_coverage(PropagateForwardCoverageContext<LinearCurve>& ctx) const {
        auto input = ctx.input<"points">();
        ctx.output<"value">().publish(input.coverage());
        // A changed point can affect an entire interpolation segment. Until
        // exact changed segments are represented, invalidate the clip's range.
        if (ctx.local_state_changed || !input.changed().empty())
            ctx.output<"value">().change(input.coverage());
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
        auto const region = input.range();
        auto const begin = ctx.index.whole();
        EventNeighbors neighbors;
        bool initialized = false;
        for (std::size_t frame = 0; frame < output.size(); ++frame) {
            auto const position = begin.advanced_by(frame);
            if (!region.contains(position)) {
                output[frame] = Sample{0};
                initialized = false;
                continue;
            }
            if (!initialized || (neighbors.after
                    && !RangeRelativeIndexOrder{region.begin}(position, neighbors.after->time))) {
                neighbors = input.neighbors(position);
                initialized = true;
            }
            float value = neighbors.at_or_before
                ? static_cast<float>(std::get<SampleEvent>(neighbors.at_or_before->value).value) : 0;
            if (neighbors.at_or_before && neighbors.after) {
                auto const first = neighbors.at_or_before->time;
                auto const distance = first.forward_distance_to(neighbors.after->time);
                auto const fraction = static_cast<double>(first.forward_distance_to(position)) / distance;
                auto const last = static_cast<float>(std::get<SampleEvent>(neighbors.after->value).value);
                value += static_cast<float>((last - value) * fraction);
            }
            output[frame] = Sample{value};
        }
    }
};

// The module is the editable semantic node. Its two leaves retain fixed schemas.
class AutomationCurve {
    std::vector<Point> points_;
    double duration_;
public:
    explicit AutomationCurve(std::vector<Point> points, double duration)
        : points_(std::move(points)), duration_(duration) {
        if (!std::isfinite(duration_) || duration_ <= 0)
            throw std::invalid_argument("automation duration must be positive and finite");
        for (auto const& point : points_) {
            if (!std::isfinite(point.seconds) || point.seconds < 0 || point.seconds > duration_
                || !std::isfinite(point.value)) throw std::invalid_argument("invalid automation point");
        }
        std::vector<std::uint32_t> ids;
        for (auto const& point : points_) ids.push_back(point.id);
        std::ranges::sort(ids);
        if (std::adjacent_find(ids.begin(), ids.end()) != ids.end())
            throw std::invalid_argument("automation point IDs must be unique");
        std::ranges::stable_sort(points_, {}, &Point::seconds);
    }
    void build_graph(GraphBuilder& g) const {
        auto points = g.node<"iv.automation.point_source">(g.immutable_array<Point>(points_), duration_);
        auto curve = g.node<"iv.automation.linear_curve">();

        curve("points"_F = points);

        g.outputs("value"_P = curve, "points"_F = points);
    }
    void on_message(ModuleMessageContext<AutomationCurve>& ctx, AddPoint const& event) const {
        auto points = points_;
        points.push_back(event.point);
        ctx.replace<"iv.automation.curve">(points, duration_);
    }
    void on_message(ModuleMessageContext<AutomationCurve>& ctx, RemovePoint const& event) const {
        auto points = points_;
        auto const found = std::ranges::find(points, event.id, &Point::id);
        if (found == points.end()) throw std::invalid_argument("automation point does not exist");
        points.erase(found);
        ctx.replace<"iv.automation.curve">(points, duration_);
    }
    void on_message(ModuleMessageContext<AutomationCurve>& ctx, MovePoint const& event) const {
        auto points = points_;
        auto const found = std::ranges::find(points, event.id, &Point::id);
        if (found == points.end()) throw std::invalid_argument("automation point does not exist");
        found->seconds = event.seconds;
        found->value = event.value;
        ctx.replace<"iv.automation.curve">(points, duration_);
    }
};
} // namespace iv::automation

