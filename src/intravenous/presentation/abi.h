#ifndef INTRAVENOUS_PRESENTATION_ABI_H
#define INTRAVENOUS_PRESENTATION_ABI_H

#include "presentation.h"
#include <stddef.h>

/* Host ABI for wasm32. Keep the layout in sync with createWasmPresentation. */
enum { IV_REUSE_DRAWING = 1, IV_SAVE_STATE = 2 };

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

typedef struct IvRecordedNumericEdits {
    IvNumericNodeEditSpan values;
    struct IvRecordedNumericEdits *next;
} IvRecordedNumericEdits;

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
    IvRecordedNumericEdits *edits, *last_edits;
    IvTimelineGrid *grid;
};

#if defined(__wasm32__)
_Static_assert(sizeof(IvGestureInput) == 56, "gesture layout");
_Static_assert(offsetof(IvGestureInput, point) == 16, "gesture position layout");
_Static_assert(sizeof(IvPolyline) == 16, "polyline layout");
_Static_assert(sizeof(IvDragTarget) == 40, "target layout");
_Static_assert(sizeof(IvRecordedPolylines) == 12, "drawing batch layout");
_Static_assert(sizeof(IvRecordedDragTargets) == 12, "target batch layout");
_Static_assert(sizeof(IvText) == 32, "text layout");
_Static_assert(sizeof(IvRecordedTexts) == 12, "text batch layout");
_Static_assert(sizeof(IvObservedTriggerColumns) == 40, "observation layout");
_Static_assert(sizeof(IvPresentationContext) == 168, "frame layout");
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
_Static_assert(sizeof(IvNumericValue) == 12, "numeric argument layout");
_Static_assert(sizeof(IvControlInput) == 24, "control event layout");
_Static_assert(sizeof(IvSetting) == 64, "setting layout");
_Static_assert(sizeof(IvNumericField) == 20, "field layout");
_Static_assert(sizeof(IvNumericNodeEdit) == 36, "node edit layout");
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
