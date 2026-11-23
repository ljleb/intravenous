#pragma once

#include "automation_curve_model.hpp"

namespace automation_presentation {

constexpr uint32_t selection_target = 1;
constexpr uint32_t addition_target = 2;
constexpr double update_interval_ms = 90;

inline uint32_t point_target(Point const& point) { return point.handle * 2 + 3; }
inline uint32_t tension_target(Point const& point) { return point_target(point) + 1; }
inline uint32_t target_handle(uint32_t target) { return (target - 3) / 2; }

template<class T> Point const* find_point(T const& points, uint32_t id)
{
    for (auto const& point : points) if (point.id == id) return &point;
    return nullptr;
}

inline uint32_t insertion_slot(Array<Point> const& points, Point const& point)
{
    uint32_t begin = 0, end = points.size();
    while (begin < end) {
        auto const center = begin + (end - begin) / 2;
        if (point_less(point, points[center])) end = center;
        else begin = center + 1;
    }
    return begin;
}

class PointStore {
    struct Handle { uint32_t id, value; };
    Array<uint32_t> ids_;
    Array<Handle> handles_;
    uint32_t indexed_handles_ = 0;
    uint32_t generation_ = UINT32_MAX;
    uint32_t rate_ = 0;

public:
    Array<Point> accepted;
    bool available = false;

    uint32_t allocate_handle(uint32_t id)
    {
        uint32_t low = 0, high = indexed_handles_;
        while (low < high) {
            auto const mid = low + (high - low) / 2;
            if (handles_[mid].id < id) low = mid + 1;
            else high = mid;
        }
        if (low < indexed_handles_ && handles_[low].id == id) return handles_[low].value;
        auto const next = ids_.size();
        if (next >= (UINT32_MAX - 4) / 2 || !ids_.reserve(next + 1) || !handles_.reserve(next + 1)) return UINT32_MAX;
        ids_.push(id);
        handles_.push({id, next});
        return next;
    }
    void index_handles()
    {
        if (indexed_handles_ == handles_.size()) return;
        handles_.sort([](auto const& a, auto const& b) { return a.id < b.id; });
        indexed_handles_ = handles_.size();
    }
    Point const* point_for_target(Array<Point> const& visible, uint32_t target) const
    {
        if (target <= addition_target || target_handle(target) >= ids_.size()) return nullptr;
        return find_point(visible, ids_[target_handle(target)]);
    }
    bool reconcile(Context const& frame, uint32_t rate)
    {
        auto const generation = frame.construction_snapshot();
        if (generation == generation_ && rate == rate_) return false;
        generation_ = generation;
        rate_ = rate;
        accepted.clear();
        available = frame.construction_argument_state<AcceptedPoints>() == IV_ARGUMENT_OMITTED;
        if (auto const* input = frame.construction_argument<AcceptedPoints>(); input && accepted.reserve(input->size)) {
            available = true;
            for (auto const& point : *input) {
                auto const handle = allocate_handle(point.id);
                if (handle == UINT32_MAX) { available = false; break; }
                accepted.push({point_sample(point, rate), point.id, point.order, point.value, point.tension, point.interpolation, handle});
            }
        }
        if (!available) accepted.clear();
        index_handles();
        accepted.sort(point_less);
        return true;
    }
};

struct SegmentStyle {
    Interpolation interpolation;
    float tension;
};
inline SegmentStyle style_of(Point const& point) { return {point.interpolation, point.tension}; }
inline bool same_style(SegmentStyle a, SegmentStyle b) { return a.interpolation == b.interpolation && a.tension == b.tension; }
inline void set_style(Point& point, SegmentStyle style) { point.interpolation = style.interpolation; point.tension = style.tension; }
struct SegmentChange { uint32_t id; SegmentStyle style; };
struct Placement {
    SegmentChange segments[2]{};
    uint32_t size = 0;

    SegmentStyle style(Point const& point) const
    {
        for (uint32_t i = 0; i < size; ++i)
            if (segments[i].id == point.id) return segments[i].style;
        return style_of(point);
    }
    void set(Point const& point, SegmentStyle value)
    {
        for (uint32_t i = 0; i < size; ++i) {
            if (segments[i].id != point.id) continue;
            if (same_style(value, style_of(point))) segments[i] = segments[--size];
            else segments[i].style = value;
            return;
        }
        if (!same_style(value, style_of(point)) && size < 2) segments[size++] = {point.id, value};
    }
    bool same(Placement const& other) const
    {
        if (size != other.size) return false;
        for (uint32_t i = 0; i < size; ++i)
            if (segments[i].id != other.segments[i].id || !same_style(segments[i].style, other.segments[i].style)) return false;
        return true;
    }
};

enum class Attachment { none, left, right, nearest };

inline Placement place_point(Array<Point> const& neighbors, Point const* original, Point& point,
    uint32_t slot, Attachment attachment)
{
    Placement changes;
    auto const size = neighbors.size();
    if (!size) return changes;
    uint32_t const origin = original ? insertion_slot(neighbors, *original) : 0;
    auto const& before = neighbors[prior(slot, size)];
    auto const& after = neighbors[slot % size];
    auto const* old_before = original ? &neighbors[prior(origin, size)] : nullptr;
    auto const* old_after = original ? &neighbors[origin % size] : nullptr;
    auto const empty = Interpolation::empty;
    SegmentStyle constexpr bezier{Interpolation::bezier, 1};
    if (original && original->interpolation == empty && old_before->interpolation != empty
        && (origin == size || old_after->sample != original->sample))
        changes.set(*old_before, {empty, old_before->tension});

    auto const incoming = changes.style(before);
    if (incoming.interpolation != empty) {
        set_style(point, original && original->interpolation != empty && old_before == &before && old_after == &after
            ? style_of(*original) : incoming);
        return changes;
    }
    if (attachment == Attachment::none) return changes;
    auto const left = point.sample - before.sample;
    auto const right = after.sample - point.sample;
    bool const nearest_left = left < right || (left == 0 && right == 0 && slot == size);
    if (attachment == Attachment::left || (attachment == Attachment::nearest && nearest_left)) {
        auto edge = prior(slot, size);
        while (edge && neighbors[edge - 1].sample == before.sample) --edge;
        bool const departing = &before == old_before && before.interpolation != incoming.interpolation;
        auto const style = departing ? style_of(before) : changes.style(neighbors[prior(edge, size)]);
        changes.set(before, style.interpolation == empty ? bezier : style);
        set_style(point, {empty, original && original->interpolation == empty ? original->tension : 1});
    } else {
        auto end = slot % size;
        while (end + 1 < size && neighbors[end + 1].sample == after.sample) ++end;
        auto const& edge = neighbors[end];
        auto const changed = changes.style(edge);
        auto const style = &edge == old_before && changed.interpolation != edge.interpolation ? style_of(edge)
            : original && original->interpolation != empty && old_before->interpolation == empty && &after == old_after
                ? style_of(*original) : changed;
        set_style(point, style.interpolation == empty ? bezier : style);
    }
    return changes;
}

struct TimeGuide { double value; uint64_t sample; bool grid; };
inline double signed_sample_distance(uint64_t from, uint64_t to)
{
    return static_cast<double>(static_cast<int64_t>(to - from));
}
inline double sample_snap_radius(Timeline const& time) { return 6 * time.span / time.width; }

class PointDrag {
    Array<TimeGuide> time_guides_;
    Array<ValueGuide> value_guides_;
    double value_position_ = 0, value_anchor_ = 0, value_plateau_ = 0, value_anchor_plateau_ = 0;
    double time_plateau_ = 0;
    int sensitivity_ = 0;
    uint32_t initial_slot_ = 0, previous_slot_ = 0;
    bool positioned_ = false;
    Attachment attachment_ = Attachment::none;
    Placement base_placement_{};
    int64_t min_travel_ = INT64_MIN, max_travel_ = INT64_MAX;

    void time_guides(Timeline const& time, IvSnapCandidates const* snapping)
    {
        time_guides_.clear();
        auto add = [&](uint64_t sample, bool grid) {
            auto const scale = time.width / time.span;
            time_guides_.push({static_cast<double>(sample - initial.sample) * scale, sample, grid});
            time_guides_.push({-static_cast<double>(initial.sample - sample) * scale, sample, grid});
        };
        uint64_t previous = 0;
        bool first = true;
        if (!companions.size()) {
            for (auto const& point : neighbors) {
                if (first || point.sample != previous) add(point.sample, false);
                first = false;
                previous = point.sample;
            }
        }
        if (snapping) {
            for (uint32_t i = 0; i < snapping->marks.size; ++i) add(static_cast<uint64_t>(snapping->marks.data[i]), true);
            if (snapping->has_playhead) add(static_cast<uint64_t>(snapping->playhead), true);
        }
        time_guides_.sort([](auto const& a, auto const& b) { return a.value < b.value; });
        uint32_t count = 0;
        for (auto const guide : time_guides_) {
            if (count && time_guides_[count - 1].value == guide.value
                && time_guides_[count - 1].sample == guide.sample) time_guides_[count - 1].grid |= guide.grid;
            else time_guides_[count++] = guide;
        }
        while (time_guides_.size() > count) time_guides_.pop();
    }

    static bool is_selected(Point const& point, Span<uint8_t const> selected)
    {
        return point.handle < selected.size && selected.data[point.handle];
    }

    void movement_limits(Array<Point> const& points, Span<uint8_t const> selected)
    {
        min_travel_ = INT64_MIN;
        max_travel_ = INT64_MAX;
        if (!companions.size() || !neighbors.size()) return;
        auto const size = points.size();
        for (uint32_t i = 0; i < size; ++i) {
            if (!is_selected(points[i], selected)) continue;
            auto const previous = prior(i, size), next = (i + 1) % size;
            if (!is_selected(points[previous], selected)) {
                auto travel = points[i].sample - points[previous].sample;
                bool const keep_order = points[previous].order > points[i].order
                    || (points[previous].order == points[i].order && points[previous].id > points[i].id);
                if ((i != 0 || travel != 0) && travel <= uint64_t(INT64_MAX))
                    min_travel_ = larger(min_travel_, static_cast<int64_t>(keep_order) - static_cast<int64_t>(travel));
            }
            if (!is_selected(points[next], selected)) {
                auto travel = points[next].sample - points[i].sample;
                bool const keep_order = points[i].order > points[next].order
                    || (points[i].order == points[next].order && points[i].id > points[next].id);
                if ((i != size - 1 || travel != 0) && travel <= uint64_t(INT64_MAX))
                    max_travel_ = smaller(max_travel_, static_cast<int64_t>(travel) - static_cast<int64_t>(keep_order));
            }
        }
    }

    void locate(uint64_t sample, double plateau, double pixels)
    {
        positioned_ = true;
        desired.sample = sample;
        ordered.clear();
        uint32_t slot = insertion_slot(neighbors, desired);
        auto start = slot, end = slot;
        while (start && neighbors[start - 1].sample == sample) --start;
        while (end < neighbors.size() && neighbors[end].sample == sample) ++end;
        if (start != end) {
            auto const group_order = pixels == 0 ? slot - start : plateau < 0 ? 0 : end - start;
            desired.order = group_order;
            slot = start + group_order;
            for (uint32_t i = start; i <= end; ++i) {
                if (i == slot) ordered.push(initial.id);
                if (i < end) ordered.push(neighbors[i].id);
            }
        } else desired.order = initial.order;
        if (!adding && slot != previous_slot_) {
            auto const distance = static_cast<int64_t>(sample - previous_sample);
            bool const forward = distance == 0 ? slot > previous_slot_ : distance > 0;
            bool const crossed = forward ? previous_slot_ != neighbors.size() || slot != 0
                : previous_slot_ != 0 || slot != neighbors.size();
            if (crossed) attachment_ = forward ? Attachment::left : Attachment::right;
        }
        previous_slot_ = slot;
        previous_sample = sample;
    }

public:
    Point initial{}, desired{};
    Point sent{};
    uint64_t previous_sample = 0, snapped_sample = 0;
    IvPoint pickup{}, last{};
    Timeline time{};
    ValueAxis axis{};
    double height = 0, snapped_value = 0;
    Placement placement{}, sent_placement{};
    Array<Point> neighbors, companions;
    Array<uint32_t> ordered, sent_ordered;
    bool adding = false, tension = false, moved = false, delete_click = false, preserve = false;
    bool has_sent = false, snapped_time = false, snapped_vertical = false;

    void begin(Point point, Array<Point> const& points, Span<uint8_t const> selected, bool group,
        Timeline timeline, ValueAxis const& value_axis, double lane_height, IvGestureInput const& input, bool insert, bool edit_tension)
    {
        initial = desired = sent = point;
        previous_sample = point.sample;
        time = timeline; axis = value_axis; height = lane_height;
        pickup = last = input.point;
        adding = insert; tension = edit_tension; moved = false;
        positioned_ = false;
        preserve = (input.modifiers & IV_SHIFT) != 0;
        delete_click = !adding && !tension && preserve;
        has_sent = !adding;
        snapped_time = snapped_vertical = false;
        placement = sent_placement = base_placement_ = {};
        ordered.clear(); sent_ordered.clear(); neighbors.clear(); companions.clear();
        for (auto const& other : points) {
            if (other.id == point.id) continue;
            neighbors.push(other);
            if (group && is_selected(other, selected)) companions.push(other);
        }
        initial_slot_ = previous_slot_ = insertion_slot(neighbors, point);
        attachment_ = insert ? Attachment::nearest : point.interpolation != Interpolation::empty ? Attachment::right
            : neighbors.size() && neighbors[prior(initial_slot_, neighbors.size())].interpolation != Interpolation::empty
                ? Attachment::left : Attachment::none;
        if (insert) {
            if (!preserve) base_placement_ = placement = place_point(neighbors, nullptr, desired, initial_slot_, attachment_);
            initial = desired;
            if (input.snapping) {
                for (uint32_t i = 0; i < input.snapping->marks.size; ++i)
                    if (static_cast<uint64_t>(input.snapping->marks.data[i]) == point.sample) snapped_time = true;
                if (input.snapping->has_playhead && static_cast<uint64_t>(input.snapping->playhead) == point.sample) snapped_time = true;
                snapped_sample = point.sample;
            }
        }
        if (tension) return;
        time_guides(time, input.snapping);
        value_guides(value_guides_, axis, height);
        movement_limits(points, selected);
        time_plateau_ = 0;
        if (neighbors.size() && !companions.size()) {
            if (initial_slot_ && neighbors[initial_slot_ - 1].sample == point.sample) time_plateau_ = 6;
            else if (initial_slot_ < neighbors.size() && neighbors[initial_slot_].sample == point.sample) time_plateau_ = -6;
        }
        value_position_ = -pickup.y; value_anchor_ = point.value;
        value_plateau_ = value_anchor_plateau_ = 0; sensitivity_ = 0;
    }

    double sensitivity() const { return power_of_two(sensitivity_); }
    void adjust_sensitivity(double wheel)
    {
        if (tension || !wheel) return;
        sensitivity_ = clamp(sensitivity_ + (wheel < 0 ? 1 : -1), -10, 10);
        value_position_ = -last.y; value_anchor_ = desired.value; value_anchor_plateau_ = value_plateau_;
    }
    void preserve_type(bool next)
    {
        if (tension) return;
        if (!next) delete_click = false;
        if (preserve == next) return;
        preserve = next;
        update_placement();
    }
    void update_placement()
    {
        set_style(desired, style_of(initial));
        placement = base_placement_;
        if (positioned_ && !preserve && !companions.size()
            && (!adding || desired.sample != initial.sample || previous_slot_ != initial_slot_))
            placement = place_point(neighbors, adding ? nullptr : &initial, desired, previous_slot_, attachment_);
    }
    void move(IvPoint position)
    {
        if (tension) {
            set_tension(initial.tension + (position.x - pickup.x) / 100);
            last = position;
            return;
        }
        auto const dx = position.x - pickup.x, dy = position.y - pickup.y;
        if (delete_click && !moved && dx * dx + dy * dy <= 25) return;
        if (position.x != last.x) {
            auto const result = at_detents(dx, 0, 1, time_plateau_, time_guides_);
            auto const target = result.snapped ? time_guides_[result.index].sample
                : initial.sample + sample_from_double(round_sample(result.value / time.width * time.span));
            auto const delta = static_cast<int64_t>(target - initial.sample);
            auto const sample = companions.size() ? initial.sample + static_cast<uint64_t>(clamp(delta, min_travel_, max_travel_)) : target;
            snapped_time = result.snapped && time_guides_[result.index].grid && sample == target;
            snapped_sample = sample;
            if (companions.size()) desired.sample = sample;
            else {
                auto const offset = signed_sample_distance(initial.sample, sample) / time.span * time.width;
                locate(sample, result.snapped ? result.plateau : result.value < offset ? -1 : 1, dx);
            }
        }
        if (position.y != last.y) {
            auto const result = at_detents(-position.y - value_position_, value_anchor_,
                axis.units_per_pixel(height) * sensitivity(), value_anchor_plateau_, value_guides_);
            auto const value = static_cast<float>(result.value);
            if (__builtin_isfinite(value)) desired.value = value;
            value_plateau_ = result.plateau; snapped_vertical = result.snapped; snapped_value = result.value;
        }
        moved |= position.x != last.x || position.y != last.y;
        last = position;
        update_placement();
    }
    void set_tension(double value) { desired.tension = static_cast<float>(clamp(value, 0., 1.)); }
    Point companion(Point point) const
    {
        point.sample += desired.sample - initial.sample;
        point.value = static_cast<float>(point.value + (double(desired.value) - initial.value));
        return point;
    }
    bool order_changed() const
    {
        if (desired.order != sent.order || ordered.size() != sent_ordered.size()) return true;
        for (uint32_t i = 0; i < ordered.size(); ++i) if (ordered[i] != sent_ordered[i]) return true;
        return false;
    }
    bool dirty() const
    {
        return !has_sent || desired.sample != sent.sample || desired.value != sent.value
            || !same_style(style_of(desired), style_of(sent)) || order_changed() || !placement.same(sent_placement);
    }
    bool place_command() const
    {
        return adding || order_changed() || !same_style(style_of(desired), style_of(sent)) || !placement.same(sent_placement)
            || (ordered.size() && desired.sample != sent.sample);
    }
    EditRequest send(Context const& frame, EditGesture gesture)
    {
        EditRequest request;
        Label sample;
        sample.integer(static_cast<int64_t>(desired.sample));
        if (tension) {
            request = frame.send(SetSegment{desired.id, static_cast<SetSegmentInterpolation>(desired.interpolation), desired.tension}, gesture);
        } else if (place_command()) {
            PlacePointSegmentsElement changes[4];
            uint32_t count = 0;
            auto add = [&](uint32_t id, SegmentStyle style) {
                changes[count++] = {id, static_cast<PlacePointSegmentsElementInterpolation>(style.interpolation), style.tension};
            };
            for (uint32_t i = 0; i < sent_placement.size; ++i) {
                auto const change = sent_placement.segments[i];
                bool still_changed = false;
                for (uint32_t j = 0; j < placement.size; ++j) if (placement.segments[j].id == change.id) still_changed = true;
                if (!still_changed) if (auto const* point = find_point(neighbors, change.id)) add(point->id, style_of(*point));
            }
            for (uint32_t i = 0; i < placement.size; ++i) {
                auto const change = placement.segments[i];
                bool already_sent = false;
                for (uint32_t j = 0; j < sent_placement.size; ++j)
                    if (sent_placement.segments[j].id == change.id && same_style(sent_placement.segments[j].style, change.style))
                        already_sent = true;
                if (!already_sent) add(change.id, change.style);
            }
            request = frame.send(PlacePoint{{desired.id, 0, desired.value, desired.interpolation, desired.tension,
                converted_string(sample.view()), desired.order}, !has_sent, {changes, count}, {ordered.begin(), ordered.size()}}, gesture);
        } else {
            request = frame.send(MovePoint{desired.id, 0, desired.value, converted_string(sample.view())}, gesture);
        }
        if (!request) return {};
        has_sent = true; sent = desired; sent_placement = placement;
        sent_ordered.clear();
        for (auto id : ordered) sent_ordered.push(id);
        return request;
    }
};

} // namespace automation_presentation
