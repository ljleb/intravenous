# Presentation C ABI

This reference describes the core wasm32 ABI version 4.
[`abi.h`](../../../src/intravenous/presentation/abi.h) declares the exports
and wire layout. [`presentation.h`](../../../src/intravenous/presentation/presentation.h)
declares the C guest API.
[`guest_runtime.c`](../../../src/intravenous/presentation/guest_runtime.c)
implements the guest helpers. The
[host adapter](../../../vscode/client/src/wasmPresentation.ts) validates
the WebAssembly interface and publishes its drawing records.

## Artifact

| Field | Contract |
| --- | --- |
| Container | Core wasm32 module with exported linear memory named `memory` and zero imports |
| Descriptor | Exactly one custom section named `iv.presentation` |
| Descriptor prefix | Three bytes in order: descriptor version `1`, ABI version `4`, flags |
| Descriptor flags | Bit 0 enables the shared timeline overlays. All other bits are reserved. |
| Icon suffix | Optional UTF-8 SVG path data in a 16 × 16 coordinate space. The byte limit is 1,024. |
| Artifact size | At most 256 KiB for package distribution |
| Scalar encoding | Little-endian integers and IEEE-754 double values. Pointers in the ABI are 32-bit offsets into the module's memory. |
| String encoding | A pointer and a 32-bit byte length. UTF-8, with a zero length for an empty string. |

`IV_PRESENTATION_DESCRIPTOR(flags, icon)` emits the custom section.
`IV_PRESENTATION_TIMELINE` is bit 0. Use `IV_PRESENTATION_NO_FLAGS`
for zero and `""` for the default icon. Linkers must preserve the custom
section.

## Exports and lifetime

| Export | Result and contract |
| --- | --- |
| `iv_alloc(size)` | Allocate guest bytes. The host checks 8-byte alignment, range, and overlap for its buffers. |
| `iv_free(buffer)` | Release a guest allocation. The SDK treats a null pointer as empty. |
| `iv_create_session()` | Construct the session and return its nonzero guest-memory handle. Zero indicates failure. |
| `iv_destroy_session(session)` | Destroy a successfully created session on normal shutdown. A trapped or timed-out worker can be terminated by the host. |
| `iv_restore_state(session, bytes, size)` | Restore before drawing. A size of zero requests initial state. Return zero on success. The host invalidates reused drawing after restore. |
| `iv_present(session, context)` | Read the borrowed frame inputs and submit drawing, targets, or a save request. The return type is void. |
| `iv_save_state(session, bytes, capacity)` | Write one UTF-8 JSON object or `null` within the buffer and return the positive byte length. Other results cause the host to reject the save. |

The host holds a session handle for one worker. Session storage persists
across frames. Frame inputs and arena pointers are valid during the call
to `iv_present`. For longer-lived data, allocate guest-owned memory with
`iv_session_allocate` and release it with `iv_session_free`.

`IV_DEFINE_PRESENTATION(State, Present, Save, Restore)` emits the session
and frame exports for a C state struct. The macro initializes the state
to zero and uses SDK allocation and destruction. Its callbacks take
`State*` and `IvPresentationContext*`, `const State*` and the
destination buffer and capacity, or `State*` and an `IvString`,
respectively. A guest that has custom lifetime rules can implement the
exports directly. The SDK calls no host functions from WASM.

## Host context and borrowed views

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

The host allocates separate frame, gesture, scene-input, and output
storage. The arena holds up to 1 MiB of output. The scene input budget is
128 KiB. The supported state payload size is 128 KiB. The build tool sets
the module's memory maximum to 8 MiB.

| Read helper | Returned data |
| --- | --- |
| `iv_content_bounds` | Rectangle with origin `(0, 0)` and the lane's width and height |
| `iv_can_edit` | Host edit permission |
| `iv_lane_mode` | `IV_LANE_EDITABLE`, `IV_LANE_COMPACT`, `IV_LANE_MINI`, or `IV_LANE_NANO`, encoded as 0 through 3 |
| `iv_gesture_inputs` | Up to 512 ordered gesture records for this frame |
| `iv_connected_inputs` | Names of the node's connected inputs. Names are case-sensitive. |
| `iv_observed_trigger_columns` | Up to eight available observation views keyed by the manifest's observation IDs |

A gesture record occupies 56 bytes. The first four 32-bit fields are the
target ID, phase, modifiers, and a reserved field. The point at offset 16
and delta at offset 32 are two doubles each. The wheel value at offset 48
is a double. The phases are begin, move, modifiers, wheel, finish, and
cancel in that order. Modifier masks are shift 1, alt 2, control 4, and meta
8. A cancel with ID zero interrupts the session's current interaction.
Coordinates use the lane content's top-left origin.

An `IvObservedTriggerColumns` record is 40 bytes. It holds its ID at
offset 0, a span of unsigned 32-bit display columns at 8, a uint64 count
at 16, an error string at 24, a uint32 status at 32, and a uint32 partial
flag at 36. Status values are pending 0, ready 1, unavailable 2, and failed
3. Use the count as a lower bound when partial is one. A column has no
per-event payload. Multiple coincident events increase the count without
adding columns. Each view contains at most 1,024 columns. A pending result
can include columns from a retained window. For the binding format, see
[Build, reload, and test a presentation package](../how-to/presentation_packages.md).

## Drawing and frame output

| Write helper | Effect |
| --- | --- |
| `iv_draw_polylines(context, span)` | Copy descriptors and vertices into the arena in one call. |
| `iv_reserve_polylines(context, lines, points)` | Reserve descriptors and vertices. Fill every descriptor and supply a vertex subspan in the reservation before returning. Failure returns null pointers. |
| `iv_draw_texts(context, span)` | Copy positions and UTF-8 labels into the arena. Labels are drawn above the line batches. |
| `iv_register_drag_targets(context, span)` | Copy hit regions for this frame. A target specifies a numeric ID, a rectangle, and relative or absolute drag. |
| `iv_reuse_drawing(context)` | Use the last drawing. Leave the line and text batches empty for this call. Submit the current hit targets independently. |
| `iv_set_status(context, status)` | Copy a status string for the host. The byte limit is 512. |
| `iv_request_save(context)` | Ask the host to serialize after this frame. Use at commit points such as drag finish. |

Each arena batch occupies 12 bytes, with its values span followed by
the next batch pointer. A polyline descriptor is 16 bytes, with 32-bit
ID and ink followed by its point span. A point is 16 bytes. Text records
are 32 bytes, with ID and ink followed by an `IvPoint` and `IvString`.
Drag targets are 40 bytes, with ID and kind followed by the four doubles
in an `IvRectangle`.

IDs for marks are nonzero and unique within a frame. Drag target IDs
are nonzero and unique within their own list. Ink uses the
`IV_INK_*` values in the guest header. The host resolves these roles
against its own palette. The host caps the frame at 4,096 marks and
16,384 vertices, with up to 4,096 points in one polyline. It accepts up to
256 text records, 1,024 bytes in each label, and 1,024 drag targets.
Helper failure sets a frame error. The host discards an invalid frame.

## Layout checks

These compile-time assertions verify the layout described above for wasm32.

<!-- check-example: abi_layout.c -->
```c
#include <intravenous/presentation/abi.h>

_Static_assert(IV_PRESENTATION_ABI_VERSION == 4, "ABI version");
_Static_assert(sizeof(IvPresentationContext) == 104, "context size");
_Static_assert(offsetof(IvPresentationContext, trigger_columns) == 80, "observation span");
_Static_assert(sizeof(IvObservedTriggerColumns) == 40, "observation record");
_Static_assert(offsetof(IvObservedTriggerColumns, count) == 16, "trigger count");
_Static_assert(sizeof(IvText) == 32, "text record");
_Static_assert(IV_LANE_NANO == 3 && IV_OBSERVATION_FAILED == 3, "encoded enums");
```

## Package binding

The server publishes artifacts by digest through `presentations.list` and
`presentations.fetch`. The host parses the descriptor once per fetched
digest. The manifest provides the scoped presentation identity, offered
node definitions, title, and observation bindings. Only the defining
package may provide `initialConfiguration`. A project package can offer a
presentation for an imported node, including a builtin definition.
[Catalog implementation](../../../src/intravenous/packages/presentation_artifacts.cpp)
