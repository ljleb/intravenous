# C++ presentation SDK

[`presentation.hpp`](../../../src/intravenous/presentation/presentation.hpp)
provides C++20 source interfaces. The compiled artifact uses the
[presentation C ABI](presentation_c_abi.md). Classes, methods, references,
and C++ object layouts stay inside the guest.

## Class registration

| Declaration | Meaning |
| --- | --- |
| `IV_PRESENTATION_CLASS(Type)` | No flags and the host's default icon |
| `IV_PRESENTATION_CLASS(Type, Flags)` | Explicit descriptor flags and default icon |
| `IV_PRESENTATION_CLASS(Type, Flags, "path")` | Explicit flags and icon path |

Register one class per WASM module. The class needs a public default
constructor, public destructor, and `void present(iv::presentation::Context&)`.
The SDK constructs the class in guest storage with up to 8-byte alignment.
The destructor runs when the host calls `iv_destroy_session` on normal
shutdown.

Persistent classes implement both methods below.

```cpp
int32_t save(char* bytes, uint32_t capacity) const;
int32_t restore(IvString serialized);
```

The required and optional methods are checked at compile time. Restore
gets a zero-size view for initial state. A class that omits the methods
accepts empty restore input. Use the
[JSON helper](../../../src/intravenous/presentation/state_json.h) if it fits
your state format. It is used for persistence and is independent of frame
rendering.

## Context

| Method | Return or effect |
| --- | --- |
| `content_bounds()` | `IvRectangle` in lane-relative logical pixels |
| `can_edit()` | Host edit permission, independent of visual size |
| `lane_mode()` | A value from `IV_LANE_EDITABLE` through `IV_LANE_NANO` |
| `gestures()` | `Span<const IvGestureInput>` |
| `connected_inputs()` | `Span<const IvString>` of connected input names |
| `observed_trigger_columns()` | `Span<const IvObservedTriggerColumns>` for manifest bindings |
| `observed_waveforms()` | `Span<const IvObservedWaveform>` for manifest bindings |
| `observed_waveform(id)` | Pointer to the matching borrowed waveform, or null when no view is available |
| `timeline_view()` | `IvTimelineView` with exact first sample, span, and project sample rate |
| `numeric_arguments()` | `Span<const IvNumericValue>` in constructor argument order |
| `control_inputs()` | `Span<const IvControlInput>` in delivery order |
| `edits_pending()`, `edit_failed()` | Preview and accepted edit state from the host |
| `set_settings_panel(panel)` | Copy the panel and its controls |
| `begin_edit()` | Reserve an `EditGesture` ID for an undo group |
| `send(event, gesture = {})` | Copy a generated event to the output arena and return an `EditRequest` |
| `replace_pending(event, gesture)` | Submit a full-state preview that can replace a queued preview in this gesture. Returns whether it was recorded. |
| `finish_edit(gesture)` | Record the end of an undo group and return an `EditRequest` |
| `edit_replies()` | `Span<const IvEditReply>` with replies delivered for this frame |
| `project_revision()` | Revision associated with the accepted frame inputs |
| `set_timeline_grid(grid)` | Submit an `IvTimelineGrid` for host projection |
| `draw_polylines(lines)` | Copy a `Span<const IvPolyline>` to the arena |
| `reserve_polylines(lines, points)` | Return an `IvPolylineBatch` with writable descriptor and point pointers |
| `draw_texts(texts)` | Copy a `Span<const IvText>` into the drawing |
| `register_drag_targets(targets)` | Submit a `Span<const IvDragTarget>` |
| `reuse_drawing()` | Keep the last drawing and supply the current targets |
| `set_status(status)` | Set the host status from an `IvString` |
| `request_save()` | Serialize the session at the end of the frame |

Return values, lengths, and errors follow the [C ABI limits](presentation_c_abi.md).
All views are borrowed for `present`. `Span<T>` has `data`, `size`,
`begin()`, and `end()`. Its size counts elements. `IvString::size`
counts bytes.

## Node events

Generate the event header from the compiler's node-event protocol JSON.
The selection file maps each C++ record name to a nominal event ID. For
an overloaded identity, use an
object with `nominalId` and `definitionFingerprint`. Generate one header
for each artifact and include it in its entry source. The header contains
the binding section and the C++ records that correspond to its layout.
It supports scalar fields, strings, enums, records, and sequences. Fields
with the protocol's `nodeAddress` kind are currently unsupported.

```sh
python3 scripts/generate_presentation_events.py protocol.json names.json events.hpp --namespace my_events
```

Use `converted_string` to put an exact decimal sample index into an
`IvConvertedScalar`. String and span data may point to local arrays.
`send` and `replace_pending` copy that data before returning. A request
with ID zero was not submitted. A nonzero `EditRequest::id` can be matched
against `IvEditReply::request` in a later frame. When an edit depends on
another edit's success, wait for the first reply before submitting the
dependent edit. Use `project_revision()` when the guest also needs the
accepted snapshot to contain the edit.

A gesture groups the edits that share its ID. Call `finish_edit` when the
group is complete. Reserve `replace_pending` for independent full-state
previews such as a Beats settings change. These previews have no individual
reply. Queue edits that must each happen with `send`.
[Send a node event from a C++ presentation](../tutorials/presentation_event.md)

## Session storage

Keep working state in members when practical. `SessionBuffer(size)`
owns a guest allocation returned by `iv_session_allocate`. `data()`
returns the address or null. The class is move-constructible and has no
copy operations. Its destructor releases the allocation.
A worker cutoff after a trap or timeout releases the module instance
without requiring a successful destructor call.

The supplied builder compiles freestanding C++20 with RTTI and exceptions
disabled. If a package uses a C++ standard library, it must supply a
WASM-compatible build and allocator that satisfy the import-free ABI.

[Marker using saved state and direct reservations](../../../examples/wasm-presentation/packages/marker/presentations/marker.cpp) ·
[Click Detector reading typed observations and drawing text](../../../src/intravenous/builtin_packages/builtin/presentations/click_detector.cpp) ·
[Beats using the settings, editing, and grid API](../../../src/intravenous/builtin_packages/builtin/presentations/beats.cpp)
