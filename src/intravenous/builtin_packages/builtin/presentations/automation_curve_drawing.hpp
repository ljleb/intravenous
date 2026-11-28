#pragma once

#include "automation_curve_model.hpp"
#include "../../../basic_nodes/automation_segment.h"

namespace automation_presentation {

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

} // namespace automation_presentation
