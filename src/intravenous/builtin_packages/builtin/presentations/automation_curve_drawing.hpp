#pragma once

#include "automation_curve_model.hpp"

namespace automation_presentation {

struct Arc {
    double begin, end, length;
    double value, next, outgoing, incoming;
    float tension;
    bool constant;

    double at(double sample) const
    {
        if (constant) return value;
        auto const t = (sample - begin) / length, u = (end - sample) / length;
        auto const difference = next - value;
        auto const linear = t <= u ? value + difference * t : next - difference * u;
        return clamp(linear + tension * t * u * ((outgoing - difference) * u + (difference - incoming) * t),
            smaller(value, next), larger(value, next));
    }
};

inline double tangent_delta(double previous, double next, double before, double after)
{
    if (!previous || !next || (previous < 0) != (next < 0)) return 0;
    auto const a = 2 * after + before, b = after + 2 * before;
    return (a + b) / (a / previous + b / next);
}

class CurveWindow {
    struct Group { uint32_t first, last; uint64_t sample; };
    Array<Group> groups_;

    uint32_t first_at(uint64_t sample) const
    {
        uint32_t start = 0, end = groups_.size();
        while (start < end) {
            auto const middle = start + (end - start) / 2;
            if (static_cast<int64_t>(groups_[middle].sample) < static_cast<int64_t>(sample)) start = middle + 1;
            else end = middle;
        }
        return start;
    }
    Arc arc(Array<Point> const& points, uint32_t index, double begin, double end) const
    {
        auto const count = groups_.size();
        auto const& current = groups_[index], &next = groups_[(index + 1) % count];
        auto const& first = points[current.last];
        auto const& last = points[next.first];
        double const length = static_cast<double>(last.sample - first.sample);
        Arc arc{begin, end, length, first.value, last.value, 0, 0, first.tension, first.interpolation == Interpolation::hold};
        if (arc.constant || !first.tension) return arc;
        auto const& before = points[groups_[prior(index, count)].last];
        auto const& following = points[groups_[(index + 2) % count].first];
        auto const slope = (arc.next - arc.value) / length;
        if (before.interpolation == Interpolation::bezier && points[current.first].value == first.value) {
            double const previous_length = static_cast<double>(first.sample - before.sample);
            arc.outgoing = tangent_delta((arc.value - before.value) / previous_length, slope, previous_length, length) * length;
        }
        if (points[next.last].interpolation == Interpolation::bezier && points[next.last].value == last.value) {
            double const next_length = static_cast<double>(following.sample - last.sample);
            arc.incoming = tangent_delta(slope, (following.value - arc.next) / next_length, length, next_length) * length;
        }
        return arc;
    }

public:
    Array<Arc> arcs;
    Array<uint32_t> visible;

    void index(Array<Point> const& points)
    {
        groups_.clear();
        for (uint32_t i = 0; i < points.size(); ++i) {
            auto const sample = points[i].sample;
            if (!groups_.size() || groups_[groups_.size() - 1].sample != sample) groups_.push({i, i, sample});
            else groups_[groups_.size() - 1].last = i;
        }
    }

    void project(Array<Point> const& points, Timeline const& time)
    {
        arcs.clear(); visible.clear();
        auto const count = groups_.size();
        if (!count) return;
        auto const first_at_origin = first_at(time.origin);
        auto const first = (first_at_origin < count && groups_[first_at_origin].sample == time.origin && time.fraction > 0
            ? first_at_origin + 1 : first_at_origin) % count;
        if (count == 1) {
            auto const& group = groups_[0];
            if (points[group.last].interpolation != Interpolation::empty)
                arcs.push({0, time.span, sample_cycle, points[group.last].value, points[group.last].value, 0, 0, 0, true});
            if (auto const offset = time.forward_offset(group.sample); offset >= 0 && offset <= time.span)
                for (auto i = group.first; i <= group.last; ++i) visible.push(i);
            return;
        }

        auto const before = prior(first, count);
        auto const& previous = groups_[before];
        // Subtract exact offsets before converting to double. This keeps
        // neighboring samples distinct even at the signed storage seam.
        auto const distance = time.origin - previous.sample;
        auto const length = groups_[first].sample - previous.sample;
        if (distance < length && points[previous.last].interpolation != Interpolation::empty) {
            auto const begin = -static_cast<double>(distance) - time.fraction;
            auto const end = static_cast<double>(length - distance) - time.fraction;
            if (end > 0) arcs.push(arc(points, before, begin, end));
        }
        for (uint32_t i = 0; i < count; ++i) {
            auto const index = (first + i) % count;
            auto const& group = groups_[index];
            auto const start = time.forward_offset(group.sample);
            if (start > time.span) break;
            if (start < 0) continue;
            for (auto slot = group.first; slot <= group.last; ++slot) visible.push(slot);
            if (points[group.last].interpolation == Interpolation::empty || start == time.span) continue;
            double const length = static_cast<double>(groups_[(index + 1) % count].sample - group.sample);
            arcs.push(arc(points, index, start, start + length));
        }
    }
};

class Bounds {
    Array<IvWaveformRegion const*> regions_;
    Array<IvWaveformRegion const*> active_;
    uint32_t next_ = 0;
    bool minimum_ = false, manual_ = false;
    double value_ = 0;

    static double interpolate(IvWaveformPointSpan points, double sample)
    {
        if (points.size == 1 || sample <= points.data[0].offset) return points.data[0].value;
        auto const& last = points.data[points.size - 1];
        if (sample >= last.offset) return last.value;
        uint32_t low = 0, high = points.size - 1;
        while (high - low > 1) {
            auto const middle = low + (high - low) / 2;
            if (points.data[middle].offset > sample) high = middle;
            else low = middle;
        }
        auto const& left = points.data[low], &right = points.data[high];
        auto const mix = (sample - left.offset) / (right.offset - left.offset);
        return (1 - mix) * left.value + mix * right.value;
    }

public:
    void read(Context const& frame, IvString name, bool minimum, Array<double>& edges, double span)
    {
        minimum_ = minimum; manual_ = false; next_ = 0;
        regions_.clear(); active_.clear();
        for (auto const& input : frame.manual_inputs())
            if (equal(input.id, name)) { value_ = input.value; manual_ = true; return; }
        auto const* waveform = frame.observed_waveform(name);
        if (!waveform) return;
        auto add_edge = [&](double sample) { if (sample > 0 && sample < span) edges.push(sample); };
        for (uint32_t i = 0; i < waveform->regions.size; ++i) {
            auto const& region = waveform->regions.data[i];
            if (!region.points.size || !(region.begin < region.end)) continue;
            regions_.push(&region);
            add_edge(region.begin); add_edge(region.end);
            for (uint32_t j = 0; j < region.points.size; ++j) {
                auto const sample = region.points.data[j].offset;
                if (sample > region.begin && sample < region.end) add_edge(sample);
            }
        }
        regions_.sort([](auto a, auto b) { return a->begin < b->begin; });
    }
    bool exists() const { return manual_ || regions_.size(); }
    void advance(double sample)
    {
        uint32_t count = 0;
        for (auto const* region : active_)
            if (region->end > sample) active_[count++] = region;
        while (active_.size() > count) active_.pop();
        while (next_ < regions_.size() && regions_[next_]->begin <= sample) {
            auto const* region = regions_[next_++];
            if (region->end > sample) active_.push(region);
        }
    }
    bool value_at(double sample, double& result) const
    {
        if (manual_) { result = value_; return true; }
        bool found = false;
        for (auto const* region : active_) {
            auto const value = interpolate(region->points, sample);
            result = !found ? value : minimum_ ? larger(result, value) : smaller(result, value);
            found = true;
        }
        return found;
    }
};

class StrokeBatch {
    struct Line { uint32_t id, ink, start, count; IvStroke style; };
    Array<Line> lines_;
    Array<IvPoint> points_;

public:
    void clear() { lines_.clear(); points_.clear(); }
    void add(uint32_t id, uint32_t ink, Span<IvPoint const> points, IvStroke style = {})
    {
        if (!points.size || points_.size() + points.size > 16384 || lines_.size() >= 2600
            || !points_.reserve(points_.size() + points.size)) return;
        if (!lines_.push({id, ink, points_.size(), points.size, style})) return;
        for (auto const& point : points) points_.push(point);
    }
    void line(uint32_t id, uint32_t ink, IvPoint a, IvPoint b, double opacity, IvStroke style = {})
    {
        IvPoint const points[]{a, b};
        style.flags |= IV_STROKE_OPACITY; style.opacity = opacity;
        add(id, ink, {points, 2}, style);
    }
    void submit(Context const& frame) const
    {
        if (!lines_.size()) return;
        auto const batch = frame.reserve_polylines(lines_.size(), points_.size());
        if (!batch.polylines) return;
        for (uint32_t i = 0; i < lines_.size(); ++i) {
            auto const& line = lines_[i];
            batch.polylines[i] = {line.id, line.ink, {batch.points + line.start, line.count}, line.style};
        }
        for (uint32_t i = 0; i < points_.size(); ++i) batch.points[i] = points_[i];
    }
};

class Track {
    struct Run { uint32_t begin, size; };
    Array<Run> runs_;
    Array<IvPoint> samples_;
    Array<IvPoint> projected_;

public:
    void clear() { runs_.clear(); samples_.clear(); }
    void append(IvPoint point, bool first)
    {
        if (!runs_.size() || (first && (samples_.size() == 0
            || samples_[samples_.size() - 1].x != point.x || samples_[samples_.size() - 1].y != point.y))) {
            if (!runs_.push({samples_.size(), 0})) return;
        } else if (first) return;
        if (samples_.push(point)) ++runs_[runs_.size() - 1].size;
    }
    void draw(StrokeBatch& batch, uint32_t id, uint32_t ink, IvStroke style)
    {
        auto const count = smaller(256u, runs_.size());
        if (!count) return;
        uint32_t interior = 0;
        auto const run = [&](uint32_t i) -> Run const& {
            return runs_[count == 1 ? 0 : i * (runs_.size() - 1) / (count - 1)];
        };
        for (uint32_t i = 0; i < count; ++i) if (run(i).size > 2) interior += run(i).size - 2;
        uint32_t seen = 0, spent = 0;
        for (uint32_t i = 0; i < count; ++i) {
            auto const& source = run(i);
            if (source.size < 2) continue;
            seen += source.size - 2;
            auto const total = interior ? smaller(seen, static_cast<uint32_t>(uint64_t(seen) * (2400 - 2 * count) / interior)) : 0;
            auto const size = 2 + total - spent;
            spent = total;
            projected_.clear();
            if (!projected_.reserve(size)) break;
            for (uint32_t j = 0; j < size; ++j) {
                auto const index = (j * (source.size - 1) + (size - 1) / 2) / (size - 1);
                projected_.push(samples_[source.begin + index]);
            }
            batch.add(id + i, ink, {projected_.begin(), projected_.size()}, style);
        }
    }
};

class CurvePreview {
    Array<double> edges_;
    Bounds min_, max_;
    Track authored_, output_, lower_, upper_;

public:
    void draw(Context const& frame, StrokeBatch& batch, Array<Arc> const& arcs, Timeline const& time, ValueAxis const& axis, double height)
    {
        edges_.clear(); edges_.push(0); edges_.push(time.span);
        authored_.clear(); output_.clear(); lower_.clear(); upper_.clear();
        for (auto const& arc : arcs) {
            if (arc.begin > 0 && arc.begin < time.span) edges_.push(arc.begin);
            if (arc.end > 0 && arc.end < time.span) edges_.push(arc.end);
        }
        min_.read(frame, text("min"), true, edges_, time.span);
        max_.read(frame, text("max"), false, edges_, time.span);
        bool const bounded = min_.exists() || max_.exists();
        edges_.sort([](double a, double b) { return a < b; });
        auto const pixels = clamp(__builtin_ceil(time.width), 1., 4096.);
        uint32_t active = 0;
        for (uint32_t i = 1; i < edges_.size(); ++i) {
            auto const begin = edges_[i - 1], end = edges_[i];
            if (!(begin < end)) continue;
            min_.advance(begin); max_.advance(begin);
            while (active < arcs.size() && arcs[active].end <= begin) ++active;
            auto const* arc = active < arcs.size() && arcs[active].begin <= begin ? &arcs[active] : nullptr;
            auto const steps = static_cast<uint32_t>(larger(1., __builtin_ceil((end - begin) / time.span * pixels)));
            for (uint32_t step = 0; step <= steps; ++step) {
                auto const sample = step == steps ? end : begin + (end - begin) * (double(step) / steps);
                auto const x = time.x(sample);
                double low = 0, high = 0;
                bool const has_low = min_.value_at(sample, low), has_high = max_.value_at(sample, high);
                if (has_low) lower_.append({x, axis.y(low, height)}, step == 0);
                if (has_high) upper_.append({x, axis.y(high, height)}, step == 0);
                if (!arc) continue;
                auto const value = arc->at(sample);
                authored_.append({x, axis.y(value, height)}, step == 0);
                if (bounded) {
                    auto const clamped = has_low && has_high && low > high ? .5 * low + .5 * high
                        : has_high ? smaller(high, has_low ? larger(low, value) : value)
                        : has_low ? larger(low, value) : value;
                    output_.append({x, axis.y(clamped, height)}, step == 0);
                }
            }
        }
        authored_.draw(batch, 1001, IV_INK_INPUT, {IV_STROKE_OPACITY, 0, bounded ? .32 : 1, 1.5, {}, {}});
        if (bounded) output_.draw(batch, 1301, IV_INK_INPUT, {IV_STROKE_OPACITY, 0, 1, 1.5, {}, {}});
        lower_.draw(batch, 1601, IV_INK_HIGHLIGHT, {IV_STROKE_OPACITY | IV_STROKE_DASHED, 0, .55, 1, {}, {}});
        upper_.draw(batch, 1901, IV_INK_WARNING, {IV_STROKE_OPACITY | IV_STROKE_DASHED, 0, .55, 1, {}, {}});
    }
};

} // namespace automation_presentation
