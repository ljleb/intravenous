#ifndef INTRAVENOUS_PRESENTATION_ABI_H
#define INTRAVENOUS_PRESENTATION_ABI_H

#include "presentation.h"
#include <stddef.h>

/* Host ABI for wasm32. Keep the layout in sync with createWasmPresentation. */
enum { IV_REUSE_DRAWING = 1, IV_SAVE_STATE = 2 };

typedef struct { const void *value; uint32_t state; } IvConstructionArgument;
IV_TYPEDEF_SPAN(IvConstructionArgument, IvConstructionArgumentSpan);

typedef struct IvRecordedPolylines {
    IvPolylineSpan values;
    struct IvRecordedPolylines *next;
} IvRecordedPolylines;

typedef struct IvRecordedDragTargets {
    IvDragTargetSpan values;
    struct IvRecordedDragTargets *next;
} IvRecordedDragTargets;

typedef struct IvRecordedTexts {
    IvTextSpan values;
    struct IvRecordedTexts *next;
} IvRecordedTexts;

typedef struct IvRecordedShapes {
    IvShapeSpan values;
    struct IvRecordedShapes *next;
} IvRecordedShapes;

typedef struct IvRecordedEdits {
    IvNodeEdit value;
    struct IvRecordedEdits *next;
} IvRecordedEdits;

struct IvPresentationContext {
    double width, height;
    IvGestureInputSpan gestures;
    uint32_t editable, flags;
    char *storage;
    uint32_t capacity, used, error;
    IvRecordedPolylines *drawing, *last_drawing;
    IvRecordedDragTargets *targets, *last_targets;
    IvString status;
    IvRecordedTexts *texts, *last_texts;
    IvObservedTriggerColumnSpan trigger_columns;
    IvStringSpan connected_inputs;
    uint32_t mode;
    uint32_t edit_state;
    IvTimelineView timeline;
    IvNumericValueSpan arguments;
    IvControlInputSpan controls;
    IvSettingsPanel *panel;
    IvRecordedEdits *edits, *last_edits;
    IvTimelineGrid *grid;
    uint64_t revision;
    IvEditReplySpan edit_replies;
    uint32_t edit_count;
    IvObservedWaveformSpan waveforms;
    IvConstructionArgumentSpan construction_arguments;
    IvEditorInputSpan editor_inputs;
    IvManualInputSpan manual_inputs;
    double frame_time;
    IvSettingsPanel *context_menu;
    IvEditorFeedback *editor_feedback;
    IvRecordedShapes *shapes, *last_shapes;
    uint32_t construction_snapshot;
};

#if defined(__wasm32__)
_Static_assert(sizeof(IvGestureInput) == 64 && offsetof(IvGestureInput, snapping) == 56, "gesture layout");
_Static_assert(sizeof(IvSnapCandidates) == 24, "snap layout");
_Static_assert(offsetof(IvGestureInput, point) == 16, "gesture position layout");
_Static_assert(sizeof(IvPolyline) == 72 && offsetof(IvPolyline, stroke) == 16, "polyline layout");
_Static_assert(sizeof(IvDragTarget) == 64 && offsetof(IvDragTarget, hint) == 44, "target layout");
_Static_assert(sizeof(IvShape) == 64 && offsetof(IvShape, geometry) == 24, "shape layout");
_Static_assert(sizeof(IvRecordedPolylines) == 12, "drawing batch layout");
_Static_assert(sizeof(IvRecordedDragTargets) == 12, "target batch layout");
_Static_assert(sizeof(IvText) == 32, "text layout");
_Static_assert(sizeof(IvRecordedTexts) == 12, "text batch layout");
_Static_assert(sizeof(IvObservedTriggerColumns) == 40, "observation layout");
_Static_assert(sizeof(IvPresentationContext) == 256, "frame layout");
_Static_assert(offsetof(IvPresentationContext, gestures) == 16, "input layout");
_Static_assert(offsetof(IvPresentationContext, storage) == 32, "arena layout");
_Static_assert(offsetof(IvPresentationContext, drawing) == 48, "drawing layout");
_Static_assert(offsetof(IvPresentationContext, targets) == 56, "target layout");
_Static_assert(offsetof(IvPresentationContext, status) == 64, "status layout");
_Static_assert(offsetof(IvPresentationContext, texts) == 72, "text output layout");
_Static_assert(offsetof(IvPresentationContext, trigger_columns) == 80, "observation input layout");
_Static_assert(offsetof(IvPresentationContext, connected_inputs) == 88, "port input layout");
_Static_assert(offsetof(IvPresentationContext, mode) == 96, "lane mode layout");
_Static_assert(offsetof(IvPresentationContext, timeline) == 104, "timeline input layout");
_Static_assert(offsetof(IvPresentationContext, arguments) == 136, "argument layout");
_Static_assert(offsetof(IvPresentationContext, controls) == 144, "control input layout");
_Static_assert(offsetof(IvPresentationContext, panel) == 152, "panel layout");
_Static_assert(offsetof(IvPresentationContext, edits) == 156, "edit layout");
_Static_assert(offsetof(IvPresentationContext, grid) == 164, "grid layout");
_Static_assert(offsetof(IvPresentationContext, revision) == 168, "revision layout");
_Static_assert(offsetof(IvPresentationContext, edit_replies) == 176, "edit replies layout");
_Static_assert(offsetof(IvPresentationContext, edit_count) == 184, "edit count layout");
_Static_assert(offsetof(IvPresentationContext, waveforms) == 188, "waveform layout");
_Static_assert(offsetof(IvPresentationContext, construction_arguments) == 196, "construction argument layout");
_Static_assert(sizeof(IvConstructionArgument) == 8, "construction argument binding layout");
_Static_assert(offsetof(IvPresentationContext, editor_inputs) == 204 && sizeof(IvEditorInput) == 40, "editor input layout");
_Static_assert(offsetof(IvPresentationContext, manual_inputs) == 212 && sizeof(IvManualInput) == 16, "manual input layout");
_Static_assert(offsetof(IvPresentationContext, frame_time) == 224, "frame clock layout");
_Static_assert(offsetof(IvPresentationContext, context_menu) == 232, "context menu layout");
_Static_assert(offsetof(IvPresentationContext, editor_feedback) == 236 && sizeof(IvEditorFeedback) == 64, "editor feedback layout");
_Static_assert(offsetof(IvPresentationContext, shapes) == 240, "shape output layout");
_Static_assert(offsetof(IvPresentationContext, construction_snapshot) == 248, "construction snapshot layout");
_Static_assert(sizeof(IvWaveformPoint) == 24, "waveform point layout");
_Static_assert(sizeof(IvWaveformRegion) == 32 && offsetof(IvWaveformRegion, points) == 20, "waveform region layout");
_Static_assert(sizeof(IvObservedWaveform) == 32, "observed waveform layout");
_Static_assert(sizeof(IvNumericValue) == 12, "numeric argument layout");
_Static_assert(sizeof(IvControlInput) == 40, "control event layout");
_Static_assert(sizeof(IvSetting) == 88, "setting layout");
_Static_assert(sizeof(IvConvertedScalar) == 24, "converted scalar layout");
_Static_assert(sizeof(IvNodeEdit) == 20, "node edit layout");
_Static_assert(sizeof(IvRecordedEdits) == 24, "recorded edit layout");
_Static_assert(sizeof(IvEditReply) == 24, "edit reply layout");
_Static_assert(sizeof(IvTimelineGrid) == 56, "timeline grid layout");
#endif

#ifdef __cplusplus
extern "C" {
#endif

void *iv_alloc(uint32_t size);
void iv_free(void *buffer);
void *iv_create_session(void);
void iv_destroy_session(void *session);
void iv_present(void *session, IvPresentationContext *context);
int32_t iv_save_state(const void *session, char *bytes, uint32_t capacity);
int32_t iv_restore_state(void *session, const char *bytes, uint32_t size);

#ifdef __cplusplus
}
#endif

#endif
