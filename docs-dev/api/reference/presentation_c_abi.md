# Presentation C ABI

This reference describes the core wasm32 ABI version 11.
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
| Descriptor prefix | Three bytes in order: descriptor version `1`, ABI version `11`, flags |
| Descriptor flags | Bit 0 enables shared timeline overlays. Bit 1 requests snap candidates for pointer gestures and requires bit 0. |
| Icon suffix | Optional UTF-8 SVG path data in a 16 × 16 coordinate space. The byte limit is 1,024. |
| Artifact size | At most 256 KiB for package distribution |
| Scalar encoding | Little-endian integers and IEEE-754 double values. Pointers in the ABI are 32-bit offsets into the module's memory. |
| String encoding | A pointer and a 32-bit byte length. UTF-8, with a zero length for an empty string. |

`IV_PRESENTATION_DESCRIPTOR(flags, icon)` emits the custom section.
`IV_PRESENTATION_TIMELINE` is bit 0 and `IV_PRESENTATION_TIMELINE_SNAPPING`
is bit 1. Use `IV_PRESENTATION_NO_FLAGS`
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
count at offset 4. The context is 256 bytes on wasm32.

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
| 100 | Edit state, bit 0 for edits pending and bit 1 for the last edit failing |
| 104 | `IvTimelineView` |
| 136 | Span of `IvNumericValue` constructor arguments |
| 144 | Span of `IvControlInput` |
| 152 | Settings panel pointer |
| 156 and 160 | Head and tail of recorded edits |
| 164 | Timeline grid pointer |
| 168 | Project revision as an unsigned 64-bit integer |
| 176 | Span of `IvEditReply` |
| 184 | Number of recorded edits |
| 188 | Span of `IvObservedWaveform` |
| 196 | Span of typed construction argument records |
| 204 | Span of editor inputs |
| 212 | Span of active manual input values |
| 224 | Frame clock in milliseconds |
| 232 and 236 | Context menu and editor feedback pointers |
| 240 and 244 | Head and tail of shape batches |
| 248 | Construction snapshot token |
| 252 | Retained timeline curve pointer |

The host allocates separate frame, gesture, scene-input, and output
storage. The arena holds up to 1 MiB of output. The scene starts with
128 KiB of input storage and grows for encoded inputs up to 1 MiB. Typed
construction arguments have a separate 1 MiB arena. The supported state payload size is 128 KiB. The build tool sets
the module's memory maximum to 8 MiB.

| Read helper | Returned data |
| --- | --- |
| `iv_content_bounds` | Rectangle with origin `(0, 0)` and the lane's width and height |
| `iv_can_edit` | Host edit permission |
| `iv_lane_mode` | `IV_LANE_EDITABLE`, `IV_LANE_COMPACT`, `IV_LANE_MINI`, or `IV_LANE_NANO`, encoded as 0 through 3 |
| `iv_gesture_inputs` | Ordered pointer gesture records for this frame |
| `iv_connected_inputs` | Names of the node's connected inputs. Names are case-sensitive. |
| `iv_observed_trigger_columns` | Up to eight available observation views keyed by the manifest's observation IDs |
| `iv_observed_waveforms` | Viewport waveform regions from manifest-bound inputs or outputs |
| `iv_timeline_view` | The viewport start, fraction, span, and project sample rate. Zero rate means unavailable. |
| `iv_numeric_arguments` | Up to 64 positional constructor arguments, with a distinct kind for unsupported values |
| `iv_construction_argument` | Borrow a typed value by binding index. A null pointer indicates an omitted or unavailable argument. An explicit empty sequence has a non-null pointer to an empty span. |
| `iv_construction_argument_state` | Read `IV_ARGUMENT_AVAILABLE`, `IV_ARGUMENT_OMITTED`, or `IV_ARGUMENT_UNAVAILABLE` for a binding. Omitted means an accepted invocation uses that constructor default. |
| `iv_construction_snapshot` | Token that changes when the host replaces accepted argument data. Use equality to skip rebuilding derived storage. |
| `iv_control_inputs` | Numeric, choice, and action inputs from the settings panel and context menu |
| `iv_editor_inputs` | Context actions, item shortcuts, vertical navigation, and interruptions |
| `iv_manual_inputs` | Enabled manual values for unconnected inputs, keyed by port name |
| `iv_frame_time` | Monotonic clock value for pacing or scheduling work |
| `iv_project_revision` | Revision associated with this frame's project inputs |
| `iv_edit_replies` | Completed requests from this session, in delivery order |
| `iv_edits_pending`, `iv_edit_failed` | Status of host-mediated editing. Keep a preview while pending and restore accepted arguments after a failure. |

An `IvConstructionArgument` record occupies 8 bytes. It holds a value
pointer at offset 0 and a `uint32_t` state at offset 4. An argument outside
the construction budget is unavailable as a whole. A guest can continue
drawing while it waits for that argument.

`IvTimelineView` occupies 32 bytes. It holds a signed 64-bit sample index
at offset 0, two doubles at 8 and 16, and an unsigned 32-bit sample rate
at 24. The fractional offset is measured from the sample index in samples.

`IvNumericValue` is 12 bytes. A span of doubles at offset 0 is followed
by a 32-bit kind at 8. Unavailable, scalar, and sequence have values
0, 1, and 2. Scalars always have one element. Sequences can contain up to
1,024 elements and can be empty. The arguments share a 4,096-number
budget. Absent constructor arguments are omitted. Unsupported and
over-budget arguments preserve their position with kind unavailable.

`IvControlInput` is 40 bytes. An ID string at offset 0 identifies the
setting. The value kind at 20 selects no value, the double at 8, or the
string at 24. The sequence index is at 32.
The phase at 16 is preview, commit, or finish, numbered 0 through 2.
Action clicks use commit with no value. The combined budget for pointer
inputs, control inputs, editor inputs, and edit replies is 512 records per
frame. Gesture, control, and editor spans preserve their own order and
share sequence indices for processing events in delivery order.

A gesture record occupies 64 bytes. The first four 32-bit fields are the
target ID, phase, modifiers, and sequence index. The point at offset 16
and delta at offset 32 are two doubles each. The wheel value at offset 48
is a double. The phases are begin, move, modifiers, wheel, finish, and
cancel in that order. Modifier masks are shift 1, alt 2, control 4, and meta
8. The optional `IvSnapCandidates` pointer at 56 borrows the visible grid
indices and background playhead position. The optional `IvGestureViewport`
pointer at 60 captures the timeline and content dimensions used for the
gesture coordinates. The record occupies 72 bytes. The signed sample
index is at 0. The fraction, span in samples, width, and height are
doubles at 8, 16, 24, and 32. The sample rate and flags are uint32 at 40
and 44. `IV_GESTURE_VIEW_CURVE` supplies the value-axis pan, zoom, and
center as doubles at 48, 56, and 64. `IV_GESTURE_VIEW_CROPPED` selects
the cropped value axis for that gesture.
Coordinates use the lane content's top-left origin. The browser's
pointer drag delivers movement in the coordinate system at pickup. `IV_EDITOR_INTERRUPT` supplies a distinct
history or disconnection notification.
For `IV_EDITOR_VALUE_VIEW` with action `IV_VALUE_PAN`, the input value
is a fraction of the lane height at the time of the pan.

An `IvObservedTriggerColumns` record is 40 bytes. It holds its ID at
offset 0, a span of unsigned 32-bit display columns at 8, a uint64 count
at 16, an error string at 24, a uint32 status at 32, and a uint32 partial
flag at 36. Status values are pending 0, ready 1, unavailable 2, and failed
3. Use the count as a lower bound when partial is one. A column has no
per-event payload. Multiple coincident events increase the count without
adding columns. Each view contains at most 1,024 columns. A pending result
can include columns from a retained window. For the binding format, see
[Build, reload, and test a presentation package](../how-to/presentation_packages.md).

An `IvObservedWaveform` is 32 bytes. Its ID is at 0, its Region span at 8,
and its error at 16. The unsigned 32-bit status and partial flag are at 24
and 28. Status uses the `IV_OBSERVATION_*` values. Pending observations
can still contain retained Regions. A partial view includes a truncated
source result or has reached the host's limit of 256 Regions or 2,048
points per observation. A waveform is a representative sample of the port.

An `IvWaveformRegion` is 32 bytes. Its double bounds at 0 and 8 give
a half-open interval in project samples from the viewport origin. The
signed pace exponent at 16 describes the sample lattice of the port. The
point span at 20 can include the adjacent samples needed for interpolation
at the bounds. Gaps and unavailable samples split the Region. An
`IvWaveformPoint` occupies 24 bytes. Its unsigned 64-bit index at 0 is
the exact native sample index on the port's lattice. The double fields
at 8 and 16 give the viewport-relative offset in project samples and the
sample value. Use `span_samples` in `IvTimelineView` to scale offsets for
drawing. For a port at the project sample pace, the index uses the same
bits as the signed project timeline index.

## Drawing and frame output

| Write helper | Effect |
| --- | --- |
| `iv_draw_polylines(context, span)` | Copy descriptors and vertices into the arena in one call. |
| `iv_reserve_polylines(context, lines, points)` | Reserve descriptors and vertices. Fill every descriptor and supply a vertex subspan in the reservation before returning. Failure returns null pointers. |
| `iv_draw_texts(context, span)` | Copy positions and UTF-8 labels into the arena. Labels are drawn above the line batches. |
| `iv_draw_shapes(context, span)` | Draw rectangle or circle batches with optional borders and hover targets. |
| `iv_register_drag_targets(context, span)` | Copy hit regions with a numeric ID, bounds, drag mode, button, and optional context or cursor behavior. |
| `iv_reuse_drawing(context)` | Use the last drawing, including its timeline grid. Leave the drawing batches and grid empty for this call. Submit current hit targets independently. |
| `iv_set_settings_panel(context, panel)` | Copy one settings panel with its strings and controls. |
| `iv_set_context_menu(context, panel)` | Copy controls for the active context target with the same layout as settings. |
| `iv_set_editor_feedback(context, feedback)` | Report item and value navigation, drag cancellation or cursor return, a wake delay, and an optional seconds ruler inset. |
| `iv_copy_edit_payload(context, data, size)` | Copy bytes into the arena at an 8-byte aligned address. Returns null on failure. |
| `iv_submit_edit(context, edit)` | Record one command with its payload already retained in the output arena. |
| `iv_set_timeline_grid(context, grid)` | Copy one regular grid declaration for host projection and optional seeking. |
| `iv_set_status(context, status)` | Copy a status string for the host. The byte limit is 512. |
| `iv_request_save(context)` | Ask the host to serialize after this frame. Use at commit points such as drag finish. |

Each arena batch occupies 12 bytes, with its values span followed by
the next batch pointer. A polyline descriptor is 72 bytes, with 32-bit
ID and ink, its point span, and an `IvStroke` at 16. The stroke carries
opacity, width, optional dash or fill style, gradient endpoints, and a
target for hover feedback. A point is 16 bytes. Text records
are 32 bytes, with ID and ink followed by an `IvPoint` and `IvString`.
Drag targets are 64 bytes, with ID and kind, bounds, flags, a hint, and
shortcuts. Shapes are 64 bytes and store the selected rectangle or circle
geometry at offset 24.

IDs for marks are nonzero and unique within a frame. Drag target IDs
are nonzero and unique within their own list. Ink uses the
`IV_INK_*` values in the guest header. The host resolves these roles
against its own palette. The host caps the frame at 4,096 marks and
16,384 vertices, with up to 4,096 points in one polyline. It accepts up to
256 text records, 1,024 bytes in each label, and 1,024 drag targets.
Helper failure sets a frame error. The host discards an invalid frame.

## Settings, edits, and regular grids

An `IvSettingsPanel` is 16 bytes. Its label string is followed by a span
of `IvSetting`. A setting occupies 88 bytes. Its ID and label are strings at
0 and 8. Its row, kind, and flags are uint32 values at 16, 20, and 24. The
value, minimum, maximum, and default for numeric knobs are doubles at 32,
40, 48, and 56. Kinds are text 0, knob 1, action 2, and choice 3. For a
choice, the index at 28 selects an option from the span at 64. Knob step and scrub speed are
at 72 and 80. Zero requests the host default. The integer flag
sets the knob's default gesture step. The disabled flag applies to actions.
Begin with row 0 and send contiguous rows in display order. The frame
limit is 256 settings, 64 rows, and 12 controls per row. Use unique IDs
within the panel. The host renders the controls and owns their interaction.

`IvNodeEdit` is 20 bytes. It holds four unsigned 32-bit fields at offsets
0 through 12, followed by a payload pointer at 16. The fields are the
binding index, gesture ID, request ID, and flags. A zero gesture ID makes
the edit standalone. A nonzero ID groups edits for undo until a finish.
A nonzero request ID asks the host to return a reply. Use distinct
request IDs during the life of the session.

With zero flags, the host delivers the edit in order. Use
`IV_EDIT_REPLACE_PENDING` for a full-state preview whose newer payload may
replace a queued preview with the same binding and gesture. This command
requires a nonzero gesture and a zero request ID. It may be presented
again while an earlier preview is in flight. Use `IV_EDIT_FINISH` with a
gesture ID and zero binding and payload. Supply a request ID if the guest
needs to await the finish acknowledgement. The frame supports 32 commands
and the host allows up to eight open edit gestures per lane.

`IvEditReply` is 24 bytes. Its request ID and status are unsigned 32-bit
fields at 0 and 4. Status is `IV_EDIT_ACCEPTED` or `IV_EDIT_REJECTED`.
The unsigned 64-bit revision is at 8 and a diagnostic `IvString` is at 16.
A zero revision means the command returned no revision. A successful
response can precede delivery of the accepted project snapshot. Use
`iv_project_revision` to tell when the snapshot has caught up. Replies
belong to the worker session that submitted the request.

Include an `iv.presentation.events` custom section when the guest sends
events. The optional section is UTF-8 JSON and describes up to 64 bindings
in array order within 64 KiB. Each entry supplies the node-event
`nominalId`, `definitionFingerprint`, and reflected `payload` schema.
The host reads the section when it initializes the module. The live
payload uses the natural wasm32 layout of the generated record. Booleans
occupy one byte. The declared scalar and enum widths determine their size
and alignment. A sequence uses a pointer and element count. Records are
aligned to their largest field. `IvConvertedScalar` is 24 bytes, with
kind at 0, its numeric value at 8, and `IvString` at 16. Boolean values use
kind 0 and number 0 or 1. Number and string use kinds 1 and 2.

Copy the payload record and all strings and sequences it references into
the frame's arena before submitting it. The host validates the record and
its referenced spans within the arena. The host supports up to 4,096
decoded values and 64 KiB of edit string data per frame. Node-event RPC accepts
exactly representable JavaScript integers. A converted scalar can carry
an exact sample index as a decimal string. The host uses the existing
node-event protocol to validate the binding and the target before delivery.
The [C++ generator](presentation_cpp.md#node-events) emits the records and
binding section together.

An `IvTimelineGrid` is 56 bytes. Its base period is a rational number of
samples, with unsigned 64-bit numerator and denominator at 0 and 8.
The numerator must be at least the nonzero denominator. A stride span
at 16 names up to 32 depths in coarse-to-fine order. Each positive stride
divides the preceding stride, and the final stride is 1. The host anchors
the grid at sample 0 and rounds on each signed side of the timeline seam.
A same-length span of line starts at 24 gives y as a fraction of the height.
Minimum spacing and opacity are doubles at 32 and 40. Minimum spacing
uses logical pixels from 1 to 128. Opacity and line starts range from 0 to 1.
The uint32 ink and flags are at 48 and 52. `IV_GRID_SEEK` exposes the
grid to host playhead seeking. `IV_GRID_LABEL_MAJOR` labels major lines
with their signed index plus 1. The host limits mark density at every zoom.

## Retained timeline curves

`iv_set_timeline_curve` records an `IvTimelineCurve` once per frame. The
host can project these records after the viewport changes. The point
span contains up to 32,768 points in nondecreasing signed sample order.
Equal-sample points retain their sequence within their group. The first
point in a group supplies the incoming value and the last supplies the
outgoing interpolation and coefficients. Segments advance cyclically,
use half-open intervals, and can be empty, hold, or Bézier. A single
nonempty group holds its last value over the full sample cycle.

An `IvTimelineCurvePoint` occupies 56 bytes. It carries the signed
sample index at 0, start and end derivatives multiplied by the segment
duration at 8 and 16, and
the binary32 value and tension at 24 and 28. The uint32 fields at 32,
36, 40, and 44 specify the point target, point mark base, tension
target, and tension mark base. A zero tension target and mark omit the
handle. Only the outgoing Bézier point in a group can have a tension
handle. Each mark base reserves two consecutive mark IDs. Interpolation
at 48 uses `IV_CURVE_EMPTY`, `IV_CURVE_HOLD`, or `IV_CURVE_BEZIER`.
Flags at 52 mark the selected and active points. The host reserves
track mark IDs 1001 through 2156 for the projected curves and bounds.
Targets and marks must have unique IDs within their respective spaces.
For a Bézier segment, let `t` and `u` be the fractions of the duration
elapsed and remaining, and let `d` be the last value minus the first.
The host adds `tension * t * u * ((start_delta - d) * u + (d - end_delta) * t)`
to the linear interpolation and clamps the result between the endpoint
values.

An `IvTimelineCurve` occupies 64 bytes. The point span is at 0. The
value-axis pan, zoom, and fractional center are doubles at 8, 16, and
24. The uint32 fields at 32, 36, 40, and 44 give the point generation,
flags, selection target, and addition target. The minimum and maximum
input names are `IvString` fields at 48 and 56. Each name also names
the manifest's waveform observation ID for that port. An empty name
omits the corresponding bound. The host projects and interpolates
these observations. After the first curve frame it can omit these
waveforms from the guest's scene inputs.

Point memory must remain readable after `iv_present` returns. Change
the generation when any point data changes. The host copies the points
once per generation. Axis settings and flags arrive every guest frame.
`IV_CURVE_EDITABLE` enables the point and background targets.
`IV_CURVE_CROPPED` selects a linear value axis centered at `pan`, with
visible span `2 * max(abs(pan) * 2^-23, 1 / zoom)`. The curved axis uses
`0.5 + atan((value - pan) * min(1, zoom)) / pi` as its fraction from the
bottom. If zoom exceeds one, it subtracts `center`, multiplies by zoom,
and adds 0.5.
`IV_CURVE_VIEWPORT_DRAWING` identifies transient pixel drawing that
depends on the specific timeline origin and span, such as a marquee or
snap highlight. When the flag is clear, the host may reuse pixel drawing
across horizontal pans at the same layout, sample rate, and grid mode.

[Historical ABI 10 requirements](../../historical/presentation_abi_10_before_retained_timeline_curves_2026-11-23.md)

## Layout checks

These compile-time assertions verify the layout described above for wasm32.

<!-- check-example: abi_layout.c -->
```c
#include <intravenous/presentation/abi.h>

_Static_assert(IV_PRESENTATION_ABI_VERSION == 11, "ABI version");
_Static_assert(sizeof(IvPresentationContext) == 256, "context size");
_Static_assert(offsetof(IvPresentationContext, trigger_columns) == 80, "observation span");
_Static_assert(sizeof(IvObservedTriggerColumns) == 40, "observation record");
_Static_assert(offsetof(IvObservedTriggerColumns, count) == 16, "trigger count");
_Static_assert(offsetof(IvPresentationContext, waveforms) == 188, "waveform span");
_Static_assert(offsetof(IvPresentationContext, construction_arguments) == 196, "construction argument span");
_Static_assert(sizeof(IvConstructionArgument) == 8, "construction argument record");
_Static_assert(sizeof(IvWaveformPoint) == 24 && sizeof(IvWaveformRegion) == 32, "waveform layouts");
_Static_assert(sizeof(IvText) == 32, "text record");
_Static_assert(offsetof(IvPresentationContext, timeline) == 104, "timeline view");
_Static_assert(sizeof(IvControlInput) == 40 && sizeof(IvSetting) == 88, "controls");
_Static_assert(sizeof(IvNodeEdit) == 20 && sizeof(IvEditReply) == 24, "node edits");
_Static_assert(sizeof(IvTimelineGrid) == 56, "grid");
_Static_assert(sizeof(IvGestureViewport) == 72 && offsetof(IvGestureInput, viewport) == 60, "gesture viewport");
_Static_assert(sizeof(IvTimelineCurvePoint) == 56 && sizeof(IvTimelineCurve) == 64, "retained curve");
_Static_assert(offsetof(IvPresentationContext, timeline_curve) == 252, "retained curve output");
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
