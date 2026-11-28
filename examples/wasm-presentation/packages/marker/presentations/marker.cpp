#include <intravenous/presentation/presentation.hpp>
#include <intravenous/presentation/state_json.h>

class Marker {
public:
    void present(iv::presentation::Context& frame);
    int32_t save(char* bytes, uint32_t capacity) const;
    int32_t restore(IvString json);

private:
    enum { marker_handle = 1, marker_line = 2 };
    double position = .5;
    double draft = 0;
    double drawn_x = 0;
    double drawn_height = 0;
    bool dragging = false;
    bool drawn = false;
};

void Marker::present(iv::presentation::Context& frame)
{
    auto const bounds = frame.content_bounds();
    bool const editable = frame.can_edit();
    for (auto const& input : frame.gestures()) {
        if (input.phase == IV_GESTURE_CANCEL) dragging = false;
        if (input.id != marker_handle || !editable) continue;
        if (input.phase == IV_GESTURE_BEGIN) {
            draft = position;
            dragging = true;
        }
        if (input.phase == IV_GESTURE_MOVE || input.phase == IV_GESTURE_FINISH) {
            double fraction = input.point.x / bounds.width;
            draft = fraction < 0 ? 0 : fraction > 1 ? 1 : fraction;
        }
        if (input.phase == IV_GESTURE_FINISH) {
            if (position != draft) {
                position = draft;
                frame.request_save();
            }
            dragging = false;
        }
    }

    double x = (dragging ? draft : position) * bounds.width;
    if (drawn && x == drawn_x && bounds.height == drawn_height) {
        frame.reuse_drawing();
    } else {
        IvPolylineBatch batch = frame.reserve_polylines(1, 2);
        if (!batch.polylines) return;
        batch.points[0] = IvPoint{x, 0};
        batch.points[1] = IvPoint{x, bounds.height};
        batch.polylines[0] = IvPolyline{marker_line, IV_INK_OUTPUT, {batch.points, 2}};
        drawn = true;
        drawn_x = x;
        drawn_height = bounds.height;
    }

    if (editable) {
        const char hint[] = "Drag the marker";
        frame.set_status({hint, sizeof(hint) - 1});
        IvDragTarget target = {marker_handle, IV_DRAG_ABSOLUTE, {x - 8, 0, 16, bounds.height}};
        frame.register_drag_targets({&target, 1});
    }
}

int32_t Marker::save(char* bytes, uint32_t capacity) const
{
    IvJsonWriter writer = {.buffer = bytes, .capacity = capacity};
    iv_json_object(&writer);
    iv_json_decimal(&writer, "position", position);
    iv_json_end(&writer);
    return iv_json_finish(&writer);
}

int32_t Marker::restore(IvString json)
{
    double const saved_position = iv_json_read_decimal_member(json, "position", .5);
    position = saved_position >= 0 && saved_position <= 1 ? saved_position : .5;
    dragging = false;
    drawn = false;
    return 0;
}

IV_PRESENTATION_CLASS(Marker, IV_PRESENTATION_NO_FLAGS, "M5 2v12m4-12v12")
