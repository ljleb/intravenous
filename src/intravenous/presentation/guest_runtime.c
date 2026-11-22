#include "abi.h"

#include <stdbool.h>
#include <stddef.h>

extern unsigned char __heap_base;

typedef struct Block {
    struct Block *next;
    uint32_t size;
    bool available;
} Block;

enum { alignment = 8, page_size = 65536 };
enum { header_size = (sizeof(Block) + alignment - 1) & ~(alignment - 1) };
static Block *first;

static char *data(Block *block)
{
    return (char *)block + header_size;
}

static bool reserve(uintptr_t start, uint32_t size)
{
    if (size > UINTPTR_MAX - start) return false;
    uintptr_t end = start + size;
    size_t required = end / page_size + (end % page_size != 0);
    size_t current = __builtin_wasm_memory_size(0);
    return required <= current || __builtin_wasm_memory_grow(0, required - current) != (size_t)-1;
}

void *iv_alloc(uint32_t size)
{
    if (!size || size > UINT32_MAX - alignment + 1) return NULL;
    size = (size + alignment - 1) & ~(uint32_t)(alignment - 1);
    Block *tail = NULL;
    for (Block *block = first; block; block = block->next) {
        if (block->available && block->size >= size) {
            uint32_t remaining = block->size - size;
            if (remaining >= header_size + alignment) {
                Block *rest = (Block *)(data(block) + size);
                *rest = (Block){.next = block->next, .size = remaining - header_size, .available = true};
                block->next = rest;
                block->size = size;
            }
            block->available = false;
            return data(block);
        }
        tail = block;
    }
    if (tail && tail->available) {
        if (!reserve((uintptr_t)data(tail), size)) return NULL;
        tail->size = size;
        tail->available = false;
        return data(tail);
    }
    uintptr_t start = tail ? (uintptr_t)data(tail) + tail->size
        : ((uintptr_t)&__heap_base + alignment - 1) & ~(uintptr_t)(alignment - 1);
    if (start > UINTPTR_MAX - header_size || !reserve(start + header_size, size)) return NULL;
    Block *block = (Block *)start;
    *block = (Block){.size = size};
    if (tail) tail->next = block;
    else first = block;
    return data(block);
}

void iv_free(void *buffer)
{
    if (!buffer) return;
    Block *previous = NULL;
    for (Block *block = first; block; previous = block, block = block->next) {
        if (data(block) != buffer || block->available) continue;
        block->available = true;
        if (block->next && block->next->available) {
            block->size += header_size + block->next->size;
            block->next = block->next->next;
        }
        if (previous && previous->available) {
            previous->size += header_size + block->size;
            previous->next = block->next;
        }
        return;
    }
}

IvRectangle iv_content_bounds(const IvPresentationContext *context)
{
    return (IvRectangle){0, 0, context->width, context->height};
}

bool iv_can_edit(const IvPresentationContext *context)
{
    return context->editable != 0;
}

IvGestureInputSpan iv_gesture_inputs(const IvPresentationContext *context)
{
    return context->gestures;
}

uint32_t iv_lane_mode(const IvPresentationContext *context) { return context->mode; }
IvStringSpan iv_connected_inputs(const IvPresentationContext *context) { return context->connected_inputs; }
IvObservedTriggerColumnSpan iv_observed_trigger_columns(const IvPresentationContext *context)
{
    return context->trigger_columns;
}
IvTimelineView iv_timeline_view(const IvPresentationContext *context) { return context->timeline; }
IvNumericValueSpan iv_numeric_arguments(const IvPresentationContext *context) { return context->arguments; }
IvControlInputSpan iv_control_inputs(const IvPresentationContext *context) { return context->controls; }
bool iv_edits_pending(const IvPresentationContext *context) { return (context->edit_state & 1u) != 0; }
bool iv_edit_failed(const IvPresentationContext *context) { return (context->edit_state & 2u) != 0; }

static void *reserve_output(IvPresentationContext *context, uint32_t size)
{
    uint32_t start = (context->used + 7u) & ~7u;
    if (context->error || start > context->capacity || size > context->capacity - start) {
        context->error = 1;
        return NULL;
    }
    context->used = start + size;
    return context->storage + start;
}

static bool copy_string(IvPresentationContext *context, IvString *text, uint32_t limit)
{
    if (text->size > limit) {
        context->error = 1;
        return false;
    }
    char *copy = reserve_output(context, text->size);
    if (!copy) return false;
    for (uint32_t i = 0; i < text->size; ++i) copy[i] = text->data[i];
    text->data = copy;
    return true;
}

static bool copy_numbers(IvPresentationContext *context, IvNumberSpan *values, uint32_t limit)
{
    if (values->size > limit) {
        context->error = 1;
        return false;
    }
    double *copy = reserve_output(context, values->size * sizeof(double));
    if (!copy) return false;
    for (uint32_t i = 0; i < values->size; ++i) copy[i] = values->data[i];
    values->data = copy;
    return true;
}

bool iv_set_settings_panel(IvPresentationContext *context, IvSettingsPanel panel)
{
    if (context->panel || panel.settings.size > 256) {
        context->error = 1;
        return false;
    }
    IvSettingsPanel *record = reserve_output(context, sizeof(*record));
    IvSetting *settings = reserve_output(context, panel.settings.size * sizeof(*settings));
    if (context->error || !copy_string(context, &panel.label, 160)) return false;
    for (uint32_t i = 0; i < panel.settings.size; ++i) {
        settings[i] = panel.settings.data[i];
        if (!copy_string(context, &settings[i].id, 160)
            || !copy_string(context, &settings[i].label, 1024)) return false;
    }
    *record = (IvSettingsPanel){panel.label, {settings, panel.settings.size}};
    context->panel = record;
    return true;
}

bool iv_submit_numeric_edits(IvPresentationContext *context, IvNumericNodeEditSpan edits)
{
    if (!edits.size) return true;
    if (edits.size > IV_MAX_NUMERIC_EDITS) {
        context->error = 1;
        return false;
    }
    IvRecordedNumericEdits *batch = reserve_output(context, sizeof(*batch));
    IvNumericNodeEdit *values = reserve_output(context, edits.size * sizeof(*values));
    if (context->error) return false;
    for (uint32_t i = 0; i < edits.size; ++i) {
        IvNumericNodeEdit edit = edits.data[i];
        if (edit.fields.size > 64) {
            context->error = 1;
            return false;
        }
        IvNumericField *fields = reserve_output(context, edit.fields.size * sizeof(*fields));
        if (context->error || !copy_string(context, &edit.nominal_id, 1024)
            || !copy_string(context, &edit.fingerprint, 1024) || !copy_string(context, &edit.gesture, 160)) return false;
        for (uint32_t field = 0; field < edit.fields.size; ++field) {
            fields[field] = edit.fields.data[field];
            if (!copy_string(context, &fields[field].name, 160)
                || !copy_numbers(context, &fields[field].value.values, 1024)) return false;
        }
        edit.fields.data = fields;
        values[i] = edit;
    }
    *batch = (IvRecordedNumericEdits){.values = {values, edits.size}};
    if (context->last_edits) context->last_edits->next = batch;
    else context->edits = batch;
    context->last_edits = batch;
    return true;
}

bool iv_set_timeline_grid(IvPresentationContext *context, const IvTimelineGrid *grid)
{
    if (context->grid || !grid->strides.size || grid->strides.size > 32
        || grid->line_starts.size != grid->strides.size || (context->flags & IV_REUSE_DRAWING)) {
        context->error = 1;
        return false;
    }
    IvTimelineGrid *record = reserve_output(context, sizeof(*record));
    uint64_t *strides = reserve_output(context, grid->strides.size * sizeof(*strides));
    if (context->error) return false;
    *record = *grid;
    if (!copy_numbers(context, &record->line_starts, 32)) return false;
    for (uint32_t i = 0; i < grid->strides.size; ++i) strides[i] = grid->strides.data[i];
    record->strides.data = strides;
    context->grid = record;
    return true;
}

IvPolylineBatch iv_reserve_polylines(IvPresentationContext *context, uint32_t polylines, uint32_t points)
{
    if (!polylines || polylines > 4096 || points > 16384 || (context->flags & IV_REUSE_DRAWING)) {
        context->error = 1;
        return (IvPolylineBatch){0};
    }
    IvRecordedPolylines *batch = reserve_output(context, sizeof(*batch));
    IvPolyline *descriptors = reserve_output(context, polylines * sizeof(*descriptors));
    IvPoint *vertices = reserve_output(context, points * sizeof(*vertices));
    if (context->error) return (IvPolylineBatch){0};
    *batch = (IvRecordedPolylines){.values = {descriptors, polylines}};
    if (context->last_drawing) context->last_drawing->next = batch;
    else context->drawing = batch;
    context->last_drawing = batch;
    return (IvPolylineBatch){descriptors, vertices};
}

bool iv_draw_polylines(IvPresentationContext *context, IvPolylineSpan polylines)
{
    if (!polylines.size) return true;
    if (polylines.size > 4096) {
        context->error = 1;
        return false;
    }
    uint32_t points = 0;
    for (uint32_t i = 0; i < polylines.size; ++i) {
        if (polylines.data[i].points.size > 16384 - points) {
            context->error = 1;
            return false;
        }
        points += polylines.data[i].points.size;
    }
    IvPolylineBatch batch = iv_reserve_polylines(context, polylines.size, points);
    if (!batch.polylines) return false;
    IvPoint *next = batch.points;
    for (uint32_t i = 0; i < polylines.size; ++i) {
        IvPolyline line = polylines.data[i];
        for (uint32_t j = 0; j < line.points.size; ++j) next[j] = line.points.data[j];
        batch.polylines[i] = (IvPolyline){line.id, line.ink, {next, line.points.size}};
        next += line.points.size;
    }
    return true;
}

bool iv_register_drag_targets(IvPresentationContext *context, IvDragTargetSpan targets)
{
    if (!targets.size) return true;
    if (targets.size > 1024) {
        context->error = 1;
        return false;
    }
    IvRecordedDragTargets *batch = reserve_output(context, sizeof(*batch));
    IvDragTarget *descriptors = reserve_output(context, targets.size * sizeof(*descriptors));
    if (context->error) return false;
    for (uint32_t i = 0; i < targets.size; ++i) descriptors[i] = targets.data[i];
    *batch = (IvRecordedDragTargets){.values = {descriptors, targets.size}};
    if (context->last_targets) context->last_targets->next = batch;
    else context->targets = batch;
    context->last_targets = batch;
    return true;
}

bool iv_draw_texts(IvPresentationContext *context, IvTextSpan texts)
{
    if (!texts.size) return true;
    if (texts.size > 256 || (context->flags & IV_REUSE_DRAWING)) {
        context->error = 1;
        return false;
    }
    IvRecordedTexts *batch = reserve_output(context, sizeof(*batch));
    IvText *values = reserve_output(context, texts.size * sizeof(*values));
    if (context->error) return false;
    for (uint32_t i = 0; i < texts.size; ++i) {
        IvText value = texts.data[i];
        if (value.text.size > 1024) {
            context->error = 1;
            return false;
        }
        char *bytes = reserve_output(context, value.text.size);
        if (!bytes) return false;
        for (uint32_t j = 0; j < value.text.size; ++j) bytes[j] = value.text.data[j];
        value.text.data = bytes;
        values[i] = value;
    }
    *batch = (IvRecordedTexts){.values = {values, texts.size}};
    if (context->last_texts) context->last_texts->next = batch;
    else context->texts = batch;
    context->last_texts = batch;
    return true;
}

void iv_reuse_drawing(IvPresentationContext *context)
{
    if (context->drawing || context->texts || context->grid) context->error = 1;
    context->flags |= IV_REUSE_DRAWING;
}

void iv_set_status(IvPresentationContext *context, IvString status)
{
    if (status.size > 512) {
        context->error = 1;
        return;
    }
    char *copy = reserve_output(context, status.size);
    if (!copy) return;
    for (uint32_t i = 0; i < status.size; ++i) copy[i] = status.data[i];
    context->status = (IvString){copy, status.size};
}

void *iv_session_allocate(uint32_t size) { return iv_alloc(size); }
void iv_session_free(void *buffer) { iv_free(buffer); }
void iv_request_save(IvPresentationContext *context) { context->flags |= IV_SAVE_STATE; }
