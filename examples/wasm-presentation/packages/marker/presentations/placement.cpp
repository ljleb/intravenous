#include "automation_events.hpp"

namespace {
using namespace iv::presentation;
using automation_events::AcceptedPoints;
using automation_events::PlacePoint;
using automation_events::PlacePointPoint;
using automation_events::PlacePointPointInterpolation;
using automation_events::PlacePointSegmentsElement;
using automation_events::PlacePointSegmentsElementInterpolation;

constexpr uint32_t target = 1;
constexpr uint32_t point_id = 17;
constexpr char point_sample[] = "9223372036854775805";
template<size_t Length>
IvString text(char const (&value)[Length]) { return {value, Length - 1}; }

bool is_placement(PlacePointPoint const& point)
{
    if (point.id != point_id || point.sample_index.kind != IV_SCALAR_STRING) return false;
    auto const sample = point.sample_index.string;
    if (sample.size != sizeof(point_sample) - 1) return false;
    for (uint32_t i = 0; i < sample.size; ++i)
        if (sample.data[i] != point_sample[i]) return false;
    return true;
}

class Placement {
    EditGesture gesture_;
    EditRequest pending_;
    uint64_t accepted_revision_ = 0;
    float desired_ = .5f;
    float sent_ = 0;
    bool started_ = false;
    bool inserted_ = false;
    bool released_ = false;
    bool finishing_ = false;
    bool finished_ = false;
    bool rejected_ = false;

    void receive(Context& frame)
    {
        for (auto const& reply : frame.edit_replies()) {
            if (reply.request != pending_.id) continue;
            pending_ = {};
            if (reply.status == IV_EDIT_REJECTED) rejected_ = true;
            else if (reply.revision > accepted_revision_) accepted_revision_ = reply.revision;
            if (finishing_) finished_ = true;
            else if (!rejected_) inserted_ = true;
        }
    }

    void read_input(Context& frame)
    {
        auto const bounds = frame.content_bounds();
        for (auto const& input : frame.gestures()) {
            if (input.id != target || finished_) continue;
            if (input.phase == IV_GESTURE_BEGIN && !started_) {
                gesture_ = frame.begin_edit();
                started_ = true;
            }
            if (!started_ || released_) continue;
            if (input.phase == IV_GESTURE_FINISH || input.phase == IV_GESTURE_CANCEL) released_ = true;
            if (input.phase == IV_GESTURE_BEGIN || input.phase == IV_GESTURE_MOVE || input.phase == IV_GESTURE_FINISH) {
                auto const value = 1 - input.point.y / bounds.height;
                desired_ = static_cast<float>(value < 0 ? 0 : value > 1 ? 1 : value);
            }
        }
    }

    EditRequest send_placement(Context& frame, bool insert)
    {
        uint32_t const ordered[] = {21, point_id};
        PlacePointSegmentsElement const segments[] = {
            {.id = 12, .interpolation = PlacePointSegmentsElementInterpolation::empty, .tension = 1},
        };
        return frame.send(PlacePoint{
            .point = {.id = point_id, .seconds = 0, .value = desired_,
                .interpolation = PlacePointPointInterpolation::hold, .tension = 1,
                .sample_index = converted_string(text(point_sample)), .order = 1},
            .insert = insert, .segments = {segments, 1}, .ordered_ids = {ordered, 2},
        }, gesture_);
    }

    void submit(Context& frame)
    {
        if (!started_ || pending_ || finished_) return;
        if (rejected_ || (released_ && inserted_ && desired_ == sent_)) {
            pending_ = frame.finish_edit(gesture_);
            finishing_ = true;
            finished_ = !pending_;
        } else if (!inserted_ || desired_ != sent_) {
            pending_ = send_placement(frame, !inserted_);
            sent_ = desired_;
            if (!pending_) rejected_ = finished_ = true;
        }
    }

    PlacePointPoint const* accepted_placement(Span<PlacePointPoint const> points) const
    {
        for (auto const& point : points)
            if (is_placement(point)) return &point;
        return nullptr;
    }

    void draw(Context& frame, float value) const
    {
        auto const bounds = frame.content_bounds();
        auto const timeline = frame.timeline_view();
        IvWaveformRegionSpan regions{};
        if (auto const* waveform = frame.observed_waveform(text("value")); waveform && timeline.span_samples > 0)
            regions = waveform->regions;
        uint32_t points = 2;
        for (uint32_t i = 0; i < regions.size; ++i) points += regions.data[i].points.size;
        auto const mark = frame.reserve_polylines(1 + regions.size, points);
        if (!mark.polylines) return;
        auto const y = bounds.height * (1 - value);
        mark.points[0] = {0, y};
        mark.points[1] = {bounds.width, y};
        mark.polylines[0] = {1, IV_INK_INPUT, {mark.points, 2}};
        auto* vertices = mark.points + 2;
        for (uint32_t i = 0; i < regions.size; ++i) {
            auto const& region = regions.data[i];
            for (uint32_t j = 0; j < region.points.size; ++j) {
                auto const& point = region.points.data[j];
                auto const offset = point.offset < region.begin ? region.begin : point.offset > region.end ? region.end : point.offset;
                vertices[j] = {bounds.width * (offset / timeline.span_samples), bounds.height * (1 - point.value)};
            }
            mark.polylines[1 + i] = {2 + i, IV_INK_OUTPUT, {vertices, region.points.size}};
            vertices += region.points.size;
        }
    }

public:
    void present(Context& frame)
    {
        receive(frame);
        for (auto const& input : frame.editor_inputs()) {
            if (input.kind != IV_EDITOR_INTERRUPT) continue;
            pending_ = {};
            finished_ = true;
        }
        read_input(frame);
        submit(frame);

        auto const bounds = frame.content_bounds();
        auto const* points = frame.construction_argument<AcceptedPoints>();
        bool const awaiting_snapshot = !finished_ || frame.project_revision() < accepted_revision_;
        auto const* accepted = points && !awaiting_snapshot ? accepted_placement(*points) : nullptr;
        auto displayed = desired_;
        if (accepted) displayed = accepted->value;
        else if (!started_ && points && points->size) displayed = points->data[points->size - 1].value;
        draw(frame, displayed);
        if (frame.can_edit() && !started_ && !finished_) {
            IvDragTarget const hit{target, IV_DRAG_ABSOLUTE, bounds};
            frame.register_drag_targets({&hit, 1});
        }
        if (rejected_) frame.set_status(text("Placement rejected"));
        else if (!started_) frame.set_status(text("Drag to place one point"));
        else if (awaiting_snapshot) frame.set_status(text("Placement pending"));
        else if (accepted) frame.set_status(text("Placement accepted"));
        else frame.set_status(text("Placement missing from accepted arguments"));
    }
};
}

IV_PRESENTATION_CLASS(Placement, IV_PRESENTATION_TIMELINE)
