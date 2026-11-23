# Write a C presentation

Make a fresh scratch copy of the [marker project](../../../examples/wasm-presentation/)
using the [C++ tutorial's setup](presentation_cpp.md). This time you will
publish a C program that draws a diagonal. The project already targets a Beats
node, so you can use its lane to see the result.

1. Save the following code as `packages/marker/presentations/diagonal.c`
   in the scratch copy.

<!-- check-example: diagonal.c -->
```c
#include <intravenous/presentation/presentation.h>

typedef struct { unsigned calls; } Diagonal;

static void render(Diagonal *state, IvPresentationContext *frame)
{
    ++state->calls;
    IvRectangle bounds = iv_content_bounds(frame);
    const IvPoint points[] = {{0, bounds.height}, {bounds.width, 0}};
    const IvPolyline line = {1, IV_INK_OUTPUT, {points, 2}};
    iv_draw_polylines(frame, (IvPolylineSpan){&line, 1});
}

static int32_t save(const Diagonal *state, char *bytes, uint32_t capacity)
{
    (void)state;
    (void)bytes;
    (void)capacity;
    return -1;
}

static int32_t restore(Diagonal *state, IvString saved)
{
    (void)saved;
    state->calls = 0;
    return 0;
}

IV_DEFINE_PRESENTATION(Diagonal, render, save, restore)
IV_PRESENTATION_DESCRIPTOR(IV_PRESENTATION_NO_FLAGS, "");
```

2. Build it from the repository in the development shell. Use the
   scratch directory from step 1.

   ```sh
   marker="$scratch/packages/marker/presentations"
   python3 scripts/build_presentation.py "$marker/diagonal.c" "$marker/marker.wasm"
   ```

3. Open the scratch project or return to its running editor. The diagonal
   fills the lane. Resize the lane and watch the line follow both corners.
4. Change `IV_INK_OUTPUT` to `IV_INK_WARNING` and repeat the build. The
   next frame uses the editor's warning color.

The C SDK generated the same session exports the C++ marker uses. The
diagonal reads its dimensions from the frame and submits geometry in one
call. The host owns the window and applies the color.
[Presentation C ABI](../reference/presentation_c_abi.md)
