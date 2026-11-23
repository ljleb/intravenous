# Presentation ABI 4 context layout

Historical excerpt from the C ABI reference, preserved on 2026-11-22.
ABI 5 extended the frame to carry controls, typed edits, and a timeline grid.
See the [current ABI reference](../api/reference/presentation_c_abi.md).

All span records are 8 bytes, with their pointer at offset 0 and element
count at offset 4. The context is 104 bytes on wasm32.

| Context byte offset | Field |
| --- | --- |
| 0 and 8 | Width and height in logical pixels |
| 16 | Span of `IvGestureInput` |
| 24 and 28 | Editability and output flags |
| 32 and 36 | Output arena pointer and byte capacity |
| 40 and 44 | Used byte count and error flag |
| 48 and 52 | Head and tail of polyline batches |
| 56 and 60 | Head and tail of drag target batches |
| 64 | Status string |
| 72 and 76 | Head and tail of text batches |
| 80 | Span of `IvObservedTriggerColumns` |
| 88 | Span of connected input names as `IvString` records |
| 96 | Lane mode |

## Beats seeking before the port

From the WASM migration checkpoint in Implementation Fronts. The current
checkpoint replaces the in-process reader with a host-projected grid.
[Current migration notes](../implementation_fronts_direction.md#project-authored-presentation-lanes-2026-11-20)

> `createBeatsPresentation` implements `openSeekReader` as a synchronous
> reader over the displayed `BeatGrid`. `usePlayheadSeek` asks it about
> windows outside the viewport during a drag. A worker cannot supply that
> answer synchronously without blocking pointer motion. Move scheduling and
> reuse of seek windows into the host. Capture the displayed grid when a
> drag starts and use that same grid for the entire gesture. Verify the
> detent behavior when an outside-window result is pending before porting
> Beats.
