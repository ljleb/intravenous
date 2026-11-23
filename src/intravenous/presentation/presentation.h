#ifndef INTRAVENOUS_PRESENTATION_H
#define INTRAVENOUS_PRESENTATION_H

#include <stdbool.h>
#include <stdint.h>

#define IV_PRESENTATION_ABI_VERSION 10
#define IV_PRESENTATION_DESCRIPTOR_VERSION 1
#define IV_PRESENTATION_TIMELINE 1
#define IV_PRESENTATION_TIMELINE_SNAPPING 2
#define IV_PRESENTATION_NO_FLAGS 0
#define IV_PRESENTATION_STRINGIFY_IMPL(Value) #Value
#define IV_PRESENTATION_STRINGIFY(Value) IV_PRESENTATION_STRINGIFY_IMPL(Value)

/* Descriptor bytes are version, ABI version, flags, then the optional
   UTF-8 path in the icon's 16 by 16 coordinate space. */
#if defined(__wasm32__)
#define IV_PRESENTATION_DETAIL_DESCRIPTOR(Flags, IconAssembly) \
    __asm__(".section .custom_section.iv.presentation,\"\",@\n" \
        ".byte " IV_PRESENTATION_STRINGIFY(IV_PRESENTATION_DESCRIPTOR_VERSION) ", " \
        IV_PRESENTATION_STRINGIFY(IV_PRESENTATION_ABI_VERSION) ", " IV_PRESENTATION_STRINGIFY(Flags) "\n" \
        IconAssembly "\n.text\n")
#else
#define IV_PRESENTATION_DETAIL_DESCRIPTOR(Flags, IconAssembly)
#endif

#define IV_PRESENTATION_DESCRIPTOR(Flags, Icon) \
    IV_PRESENTATION_DETAIL_DESCRIPTOR(Flags, ".ascii " IV_PRESENTATION_STRINGIFY(Icon))

#ifdef __cplusplus
extern "C" {
#define IV_PRESENTATION_EXPORT extern "C"
#else
#define IV_PRESENTATION_EXPORT
#endif

#define IV_TYPEDEF_SPAN(Type, Name) typedef struct { const Type *data; uint32_t size; } Name
typedef struct IvPresentationContext IvPresentationContext;
typedef struct { double x, y; } IvPoint;
typedef struct { double x, y, width, height; } IvRectangle;
IV_TYPEDEF_SPAN(char, IvString);
IV_TYPEDEF_SPAN(IvString, IvStringSpan);
IV_TYPEDEF_SPAN(IvPoint, IvPointSpan);
IV_TYPEDEF_SPAN(int64_t, IvSampleIndexSpan);

typedef struct {
    IvSampleIndexSpan marks;
    int64_t playhead;
    uint32_t has_playhead;
} IvSnapCandidates;

enum {
    IV_GESTURE_BEGIN, IV_GESTURE_MOVE, IV_GESTURE_MODIFIERS,
    IV_GESTURE_WHEEL, IV_GESTURE_FINISH, IV_GESTURE_CANCEL
};
enum { IV_SHIFT = 1, IV_ALT = 2, IV_CONTROL = 4, IV_META = 8 };
typedef struct {
    uint32_t id, phase, modifiers, sequence;
    IvPoint point, delta;
    double wheel;
    const IvSnapCandidates *snapping;
} IvGestureInput;
IV_TYPEDEF_SPAN(IvGestureInput, IvGestureInputSpan);

enum {
    IV_INK_DEFAULT, IV_INK_FOREGROUND, IV_INK_MUTED, IV_INK_INPUT,
    IV_INK_OUTPUT, IV_INK_SELECTION, IV_INK_HIGHLIGHT, IV_INK_WARNING,
    IV_INK_BACKGROUND
};
enum { IV_STROKE_OPACITY = 1, IV_STROKE_DASHED = 2, IV_STROKE_FILL = 4, IV_STROKE_FADE = 8 };
typedef struct {
    uint32_t flags, target;
    double opacity, width;
    IvPoint fade_from, fade_to;
} IvStroke;
typedef struct {
    uint32_t id, ink;
    IvPointSpan points;
    IvStroke stroke;
} IvPolyline;
IV_TYPEDEF_SPAN(IvPolyline, IvPolylineSpan);

enum { IV_SHAPE_RECTANGLE, IV_SHAPE_CIRCLE };
typedef struct { IvPoint center; double radius; } IvCircle;
typedef struct {
    uint32_t id, kind, ink, border, target;
    union { IvRectangle rectangle; IvCircle circle; } geometry;
    double opacity;
} IvShape;
IV_TYPEDEF_SPAN(IvShape, IvShapeSpan);

typedef struct {
    uint32_t id, ink;
    IvPoint position;
    IvString text;
} IvText;
IV_TYPEDEF_SPAN(IvText, IvTextSpan);

enum { IV_OBSERVATION_PENDING, IV_OBSERVATION_READY, IV_OBSERVATION_UNAVAILABLE, IV_OBSERVATION_FAILED };
IV_TYPEDEF_SPAN(uint32_t, IvColumnSpan);
typedef struct {
    IvString id;
    IvColumnSpan columns;
    uint64_t count;
    IvString error;
    uint32_t status;
    uint32_t partial;
} IvObservedTriggerColumns;
IV_TYPEDEF_SPAN(IvObservedTriggerColumns, IvObservedTriggerColumnSpan);

typedef struct { uint64_t index; double offset, value; } IvWaveformPoint;
IV_TYPEDEF_SPAN(IvWaveformPoint, IvWaveformPointSpan);
typedef struct {
    double begin, end;
    int32_t pace_exponent;
    IvWaveformPointSpan points;
} IvWaveformRegion;
IV_TYPEDEF_SPAN(IvWaveformRegion, IvWaveformRegionSpan);
typedef struct {
    IvString id;
    IvWaveformRegionSpan regions;
    IvString error;
    uint32_t status, partial;
} IvObservedWaveform;
IV_TYPEDEF_SPAN(IvObservedWaveform, IvObservedWaveformSpan);

enum { IV_LANE_EDITABLE, IV_LANE_COMPACT, IV_LANE_MINI, IV_LANE_NANO };

enum { IV_DRAG_RELATIVE, IV_DRAG_ABSOLUTE };
enum {
    IV_TARGET_SECONDARY = 1, IV_TARGET_WHEEL = 2, IV_TARGET_CONTEXT = 4,
    IV_TARGET_CURSOR_RETURN = 8, IV_TARGET_HIDE_CURSOR = 16, IV_TARGET_HORIZONTAL_RESIZE = 32
};
typedef struct {
    uint32_t id, drag;
    IvRectangle bounds;
    uint32_t flags;
    IvString hint, shortcuts;
} IvDragTarget;
IV_TYPEDEF_SPAN(IvDragTarget, IvDragTargetSpan);

typedef struct {
    int64_t begin_sample;
    double begin_fraction, span_samples;
    uint32_t sample_rate, flags;
} IvTimelineView;
enum { IV_TIMELINE_CUSTOM_GRID = 1 };

IV_TYPEDEF_SPAN(double, IvNumberSpan);
enum { IV_NUMERIC_UNAVAILABLE, IV_NUMERIC_SCALAR, IV_NUMERIC_SEQUENCE };
typedef struct {
    IvNumberSpan values;
    uint32_t kind;
} IvNumericValue;
IV_TYPEDEF_SPAN(IvNumericValue, IvNumericValueSpan);

enum { IV_CONTROL_PREVIEW, IV_CONTROL_COMMIT, IV_CONTROL_FINISH };
enum { IV_CONTROL_NO_VALUE, IV_CONTROL_NUMBER, IV_CONTROL_STRING };
typedef struct {
    IvString id;
    double value;
    uint32_t phase, value_kind;
    IvString string;
    uint32_t sequence;
} IvControlInput;
IV_TYPEDEF_SPAN(IvControlInput, IvControlInputSpan);

enum { IV_SETTING_TEXT, IV_SETTING_KNOB, IV_SETTING_ACTION, IV_SETTING_CHOICE };
enum { IV_SETTING_INTEGER = 1, IV_SETTING_DISABLED = 2 };
typedef struct { IvString value, label; } IvChoice;
IV_TYPEDEF_SPAN(IvChoice, IvChoiceSpan);
typedef struct {
    IvString id, label;
    uint32_t row, kind, flags, choice_index;
    double value, min, max, default_value;
    IvChoiceSpan choices;
    double step, scrub_speed;
} IvSetting;
IV_TYPEDEF_SPAN(IvSetting, IvSettingSpan);
typedef struct { IvString label; IvSettingSpan settings; } IvSettingsPanel;

enum { IV_EDITOR_CONTEXT, IV_EDITOR_DISMISS_CONTEXT, IV_EDITOR_ITEMS, IV_EDITOR_VALUE_VIEW, IV_EDITOR_INTERRUPT };
enum { IV_ITEMS_DELETE, IV_ITEMS_SELECT_ALL };
enum { IV_VALUE_PAN, IV_VALUE_ZOOM, IV_VALUE_RESET };
enum { IV_INTERRUPT_HISTORY, IV_INTERRUPT_DISCONNECTED };
typedef struct {
    uint32_t kind, action, target, sequence;
    IvPoint point;
    double value;
} IvEditorInput;
IV_TYPEDEF_SPAN(IvEditorInput, IvEditorInputSpan);
typedef struct { IvString id; double value; } IvManualInput;
IV_TYPEDEF_SPAN(IvManualInput, IvManualInputSpan);

enum {
    IV_EDITOR_ITEM_EDITING = 1, IV_EDITOR_VALUE_NAVIGATION = 2, IV_EDITOR_CANCEL_DRAG = 4,
    IV_EDITOR_WAKE = 8, IV_EDITOR_SECONDS_RULER = 16
};
enum { IV_CURSOR_UNCHANGED, IV_CURSOR_POINT, IV_CURSOR_SAMPLE };
typedef struct {
    uint32_t flags, cursor;
    int64_t sample;
    /* Logical pixels for IV_CURSOR_POINT. For IV_CURSOR_SAMPLE, y is a
       fraction of the content height and x is unused. */
    IvPoint position;
    IvString hint, shortcuts;
    double wake_after_ms;
    double ruler_label_inset;
} IvEditorFeedback;

enum { IV_SCALAR_BOOLEAN, IV_SCALAR_NUMBER, IV_SCALAR_STRING };
typedef struct { uint32_t kind; double number; IvString string; } IvConvertedScalar;

enum { IV_EDIT_REPLACE_PENDING = 1, IV_EDIT_FINISH = 2 };
enum { IV_MAX_EDITS = 32 };
typedef struct {
    uint32_t binding, gesture, request, flags;
    const void *payload;
} IvNodeEdit;

enum { IV_EDIT_ACCEPTED, IV_EDIT_REJECTED };
typedef struct {
    uint32_t request, status;
    uint64_t revision;
    IvString error;
} IvEditReply;
IV_TYPEDEF_SPAN(IvEditReply, IvEditReplySpan);

IV_TYPEDEF_SPAN(uint64_t, IvStrideSpan);
enum { IV_GRID_SEEK = 1, IV_GRID_LABEL_MAJOR = 2 };
typedef struct {
    uint64_t period_numerator, period_denominator;
    IvStrideSpan strides;
    IvNumberSpan line_starts;
    double minimum_spacing, opacity;
    uint32_t ink, flags;
} IvTimelineGrid;

IvRectangle iv_content_bounds(const IvPresentationContext *context);
bool iv_can_edit(const IvPresentationContext *context);
IvGestureInputSpan iv_gesture_inputs(const IvPresentationContext *context);
uint32_t iv_lane_mode(const IvPresentationContext *context);
IvStringSpan iv_connected_inputs(const IvPresentationContext *context);
IvObservedTriggerColumnSpan iv_observed_trigger_columns(const IvPresentationContext *context);
IvObservedWaveformSpan iv_observed_waveforms(const IvPresentationContext *context);
IvTimelineView iv_timeline_view(const IvPresentationContext *context);
IvNumericValueSpan iv_numeric_arguments(const IvPresentationContext *context);
/* An omitted argument belongs to an accepted invocation and uses the node's
   constructor default. An unavailable argument cannot be read by the guest. */
enum { IV_ARGUMENT_UNAVAILABLE, IV_ARGUMENT_OMITTED, IV_ARGUMENT_AVAILABLE };
uint32_t iv_construction_argument_state(const IvPresentationContext *context, uint32_t binding);
/* The typed value is borrowed for the current call. NULL means omitted or unavailable. */
const void *iv_construction_argument(const IvPresentationContext *context, uint32_t binding);
uint32_t iv_construction_snapshot(const IvPresentationContext *context);
IvControlInputSpan iv_control_inputs(const IvPresentationContext *context);
bool iv_edits_pending(const IvPresentationContext *context);
bool iv_edit_failed(const IvPresentationContext *context);
uint64_t iv_project_revision(const IvPresentationContext *context);
IvEditReplySpan iv_edit_replies(const IvPresentationContext *context);
IvEditorInputSpan iv_editor_inputs(const IvPresentationContext *context);
IvManualInputSpan iv_manual_inputs(const IvPresentationContext *context);
double iv_frame_time(const IvPresentationContext *context);

/* Calls record in memory. The host consumes the recorded batches after the
   presentation returns. Each span can contain all items of that kind. */
bool iv_draw_polylines(IvPresentationContext *context, IvPolylineSpan polylines);
bool iv_draw_texts(IvPresentationContext *context, IvTextSpan texts);
bool iv_draw_shapes(IvPresentationContext *context, IvShapeSpan shapes);
bool iv_register_drag_targets(IvPresentationContext *context, IvDragTargetSpan targets);
bool iv_set_settings_panel(IvPresentationContext *context, IvSettingsPanel panel);
bool iv_set_context_menu(IvPresentationContext *context, IvSettingsPanel panel);
bool iv_set_editor_feedback(IvPresentationContext *context, IvEditorFeedback feedback);
/* Copy every span referenced by a generated payload into the output arena.
   The record and its spans are inspected after iv_present returns. */
void *iv_copy_edit_payload(IvPresentationContext *context, const void *data, uint32_t size);
bool iv_submit_edit(IvPresentationContext *context, IvNodeEdit edit);
bool iv_set_timeline_grid(IvPresentationContext *context, const IvTimelineGrid *grid);
void iv_reuse_drawing(IvPresentationContext *context);
void iv_set_status(IvPresentationContext *context, IvString status);

typedef struct {
    IvPolyline *polylines;
    IvPoint *points;
} IvPolylineBatch;
/* Fill the descriptors and give each a subspan of points before returning. */
IvPolylineBatch iv_reserve_polylines(IvPresentationContext *context, uint32_t polylines, uint32_t points);

void *iv_session_allocate(uint32_t size);
void iv_session_free(void *memory);
void iv_request_save(IvPresentationContext *context);

#ifdef __cplusplus
}
#endif

/* C sessions start with zero-initialized state. An empty string requests
   the author's initial values. Save and restore run outside the live frame. */
#define IV_DEFINE_PRESENTATION(State, Present, Save, Restore) \
    IV_PRESENTATION_EXPORT void *iv_create_session(void) { \
        State *state = (State *)iv_session_allocate(sizeof(State)); \
        if (state) *state = (State){0}; \
        return state; \
    } \
    IV_PRESENTATION_EXPORT void iv_destroy_session(void *state) { iv_session_free(state); } \
    IV_PRESENTATION_EXPORT void iv_present(void *state, IvPresentationContext *context) { Present((State *)state, context); } \
    IV_PRESENTATION_EXPORT int32_t iv_save_state(const void *state, char *bytes, uint32_t capacity) { \
        return Save((const State *)state, bytes, capacity); \
    } \
    IV_PRESENTATION_EXPORT int32_t iv_restore_state(void *state, const char *bytes, uint32_t size) { \
        IvString serialized = {bytes, size}; \
        return Restore((State *)state, serialized); \
    }

#endif
