#ifndef INTRAVENOUS_PRESENTATION_H
#define INTRAVENOUS_PRESENTATION_H

#include <stdbool.h>
#include <stdint.h>

#define IV_PRESENTATION_ABI_VERSION 5
#define IV_PRESENTATION_DESCRIPTOR_VERSION 1
#define IV_PRESENTATION_TIMELINE 1
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

enum {
    IV_GESTURE_BEGIN, IV_GESTURE_MOVE, IV_GESTURE_MODIFIERS,
    IV_GESTURE_WHEEL, IV_GESTURE_FINISH, IV_GESTURE_CANCEL
};
enum { IV_SHIFT = 1, IV_ALT = 2, IV_CONTROL = 4, IV_META = 8 };
typedef struct {
    uint32_t id, phase, modifiers, reserved;
    IvPoint point, delta;
    double wheel;
} IvGestureInput;
IV_TYPEDEF_SPAN(IvGestureInput, IvGestureInputSpan);

enum {
    IV_INK_DEFAULT, IV_INK_FOREGROUND, IV_INK_MUTED, IV_INK_INPUT,
    IV_INK_OUTPUT, IV_INK_SELECTION, IV_INK_HIGHLIGHT, IV_INK_WARNING,
    IV_INK_BACKGROUND
};
typedef struct {
    uint32_t id, ink;
    IvPointSpan points;
} IvPolyline;
IV_TYPEDEF_SPAN(IvPolyline, IvPolylineSpan);

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

enum { IV_LANE_EDITABLE, IV_LANE_COMPACT, IV_LANE_MINI, IV_LANE_NANO };

enum { IV_DRAG_RELATIVE, IV_DRAG_ABSOLUTE };
typedef struct {
    uint32_t id, drag;
    IvRectangle bounds;
} IvDragTarget;
IV_TYPEDEF_SPAN(IvDragTarget, IvDragTargetSpan);

typedef struct {
    int64_t begin_sample;
    double begin_fraction, span_samples;
    uint32_t sample_rate;
} IvTimelineView;

IV_TYPEDEF_SPAN(double, IvNumberSpan);
enum { IV_NUMERIC_UNAVAILABLE, IV_NUMERIC_SCALAR, IV_NUMERIC_SEQUENCE };
typedef struct {
    IvNumberSpan values;
    uint32_t kind;
} IvNumericValue;
IV_TYPEDEF_SPAN(IvNumericValue, IvNumericValueSpan);

enum { IV_CONTROL_PREVIEW, IV_CONTROL_COMMIT, IV_CONTROL_FINISH };
typedef struct {
    IvString id;
    double value;
    uint32_t phase, has_value;
} IvControlInput;
IV_TYPEDEF_SPAN(IvControlInput, IvControlInputSpan);

enum { IV_SETTING_TEXT, IV_SETTING_KNOB, IV_SETTING_ACTION };
enum { IV_SETTING_INTEGER = 1, IV_SETTING_DISABLED = 2 };
typedef struct {
    IvString id, label;
    uint32_t row, kind, flags;
    double value, min, max, default_value;
} IvSetting;
IV_TYPEDEF_SPAN(IvSetting, IvSettingSpan);
typedef struct { IvString label; IvSettingSpan settings; } IvSettingsPanel;

typedef struct { IvString name; IvNumericValue value; } IvNumericField;
IV_TYPEDEF_SPAN(IvNumericField, IvNumericFieldSpan);
enum { IV_EDIT_REPLACE_PENDING = 1, IV_EDIT_FINISH = 2 };
enum { IV_MAX_NUMERIC_EDITS = 32 };
typedef struct {
    IvString nominal_id, fingerprint, gesture;
    IvNumericFieldSpan fields;
    uint32_t flags;
} IvNumericNodeEdit;
IV_TYPEDEF_SPAN(IvNumericNodeEdit, IvNumericNodeEditSpan);

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
IvTimelineView iv_timeline_view(const IvPresentationContext *context);
IvNumericValueSpan iv_numeric_arguments(const IvPresentationContext *context);
IvControlInputSpan iv_control_inputs(const IvPresentationContext *context);
bool iv_edits_pending(const IvPresentationContext *context);
bool iv_edit_failed(const IvPresentationContext *context);

/* Calls record in memory. The host consumes the recorded batches after the
   presentation returns. Each span can contain all items of that kind. */
bool iv_draw_polylines(IvPresentationContext *context, IvPolylineSpan polylines);
bool iv_draw_texts(IvPresentationContext *context, IvTextSpan texts);
bool iv_register_drag_targets(IvPresentationContext *context, IvDragTargetSpan targets);
bool iv_set_settings_panel(IvPresentationContext *context, IvSettingsPanel panel);
bool iv_submit_numeric_edits(IvPresentationContext *context, IvNumericNodeEditSpan edits);
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
