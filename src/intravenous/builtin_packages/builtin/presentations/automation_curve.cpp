#include "automation_curve_drawing.hpp"
#include "automation_curve_edits.hpp"

namespace automation_presentation {
namespace {

constexpr uint32_t point_mark_base = 0x40000000;
constexpr uint32_t tension_mark_base = 0x80000000;
constexpr uint32_t selection_mark = 0xc0000000;

constexpr IvChoice display_choices[]{{text("curved"), text("Curved")}, {text("cropped"), text("Cropped")}};
constexpr IvChoice ruler_choices[]{{text("auto"), text("Auto")}, {text("whole"), text("Whole")},
    {text("half"), text("Halves")}, {text("off"), text("Off")}};
constexpr IvChoice segment_choices[]{{text("empty"), text("Empty")}, {text("hold"), text("Hold")}, {text("bezier"), text("Bézier")}};
constexpr IvString sensitivity_shortcut = text("Wheel ↑×2 ↓½");

template<size_t N>
int choice_index(IvChoice const (&options)[N], IvString value)
{
    for (unsigned i = 0; i < N; ++i) if (equal(options[i].value, value)) return static_cast<int>(i);
    return -1;
}

template<size_t N>
IvSetting choice(IvString id, IvString label, uint32_t row, unsigned index, IvChoice const (&options)[N])
{
    return {.id = id, .label = label, .row = row, .kind = IV_SETTING_CHOICE, .choice_index = index, .choices = {options, N}};
}

void describe_point(Label& label, Point const& point, bool tension)
{
    label.integer(static_cast<int64_t>(point.sample));
    label.append(text(" smp · Value "));
    label.decimal(point.value, 5);
    if (tension) {
        label.append(text(" · Tension "));
        label.decimal(point.tension, 2);
    }
}

struct SelectionBox {
    bool active = false;
    IvPoint begin{}, end{};
    uint32_t modifiers = 0;

    IvRectangle bounds() const
    {
        return {smaller(begin.x, end.x), smaller(begin.y, end.y),
            __builtin_fabs(end.x - begin.x), __builtin_fabs(end.y - begin.y)};
    }
};

enum class Operation { none, drag, delete_points, segment };

class AutomationCurve {
    PointStore store_;
    Array<Point> display_;
    Array<Point> overlay_;
    struct RetainedPoint { Point point; uint64_t revision; };
    Array<RetainedPoint> retained_;
    Array<uint32_t> shadowed_;
    uint32_t overlay_epoch_ = 0;
    Array<uint8_t> selection_;
    Array<uint32_t> delete_ids_;
    Array<ValueGuide> guides_;
    Array<IvTimelineCurvePoint> curve_points_;
    uint32_t curve_generation_ = 0;
    uint32_t curve_active_ = UINT32_MAX;
    bool curve_dirty_ = true;
    StrokeBatch strokes_;
    SelectionBox marquee_{};
    Timeline marquee_time_{};
    double marquee_height_ = 0;
    ValueAxis view_{}, marquee_axis_{};
    Timeline time_{};
    double height_ = 0;
    bool redraw_points_ = true;

    PointDrag drag_;
    EditGesture gesture_{};
    EditRequest request_{};
    Operation operation_ = Operation::none;
    uint32_t current_target_ = 0, next_delete_ = 0, next_companion_ = 0;
    bool menu_open_ = false, menu_tension_ = false;
    uint32_t menu_id_ = 0;
    bool released_ = false, stopped_ = false, finishing_ = false;
    bool remove_on_release_ = false, deletion_sent_ = false;
    bool point_in_accepted_ = false;
    uint64_t accepted_ = 0;
    double next_update_ = 0;
    Point segment_{};
    Label error_{};

    bool editing_pointer() const { return operation_ == Operation::drag && !released_ && !stopped_; }
    bool can_edit_points(Context const& frame) const { return frame.can_edit() && store_.available; }
    IvPoint marker(Point const& point, ValueAxis const& axis) const
    {
        return {time_.x(time_.offset(point.sample)), clamp(axis.y(point.value, height_), 2., larger(2., height_ - 2))};
    }
    bool visible(Point const& point, ValueAxis const& axis) const
    {
        if (!axis.cropped && axis.zoom == 1) return true;
        auto const fraction = axis.fraction(point.value);
        return fraction >= 0 && fraction <= 1;
    }
    bool selected(Point const& point) const
    {
        return point.handle < selection_.size() && selection_[point.handle];
    }
    void set_selected(Point const& point, bool toggle)
    {
        while (point.handle >= selection_.size()) if (!selection_.push(0)) return;
        selection_[point.handle] = toggle ? !selection_[point.handle] : true;
        curve_dirty_ = true;
    }
    void select_all()
    {
        selection_.clear();
        for (auto const& point : display_) set_selected(point, false);
    }
    void select_box()
    {
        auto const bounds = marquee_.bounds();
        auto const toggle = (marquee_.modifiers & (IV_CONTROL | IV_META)) != 0;
        if (!toggle && !(marquee_.modifiers & IV_SHIFT)) {
            selection_.clear();
            curve_dirty_ = true;
        }
        if (bounds.width * bounds.width + bounds.height * bounds.height < 9) return;
        for (auto const& point : display_) {
            auto const offset = marquee_time_.forward_offset(point.sample);
            if (offset < 0 || offset > marquee_time_.span || !visible(point, marquee_axis_)) continue;
            auto const x = marquee_time_.x(offset);
            auto const y = clamp(marquee_axis_.y(point.value, marquee_height_), 2., larger(2., marquee_height_ - 2));
            if (x >= bounds.x && x <= bounds.x + bounds.width && y >= bounds.y && y <= bounds.y + bounds.height)
                set_selected(point, toggle);
        }
    }

    void populate_overlay()
    {
        overlay_.clear();
        if (operation_ != Operation::drag || stopped_ || remove_on_release_) return;
        overlay_.push(drag_.desired);
        for (auto const& companion : drag_.companions) overlay_.push(drag_.companion(companion));
        for (uint32_t i = 0; i < drag_.placement.size; ++i) {
            auto const patch = drag_.placement.segments[i];
            if (auto const* point = find_point(drag_.neighbors, patch.id)) {
                auto styled = *point;
                set_style(styled, patch.style);
                overlay_.push(styled);
            }
        }
        for (uint32_t i = 0; i < drag_.ordered.size(); ++i) {
            auto const id = drag_.ordered[i];
            bool found = false;
            for (auto& existing : overlay_) {
                if (existing.id != id) continue;
                existing.order = i;
                found = true;
                break;
            }
            if (!found) if (auto const* point = find_point(drag_.neighbors, id)) {
                auto reordered = *point;
                reordered.order = i;
                overlay_.push(reordered);
            }
        }
    }

    void refresh_display()
    {
        if (!redraw_points_) return;
        populate_overlay();
        if (++overlay_epoch_ == 0) {
            shadowed_.clear();
            ++overlay_epoch_;
        }
        auto shadow = [&](Point const& point) {
            while (point.handle >= shadowed_.size()) if (!shadowed_.push(0)) return false;
            if (shadowed_[point.handle] == overlay_epoch_) return false;
            shadowed_[point.handle] = overlay_epoch_;
            return true;
        };
        for (auto const& point : overlay_) shadow(point);
        for (auto const& entry : retained_) if (shadow(entry.point)) overlay_.push(entry.point);
        overlay_.sort(point_less);

        display_.clear();
        display_.reserve(store_.accepted.size() + overlay_.size());
        uint32_t next = 0;
        for (auto const& point : store_.accepted) {
            if (point.handle < shadowed_.size() && shadowed_[point.handle] == overlay_epoch_) continue;
            while (next < overlay_.size() && point_less(overlay_[next], point)) display_.push(overlay_[next++]);
            display_.push(point);
        }
        while (next < overlay_.size()) display_.push(overlay_[next++]);
        curve_dirty_ = true;
        redraw_points_ = false;
    }

    void clear_confirmed(uint64_t revision)
    {
        uint32_t kept = 0;
        for (auto const& entry : retained_) if (entry.revision > revision) retained_[kept++] = entry;
        while (retained_.size() > kept) {
            retained_.pop();
            redraw_points_ = true;
        }
    }

    void retain_draft(uint64_t revision)
    {
        if (operation_ != Operation::drag || stopped_ || remove_on_release_ || accepted_ <= revision) return;
        populate_overlay();
        for (auto const& point : overlay_) {
            bool replaced = false;
            for (auto& entry : retained_) if (entry.point.id == point.id) {
                entry = {point, accepted_}; replaced = true; break;
            }
            if (!replaced) retained_.push({point, accepted_});
        }
    }

    void open(Context const& frame, Operation operation)
    {
        gesture_ = frame.begin_edit();
        operation_ = operation;
        request_ = {};
        released_ = stopped_ = finishing_ = remove_on_release_ = deletion_sent_ = false;
        next_delete_ = next_companion_ = 0;
        next_update_ = frame.frame_time() + update_interval_ms;
        accepted_ = 0;
        error_.size = 0;
    }
    void close()
    {
        operation_ = Operation::none;
        request_ = {}; gesture_ = {};
        menu_tension_ = false;
        redraw_points_ = true;
    }
    void stop(bool disconnected)
    {
        if (disconnected) close();
        else if (operation_ != Operation::none) { stopped_ = true; released_ = true; }
        menu_tension_ = false;
        redraw_points_ = true;
    }

    void receive(Context const& frame, IvEditorFeedback& feedback)
    {
        for (auto const& reply : frame.edit_replies()) {
            if (!request_ || reply.request != request_.id) continue;
            request_ = {};
            if (reply.status == IV_EDIT_REJECTED) {
                error_.size = 0; error_.append(reply.error);
                if (editing_pointer()) feedback.flags |= IV_EDITOR_CANCEL_DRAG;
                stopped_ = true;
                redraw_points_ = true;
            } else accepted_ = larger(accepted_, reply.revision);
            if (!finishing_) continue;
            retain_draft(frame.project_revision());
            close();
        }
    }

    static EditRequest send_move(Context const& frame, Point const& point, EditGesture gesture)
    {
        Label sample;
        sample.integer(static_cast<int64_t>(point.sample));
        return frame.send(MovePoint{point.id, 0, point.value, converted_string(sample.view())}, gesture);
    }

    EditRequest next_command(Context const& frame, IvEditorFeedback& feedback)
    {
        if (stopped_) return {};
        switch (operation_) {
            case Operation::none: return {};
            case Operation::delete_points:
                return next_delete_ < delete_ids_.size()
                    ? frame.send(RemovePoint{delete_ids_[next_delete_++]}, gesture_) : EditRequest{};
            case Operation::segment:
                if (deletion_sent_) return {};
                deletion_sent_ = true;
                return frame.send(SetSegment{segment_.id, static_cast<SetSegmentInterpolation>(segment_.interpolation), segment_.tension}, gesture_);
            case Operation::drag:
                break;
        }
        if (remove_on_release_) {
            if (deletion_sent_) return {};
            deletion_sent_ = true;
            return frame.send(RemovePoint{drag_.desired.id}, gesture_);
        }
        if (drag_.companions.size()) {
            if (!released_ || (!next_companion_ && !drag_.dirty())) return {};
            auto const i = next_companion_++;
            if (!i) return send_move(frame, drag_.desired, gesture_);
            if (i <= drag_.companions.size()) return send_move(frame, drag_.companion(drag_.companions[i - 1]), gesture_);
            return {};
        }
        if (!drag_.dirty()) return {};
        auto const now = frame.frame_time();
        if (!released_ && drag_.has_sent && now < next_update_) {
            feedback.flags |= IV_EDITOR_WAKE;
            feedback.wake_after_ms = clamp(next_update_ - now, 0., update_interval_ms);
            return {};
        }
        next_update_ = now + update_interval_ms;
        auto const sent = drag_.send(frame, gesture_);
        if (!sent) {
            error_.append(text("Automation edit could not be submitted"));
            stop(false);
            feedback.flags |= IV_EDITOR_CANCEL_DRAG;
        }
        return sent;
    }
    void dispatch(Context const& frame, IvEditorFeedback& feedback)
    {
        if (operation_ == Operation::none || request_) return;
        request_ = next_command(frame, feedback);
        if (request_ || (operation_ == Operation::drag && !released_ && !stopped_)) return;
        finishing_ = true;
        request_ = frame.finish_edit(gesture_);
        if (!request_) close();
    }

    uint32_t next_point_id() const
    {
        uint32_t id = display_.size() ? display_[0].id : UINT32_MAX;
        for (auto const& point : display_) if (point.id > id) id = point.id;
        ++id;
        while (find_point(display_, id)) ++id;
        return id;
    }

    Point new_point(IvGestureInput const& input, Timeline const& time, ValueAxis const& axis, double height)
    {
        auto const id = next_point_id();
        auto const handle = store_.allocate_handle(id);
        store_.index_handles();
        if (handle < selection_.size()) selection_[handle] = false;
        auto const unsnapped = time.sample_at(input.point.x);
        auto sample = unsnapped;
        if (input.snapping) {
            auto nearest = sample_snap_radius(time);
            for (uint32_t i = 0; i < input.snapping->marks.size; ++i) {
                auto const candidate = static_cast<uint64_t>(input.snapping->marks.data[i]);
                auto const distance = __builtin_fabs(signed_sample_distance(unsnapped, candidate));
                if (distance < nearest) { nearest = distance; sample = candidate; }
            }
        }
        auto const inset = axis.cropped ? 0 : smaller(.5, .5 / height);
        auto const fraction = clamp(1 - input.point.y / height, inset, 1 - inset);
        return {sample, id, 0, static_cast<float>(axis.value_at(fraction)), 1, Interpolation::empty, handle};
    }

    bool begin_drag(Context const& frame, IvGestureInput const& input, bool from_menu = false)
    {
        if (operation_ != Operation::none || marquee_.active || !time_.rate) return false;
        bool const insert = input.id == addition_target;
        auto const* target = insert ? nullptr : store_.point_for_target(display_, input.id);
        bool const tension = !insert && (input.id & 1) == 0;
        if (!insert && (!target || (tension && target->interpolation != Interpolation::bezier))) return false;
        auto const pickup = input_timeline(input);
        auto const height = input.viewport ? input.viewport->height : height_;
        if (pickup.rate != time_.rate) return false;
        auto const axis = input_axis(input);
        auto const point = insert ? new_point(input, pickup, axis, height) : *target;
        if (point.handle == UINT32_MAX || !__builtin_isfinite(point.value)) return false;
        open(frame, Operation::drag);
        drag_.begin(point, display_, {selection_.begin(), selection_.size()}, !insert && !tension && selected(point),
            pickup, axis, height, input, insert, tension);
        point_in_accepted_ = find_point(store_.accepted, point.id) != nullptr;
        current_target_ = input.id;
        redraw_points_ = true;
        if (!from_menu) menu_open_ = false;
        return true;
    }

    Timeline input_timeline(IvGestureInput const& input) const
    {
        if (!input.viewport) return time_;
        auto const& viewport = *input.viewport;
        return {static_cast<uint64_t>(viewport.begin_sample), viewport.begin_fraction,
            viewport.span_samples, viewport.width, viewport.sample_rate};
    }
    ValueAxis input_axis(IvGestureInput const& input) const
    {
        if (!input.viewport || !(input.viewport->flags & IV_GESTURE_VIEW_CURVE)) return view_;
        auto const& viewport = *input.viewport;
        return {viewport.value_pan, viewport.value_zoom, viewport.value_center,
            bool(viewport.flags & IV_GESTURE_VIEW_CROPPED), view_.ruler};
    }
    IvPoint bounded(IvPoint point) const
    {
        return {clamp(point.x, 0., marquee_time_.width), clamp(point.y, 0., marquee_height_)};
    }
    void gesture(Context const& frame, IvGestureInput const& input, IvEditorFeedback& feedback)
    {
        if (input.id == selection_target) {
            if (input.phase == IV_GESTURE_BEGIN) {
                if (editing_pointer()) { feedback.flags |= IV_EDITOR_CANCEL_DRAG; return; }
                marquee_time_ = input_timeline(input);
                if (marquee_time_.rate != time_.rate) { feedback.flags |= IV_EDITOR_CANCEL_DRAG; return; }
                marquee_axis_ = input_axis(input);
                marquee_height_ = input.viewport ? input.viewport->height : height_;
                auto const point = bounded(input.point);
                marquee_ = {true, point, point, input.modifiers};
            } else if (marquee_.active) {
                marquee_.end = bounded(input.point);
                if (input.phase == IV_GESTURE_FINISH) select_box();
                if (input.phase == IV_GESTURE_FINISH || input.phase == IV_GESTURE_CANCEL) marquee_.active = false;
            }
            return;
        }
        if (input.phase == IV_GESTURE_BEGIN) {
            if (!begin_drag(frame, input)) feedback.flags |= IV_EDITOR_CANCEL_DRAG;
            else if (drag_.adding && !request_) dispatch(frame, feedback);
            return;
        }
        if (!editing_pointer() || current_target_ != input.id) return;
        switch (input.phase) {
            case IV_GESTURE_MOVE: drag_.move(input.point); break;
            case IV_GESTURE_MODIFIERS: drag_.preserve_type(input.modifiers & IV_SHIFT); break;
            case IV_GESTURE_WHEEL: drag_.adjust_sensitivity(input.wheel); break;
            case IV_GESTURE_FINISH:
                if (!drag_.tension) {
                    feedback.cursor = IV_CURSOR_SAMPLE;
                    feedback.sample = static_cast<int64_t>(drag_.desired.sample);
                    feedback.position.y = clamp(marker(drag_.desired, view_).y / height_, 0., 1.);
                }
                remove_on_release_ = (input.modifiers & IV_SHIFT) && drag_.delete_click && !drag_.moved;
                released_ = true;
                break;
            case IV_GESTURE_CANCEL: released_ = true; break;
            default: break;
        }
        redraw_points_ = true;
    }

    void remove_points(Context const& frame, Point const* one = nullptr)
    {
        if (operation_ != Operation::none) return;
        delete_ids_.clear();
        if (one) delete_ids_.push(one->id);
        else for (auto const& point : display_) if (selected(point)) delete_ids_.push(point.id);
        if (delete_ids_.size()) open(frame, Operation::delete_points);
    }
    void change_segment(Context const& frame, Point const& point, Interpolation kind, float tension)
    {
        if (operation_ != Operation::none) return;
        open(frame, Operation::segment);
        segment_ = point;
        segment_.interpolation = kind; segment_.tension = tension;
    }

    void control(Context const& frame, IvControlInput const& input)
    {
        auto const before = view_;
        if (input.value_kind == IV_CONTROL_STRING && equal(input.id, text("display"))) {
            auto const index = choice_index(display_choices, input.string);
            if (index >= 0) {
                view_.cropped = index != 0;
                view_.reset();
            }
        } else if (input.value_kind == IV_CONTROL_STRING && equal(input.id, text("ruler"))) {
            auto const index = choice_index(ruler_choices, input.string);
            if (index >= 0) view_.ruler = static_cast<Ruler>(index);
        }
        if (before != view_) frame.request_save();
        auto const* point = menu_open_ && store_.available ? find_point(display_, menu_id_) : nullptr;
        if (!point) return;
        if (input.value_kind == IV_CONTROL_STRING && equal(input.id, text("segment"))) {
            if (auto const index = choice_index(segment_choices, input.string); index >= 0)
                change_segment(frame, *point, static_cast<Interpolation>(index), point->tension);
        } else if (equal(input.id, text("remove"))) {
            remove_points(frame, point);
            menu_open_ = false;
        } else if (equal(input.id, text("tension"))) {
            if (input.phase == IV_CONTROL_FINISH && menu_tension_) {
                released_ = true;
                menu_tension_ = false;
            } else if (input.value_kind == IV_CONTROL_NUMBER && input.phase != IV_CONTROL_FINISH) {
                if (input.phase == IV_CONTROL_COMMIT && !menu_tension_)
                    change_segment(frame, *point, Interpolation::bezier, static_cast<float>(clamp(input.value, 0., 1.)));
                else {
                    if (!menu_tension_) menu_tension_ = begin_drag(frame, {.id = tension_target(*point)}, true);
                    if (menu_tension_) { drag_.set_tension(input.value); redraw_points_ = true; }
                }
            }
        }
    }

    void editor_action(Context const& frame, IvEditorInput const& input, IvEditorFeedback& feedback)
    {
        switch (input.kind) {
            case IV_EDITOR_INTERRUPT: {
                if (editing_pointer() || marquee_.active) feedback.flags |= IV_EDITOR_CANCEL_DRAG;
                bool const disconnected = input.action == IV_INTERRUPT_DISCONNECTED;
                stop(disconnected);
                marquee_.active = menu_open_ = false;
                if (disconnected) { selection_.clear(); retained_.clear(); redraw_points_ = true; }
                error_.size = 0;
                break;
            }
            case IV_EDITOR_DISMISS_CONTEXT:
                if (menu_tension_) {
                    if (input.action) stop(false);
                    else released_ = true;
                }
                menu_open_ = menu_tension_ = false;
                break;
            case IV_EDITOR_VALUE_VIEW: {
                if (editing_pointer() || marquee_.active) break;
                auto const before = view_;
                if (input.action == IV_VALUE_RESET) view_.reset();
                else if (input.action == IV_VALUE_ZOOM && input.value > 0 && __builtin_isfinite(input.value))
                    view_.zoom_by(input.value, 1 - input.point.y);
                else if (input.action == IV_VALUE_PAN && __builtin_isfinite(input.value)) view_.pan_by(input.value);
                if (before != view_) frame.request_save();
                break;
            }
            case IV_EDITOR_CONTEXT:
                if (can_edit_points(frame)) if (auto const* point = store_.point_for_target(display_, input.target)) {
                    menu_open_ = true;
                    menu_id_ = point->id;
                }
                break;
            case IV_EDITOR_ITEMS:
                if (can_edit_points(frame)) {
                    if (input.action == IV_ITEMS_SELECT_ALL) select_all();
                    else remove_points(frame);
                }
                break;
            default: break;
        }
    }

    void inputs(Context const& frame, IvEditorFeedback& feedback)
    {
        frame.visit_interactions(
            [&](IvGestureInput const& input) {
                if (input.phase == IV_GESTURE_BEGIN || (input.id == selection_target && input.phase == IV_GESTURE_FINISH)) refresh_display();
                if (can_edit_points(frame)) gesture(frame, input, feedback);
            }, [&](IvControlInput const& input) {
                if (menu_open_) refresh_display();
                if (frame.can_edit()) control(frame, input);
            }, [&](IvEditorInput const& input) {
                if (input.kind == IV_EDITOR_CONTEXT || input.kind == IV_EDITOR_ITEMS) refresh_display();
                editor_action(frame, input, feedback);
            });
    }

    double draw_guides(Context const& frame)
    {
        auto const& axis = view_;
        value_guides(guides_, axis, height_);
        auto const mode = frame.lane_mode();
        bool const labels = mode == IV_LANE_EDITABLE || (mode == IV_LANE_COMPACT && height_ > 16);
        IvText marks[101];
        Label strings[101];
        uint32_t count = 0, length = 0;
        auto const label_at = [&](double value, double y, uint32_t ink, uint32_t id) {
            if (!labels) return;
            auto& string = strings[count];
            string.decimal(value, 6);
            length = larger(length, string.size);
            marks[count++] = {id, ink, {4, y >= 12 ? y - 3 : smaller(height_ - 1, y + 10)}, string.view()};
        };
        double smallest_step = 1;
        for (auto const& guide : guides_) {
            auto const bits = __builtin_bit_cast(uint32_t, static_cast<float>(guide.value)) & 0x7fffffff;
            if (!bits) continue;
            auto const exponent = bits >> 23;
            auto const mantissa = (bits & 0x7fffff) | (exponent ? 0x800000 : 0);
            smallest_step = smaller(smallest_step, power_of_two(static_cast<int>(exponent ? exponent : 1) - 150 + __builtin_ctz(mantissa)));
        }
        auto depth = [&](ValueGuide const& entry) {
            if (!entry.value) return 0u;
            auto const index = entry.value / smallest_step;
            if (__builtin_fabs(entry.value) == 1 || index / 4 == __builtin_trunc(index / 4)) return 1u;
            return index / 2 == __builtin_trunc(index / 2) ? 2u : 3u;
        };
        double gaps[]{1e30, 1e30, 1e30, 1e30};
        for (unsigned i = 1; i < guides_.size(); ++i)
            for (unsigned j = 0; j < i; ++j) {
                auto const level = larger(depth(guides_[i]), depth(guides_[j]));
                gaps[level] = smaller(gaps[level], guides_[j].y - guides_[i].y);
            }
        auto const strength = [](double gap) { auto const ratio = 24 / gap; return 1 / (1 + ratio * ratio); };
        double const overall = smaller(smaller(gaps[0], gaps[1]), smaller(gaps[2], gaps[3]));
        double opacity[4];
        double gap = 1e30, level_opacity = .15 * (.6 + .4 * strength(overall));
        for (unsigned i = 0; i < 4; ++i) {
            gap = smaller(gap, gaps[i]); opacity[i] = level_opacity * strength(gap); level_opacity *= .82;
        }
        for (unsigned i = 0; i < guides_.size(); ++i) {
            auto const& guide = guides_[i];
            strokes_.line(1 + i, IV_INK_FOREGROUND, {0, guide.y}, {time_.width, guide.y}, opacity[depth(guide)]);
            if (!(editing_pointer() && drag_.snapped_vertical && guide.value == drag_.snapped_value))
                label_at(guide.value, guide.y, IV_INK_MUTED, 201 + i);
        }
        if (editing_pointer() && !drag_.tension) {
            auto const at = marker(drag_.desired, axis);
            if (drag_.snapped_vertical) {
                auto const y = axis.y(drag_.snapped_value, height_);
                if (y >= 0 && y <= height_) {
                    strokes_.line(401, IV_INK_HIGHLIGHT, {0, y}, {time_.width, y}, .35);
                    strokes_.line(402, IV_INK_HIGHLIGHT, {0, y}, {time_.width, y}, 1,
                        {IV_STROKE_FADE, 0, 1, 1, {at.x - 144, y}, {at.x + 144, y}});
                    label_at(drag_.snapped_value, y, IV_INK_HIGHLIGHT, 403);
                }
            }
            if (drag_.snapped_time) {
                auto const x = time_.x(time_.offset(drag_.snapped_sample));
                if (x >= 0 && x <= time_.width)
                    strokes_.line(404, IV_INK_HIGHLIGHT, {x, 0}, {x, height_}, 1,
                        {IV_STROKE_FADE, 0, 1, 1, {x, at.y - 144}, {x, at.y + 144}});
            }
        }
        frame.draw_texts({marks, count});
        return length ? 9 + 6 * length : 0;
    }

    bool refresh_curve()
    {
        auto const active = editing_pointer() ? drag_.desired.handle : UINT32_MAX;
        if (!curve_dirty_ && curve_active_ == active) return true;
        auto const count = display_.size();
        if (!curve_points_.reserve(count)) return false;
        curve_points_.clear();
        auto group_end = [&](uint32_t begin) {
            auto end = begin + 1;
            while (end < count && display_[end].sample == display_[begin].sample) ++end;
            return end;
        };
        auto previous = count ? count - 1 : 0;
        for (uint32_t begin = 0; begin < count;) {
            auto const end = group_end(begin);
            auto const next = end == count ? 0 : end;
            auto const next_end = group_end(next);
            auto const& first = display_[end - 1];
            auto const& last = display_[next];
            double start_delta = 0, end_delta = 0;
            if (begin != next && first.interpolation == Interpolation::bezier && first.tension) {
                auto const length = static_cast<double>(last.sample - first.sample);
                auto const slope = (double(last.value) - first.value) / length;
                auto const& before = display_[previous];
                if (before.interpolation == Interpolation::bezier && display_[begin].value == first.value) {
                    auto const before_length = static_cast<double>(first.sample - before.sample);
                    start_delta = iv::automation::curve_tangent((double(first.value) - before.value) / before_length,
                        slope, before_length, length) * length;
                }
                auto const& outgoing = display_[next_end - 1];
                if (outgoing.interpolation == Interpolation::bezier && outgoing.value == last.value) {
                    auto const& after = display_[next_end == count ? 0 : next_end];
                    auto const after_length = static_cast<double>(after.sample - last.sample);
                    end_delta = iv::automation::curve_tangent(slope, (double(after.value) - last.value) / after_length,
                        length, after_length) * length;
                }
            }
            for (auto index = begin; index < end; ++index) {
                auto const& point = display_[index];
                bool const outgoing = index + 1 == end;
                bool const tension = outgoing && point.interpolation == Interpolation::bezier;
                curve_points_.push({.sample = static_cast<int64_t>(point.sample),
                    .start_delta = outgoing ? start_delta : 0, .end_delta = outgoing ? end_delta : 0,
                    .value = point.value, .tension = point.tension, .target = point_target(point),
                    .mark = point_mark_base + 2 * point.handle,
                    .tension_target = tension ? tension_target(point) : 0,
                    .tension_mark = tension ? tension_mark_base + 2 * point.handle : 0,
                    .interpolation = static_cast<uint32_t>(point.interpolation),
                    .flags = (selected(point) ? uint32_t(IV_CURVE_SELECTED) : 0u)
                        | (point.handle == active ? uint32_t(IV_CURVE_ACTIVE) : 0u)});
            }
            previous = end - 1;
            begin = end;
        }
        curve_active_ = active;
        curve_dirty_ = false;
        ++curve_generation_;
        return true;
    }

    void draw_curve(Context const& frame)
    {
        auto const& axis = view_;
        if (!refresh_curve()) {
            frame.set_status(text("Could not allocate Automation Curve view"));
            return;
        }
        frame.set_timeline_curve({.points = {curve_points_.begin(), curve_points_.size()},
            .pan = axis.pan, .zoom = axis.zoom, .center = axis.center,
            .generation = curve_generation_,
            .flags = (axis.cropped ? uint32_t(IV_CURVE_CROPPED) : 0u)
                | (can_edit_points(frame) ? uint32_t(IV_CURVE_EDITABLE) : 0u)
                | (editing_pointer() || marquee_.active ? uint32_t(IV_CURVE_VIEWPORT_DRAWING) : 0u),
            .selection_target = selection_target, .addition_target = addition_target,
            .minimum_input = text("min"), .maximum_input = text("max")});
        if (marquee_.active) {
            auto box = marquee_.bounds();
            auto project_x = [&](double x) {
                auto const offset = x / marquee_time_.width * marquee_time_.span;
                auto const whole = __builtin_floor(offset);
                auto const sample = marquee_time_.origin + sample_from_double(whole);
                return time_.x(time_.offset(sample) + (offset - whole) + marquee_time_.fraction);
            };
            auto const first = project_x(box.x), last = project_x(box.x + box.width);
            box.x = smaller(first, last);
            box.width = __builtin_fabs(last - first);
            if (marquee_axis_ == axis) {
                box.y *= height_ / marquee_height_;
                box.height *= height_ / marquee_height_;
            } else {
                auto project_y = [&](double y) {
                    auto const inset = marquee_axis_.cropped ? 0 : smaller(.5, .5 / marquee_height_);
                    auto const fraction = clamp(1 - y / marquee_height_, inset, 1 - inset);
                    return clamp(axis.y(marquee_axis_.value_at(fraction), height_), 0., height_);
                };
                auto const top = project_y(box.y), bottom = project_y(box.y + box.height);
                box.y = top;
                box.height = larger(0., bottom - top);
            }
            IvShape const shape{selection_mark, IV_SHAPE_RECTANGLE, IV_INK_SELECTION, IV_INK_SELECTION,
                0, {.rectangle = box}, .25};
            frame.draw_shapes({&shape, 1});
        }
    }

    void draw_panels(Context const& frame) const
    {
        if (!frame.can_edit()) return;
        IvSetting const settings[]{
            choice(text("display"), text("Display"), 0, view_.cropped, display_choices),
            choice(text("ruler"), text("Value snapping"), 1, static_cast<unsigned>(view_.ruler), ruler_choices)
        };
        frame.set_settings_panel({text("Scale"), {settings, 2}});
        auto const* point = menu_open_ ? find_point(display_, menu_id_) : nullptr;
        if (!point) return;
        IvSetting menu[4]{};
        unsigned size = 0;
        menu[size++] = choice(text("segment"), text("Segment"), 0, static_cast<unsigned>(point->interpolation), segment_choices);
        if (point->interpolation == Interpolation::bezier) {
            menu[size++] = {.id = text("tension-label"), .label = text("Tension"), .row = 1, .kind = IV_SETTING_TEXT};
            menu[size++] = {.id = text("tension"), .label = text("Bézier tension"), .row = 1, .kind = IV_SETTING_KNOB,
                .value = point->tension, .min = 0, .max = 1, .default_value = 1, .step = .05};
        }
        auto const action_row = size == 1 ? 1u : 2u;
        menu[size++] = {.id = text("remove"), .label = text("Delete point"), .row = action_row, .kind = IV_SETTING_ACTION};
        frame.set_context_menu({text("Automation segment"), {menu, size}});
    }

public:
    void present(Context& frame)
    {
        auto const bounds = frame.content_bounds();
        auto const timeline = frame.timeline_view();
        IvEditorFeedback feedback{};
        height_ = bounds.height;
        time_ = {static_cast<uint64_t>(timeline.begin_sample), timeline.begin_fraction,
            timeline.span_samples, bounds.width, timeline.sample_rate};
        if (!(time_.span > 0) || !time_.rate) {
            frame.set_status(text("Waiting for Automation Curve timeline"));
            return;
        }
        receive(frame, feedback);
        if (store_.reconcile(frame, time_.rate)) {
            redraw_points_ = true;
            if (!store_.available) {
                if (editing_pointer() || marquee_.active) feedback.flags |= IV_EDITOR_CANCEL_DRAG;
                stop(false);
                marquee_.active = menu_open_ = false;
                retained_.clear();
            } else if (editing_pointer()) {
                bool const exists = find_point(store_.accepted, drag_.desired.id) != nullptr;
                if (point_in_accepted_ && !exists) { stop(false); feedback.flags |= IV_EDITOR_CANCEL_DRAG; }
                point_in_accepted_ |= exists;
            }
        }
        clear_confirmed(frame.project_revision());
        if (!frame.can_edit() && editing_pointer()) {
            stop(false);
            feedback.flags |= IV_EDITOR_CANCEL_DRAG;
        }
        refresh_display();
        inputs(frame, feedback);
        dispatch(frame, feedback);
        refresh_display();

        if (can_edit_points(frame)) feedback.flags |= IV_EDITOR_ITEM_EDITING;
        if (!editing_pointer() && !marquee_.active) feedback.flags |= IV_EDITOR_VALUE_NAVIGATION;
        feedback.flags |= IV_EDITOR_SECONDS_RULER;

        strokes_.clear();
        feedback.ruler_label_inset = draw_guides(frame);
        draw_curve(frame);
        strokes_.submit(frame);
        draw_panels(frame);
        Label hint;
        if (editing_pointer() && !menu_tension_) {
            describe_point(hint, drag_.desired, true);
            if (!drag_.tension) {
                hint.append(text(" · "));
                hint.decimal(drag_.sensitivity(), 6);
                hint.append(text("×"));
                feedback.shortcuts = sensitivity_shortcut;
            }
            feedback.hint = hint.view();
        }
        frame.set_editor_feedback(feedback);
        if (error_.size) frame.set_status(error_.view());
        else if (!store_.available) frame.set_status(text("Waiting for accepted Automation Curve arguments"));
        else if (!frame.can_edit()) frame.set_status(text("Read-only curve"));
    }

    int32_t save(char* bytes, uint32_t capacity) const
    {
        if (view_ == ValueAxis{}) {
            if (capacity < 4) return -1;
            auto const value = text("null");
            for (unsigned i = 0; i < 4; ++i) bytes[i] = value.data[i];
            return 4;
        }
        IvJsonWriter writer{bytes, capacity, 0, 0, false, {}};
        iv_json_object(&writer);
        if (view_.cropped) iv_json_string(&writer, "display", "cropped");
        if (view_.ruler != Ruler::automatic) {
            char const* values[]{"auto", "whole", "half", "off"};
            iv_json_string(&writer, "ruler", values[static_cast<unsigned>(view_.ruler)]);
        }
        if (view_.pan) iv_json_number(&writer, "pan", view_.pan);
        if (view_.zoom != 1) {
            iv_json_object_member(&writer, "zoom");
            iv_json_number(&writer, "factor", view_.zoom);
            iv_json_number(&writer, "center", view_.center);
            iv_json_end(&writer);
        }
        iv_json_end(&writer);
        return iv_json_finish(&writer);
    }

    int32_t restore(IvString state)
    {
        view_ = {};
        if (!state.size) return 0;
        IvJsonReader const object{state.data, state.data + state.size};
        view_.cropped = iv_json_token_is(iv_json_find_field(object, "display"), "cropped");
        auto const ruler = iv_json_find_field(object, "ruler");
        if (iv_json_token_is(ruler, "whole")) view_.ruler = Ruler::whole;
        else if (iv_json_token_is(ruler, "half")) view_.ruler = Ruler::half;
        else if (iv_json_token_is(ruler, "off")) view_.ruler = Ruler::off;
        view_.pan = iv_json_read_number(iv_json_find_field(object, "pan"), 0);
        auto const zoom = iv_json_find_field(object, "zoom");
        view_.zoom = clamp(iv_json_read_number(iv_json_find_field(zoom, "factor"), 1), 0x1p-20, 0x1p20);
        view_.center = view_.cropped || view_.zoom <= 1 ? .5
            : ValueAxis::crop_center(iv_json_read_number(iv_json_find_field(zoom, "center"), .5), view_.zoom);
        return 0;
    }
};

} // namespace
} // namespace automation_presentation

IV_PRESENTATION_CLASS(automation_presentation::AutomationCurve, IV_PRESENTATION_TIMELINE | IV_PRESENTATION_TIMELINE_SNAPPING,
    "M2 12C5 12 5 4 14 4M2 10v4m12-12v4")
