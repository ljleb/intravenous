# Vue Lane, Meter, Port-Control, Cable, And Automation Interaction Direction

_Status: normative owner intent recorded 2026-11-16, **not an implementation
checkpoint or an instruction to implement all items immediately**. Priorities and
prerequisites are tracked in [Vue Client Implementation Stages](./vue_client_implementation_stages.md).
This document refines [Node Presentation And Manual Controls](./node_presentation_and_manual_controls_direction.md),
[Vue Output Observation](./vue_output_observation_direction.md), and
[Project Playback And Presentation Grids](./project_playback_and_presentation_grids_direction.md).
The replaced inline-value and selection-only cable presentation requirements
are preserved [historically](./historical/vue_lane_controls_and_cables_before_refinement_2026-11-16.md)._

This is presentation and graph-capability intent, not a new family of backend
presentation registration or an observation mutation API. Port identities,
connection semantics, undoable edits, observation interests, and data snapshots
retain their existing owners. The current Vue rendering is evidence of the
implementation, not a constraint on these visual requirements.

## 1. Channel-correct meters and two display modes

A lane observing a stereo `Sample` output must show **two distinct channel
meters**, corresponding to the actual resolved output channels. More generally,
show a channel's own observation result for every displayed channel; never
silently combine a multichannel source into one mono meter. The observation
operation, result schema, channel selectors and Vue rendering must agree on the
resolved physical channel layout. Missing data or an unsupported operation is
shown as unavailable, not as a zero measurement. Do not infer channel count
from an unresolved declaration's allowed-type set.

The default **level** meter uses green level fill and a legible dBFS amplitude
scale, with a meaningful lower floor and labelled intermediate divisions. For
positive sample magnitude `a`, use `20 * log10(a)` to locate the level; zero
magnitude falls below the displayed floor rather than evaluating `log10(0)`.
Unity magnitude is 0 dBFS. Values above unity remain distinguishable as
above-reference/over-range, rather than masquerading as exactly 0 dBFS. The
precise floor, labelled tick intervals, RMS/peak indication, and clipping
presentation may be tuned for readability; a raw linear amplitude fraction is
**not** a dB meter. The existing `level` observation remains a sample-level
block reduction, not an inter-sample true-peak claim.

Both **`level`** and **`value`** meter modes show the same per-channel
sample-magnitude peak and RMS readings. Only the scale changes: `level` uses
dBFS, while `value` uses a linear magnitude scale (on 0–1, `0.5` is halfway).
Value mode is not a display of the final signed sample of the block. This owner
update on 2026-11-16 supersedes the earlier signed-sample requirement, preserved
[historically](./historical/vue_meter_value_before_shared_peak_display_2026-11-16.md).
A mode switch changes presentation only; it does not request another capture
operation or compile another graph.

Do not reserve space beside an output visualization for its name or a scale
switch button. Output and channel identities remain available in hover status
and accessibility labels. Right-clicking the visualization opens a context menu
with Level (dBFS) and Value (linear), showing the selected mode. The same menu
can gain more visualization choices later. Both stereo channels share the
output's display preference. Keyboard users can open it from a focused meter
with the context-menu key or Shift+F10.

Use the same latest-peak fill and held peak marker in both modes. The fill shows
the latest received block's sample-magnitude peak immediately, on both rises
and falls. Do not smooth it toward RMS or interpolate it as a fictional stream
of measurements between telemetry updates. Here, instantaneous means the latest
received block peak, not an audio-rate sample value or a guarantee of observing
every peak between publications. RMS can remain in the numeric readout.

A new peak snaps the held marker up and resets its downward velocity. After a
short hold, the marker
falls with increasing speed toward zero, staying at least as high as the
currently measured peak. Run only this held-marker motion on the client at
display refresh, independently of telemetry delivery. Missing/unavailable data
remain unavailable rather than
becoming silence. Reset obsolete-source motion on activation/invalidation and
stop animation when hidden or settled. Hold and release tuning can be adjusted
for feel without changing capture semantics. This owner refinement supersedes
the [RMS-fill motion contract](./historical/vue_rms_fill_before_latest_peak_2026-11-16.md).

Both modes have **square corners**, no decorative rounding, and consistent
ruler-like grading. Reuse the existing time ruler's tick/label/gradation
presentation code or extract a shared grading primitive instead of building a
second unrelated tick renderer. The meter's dB or linear domain determines
the tick positions; the ruler's typography and alignment supply visual
consistency. Switching Vue display modes changes presentation/observation
interest only, not graph signal values.

**Use the lane's remaining horizontal width for its output meter.** After the
necessary port/knob handles and labels are laid out, the meter area stretches
to fill the rest of the lane instead of occupying a tiny fixed-width indicator.
Stereo and other multichannel meters remain distinct, clearly identified
channel readings within that full-width area, with aligned graduations and
legible levels. Do not sacrifice input/output controls or force an artificial
lane width to achieve the fill; the meter adapts to the available space.

A default lane with no output ports observes its first continuous input when
the input supports Sample observation. It uses the same per-channel meter for
live level data and the same waveform view for windowed observations. Lanes
with outputs show their usual output observations.

The default visualization fills its allocated viewport without surrounding
horizontal or vertical padding. Keep the channels distinguishable; spacing
between channels is separate from an outer margin around the visualization.

### Visualization performance and display settings (deferred)

Add visualization-specific performance and display preferences later. Meters
need selectable refresh rates and display options, including held-peak timing
and appearance. Distinguish telemetry delivery cadence from client animation
frame rate; rendering the same received value more often does not create more
audio measurements. Other visualization types may need different quality,
resolution or update controls rather than sharing one universal settings set.

For now, store these preferences in VS Code with presentation preferences, not
in node construction data or the project's audio graph. Rate/display changes
that only affect presentation or delivery must not rebuild DSP code. Preserve
the distinction if a future quality option actually changes producer work.
This records future work only; no new settings UI or configurable rates are
implemented by this refinement.

### Future signal-distribution and spectral views (deferred)

A `Sample` output may later be presented with a **time-local amplitude-density
view**: signed sample value on a default `-1` to `+1` horizontal axis and an
exponentially fading, optionally Gaussian-smoothed density distribution. Its
purpose is to reveal the recent distribution and concentration of sample
values, not to act as another clipping meter. Using roughly one exclusive
histogram bin per displayed horizontal pixel is a reasonable starting point;
expose optional resolution/quality controls only when useful, separately from
kernel bandwidth and decay time. Keep different channels identifiable.

A second, later **time-frequency** view may show an STFT spectrogram or a
wavelet-based representation with meaningful axes and transform settings.
These are independent observation presentations, **not** alternate `level`/
`value` grading modes, and they must not delay current meter, header, or
curve-editor work. Their data/operation requirements are specified in
[Vue Output Observation](./vue_output_observation_direction.md#deferred-sample-amplitude-density-and-time-frequency-views-owner-intent-2026-11-16).

## 2. Input knobs and ports as one visual input

An eligible Sequential input's manual knob and port are two parts of **one
input control**, not two unrelated rows. Favor a compact composition with the
**knob on the left, its port circle immediately to the right**, and the input
name to the right of the combined control. Keep separate pointer targets and
normal port/knob gestures, even if their visuals appear joined. Directly shown
inputs and temporarily expanded port lists must use the same composition.
Place a knob's label close to its upper-right edge for inputs and its
upper-left edge for outputs.
In editable headers and their continuations, center a socket without a knob
on the knob column for its direction. Use the same row heights for both
compositions. Individual event sockets keep their rounded-square shape.
Applied cables use solid strokes for either port kind. See the
[previous socket alignment](./historical/vue_ports_before_knob_column_alignment_2026-11-24.md).
The input name must remain readable when cables cross the lane area; cable
layout/opacity must not obscure it.

In an editable lane header, the input and output names on each row share
the space between their fixed controls. A short name uses only its own
width and leaves the remainder for the opposite name. A single name can
use the full space. If both names need more than half, divide the space
equally and truncate each name with an ellipsis. Hovering a port shows its
full name in the status bar.

### Input-wide connection drop targets (owner follow-up, deferred)

When dragging a port connection over an input, treat that input's related
elements as the same candidate destination: its port handle, knob, name and
associated row/control area, including within a port continuation or popover.
Hovering any of those elements must resolve the intended input and show the
same ghost connection as hovering its handle. Releasing there creates the
connection when the existing compatibility rules permit it. This widens the
target area; it does not relax connection compatibility or change the gestures
used to tweak a knob when no connection is being dragged. Implementation is
deferred from the current feature.

### Stable knob range and scrub sensitivity (owner refinement, 2026-11-16)

A finite minimum without a maximum maps to the beginning of the radial arc;
a finite maximum without a minimum maps to its end. The remaining unbounded
direction uses a stable monotone curved mapping, with its scale derived from
the declaration rather than the current knob value. Two finite bounds retain
their linear mapping. One-sided bounds must not be drawn as if the knob were
unbounded on both sides.

The 1× scrub speed must remain stable when the knob's current value changes.
Sample Sequential input options, including module boundary options, can declare
an optional positive finite `scrub_speed`: logical value units per vertical
pointer pixel at 1×. It is presentation metadata, alongside default/min/max;
it does not alter signal processing or the manual-control value. The frontend
metadata spelling is `scrubSpeed`. Absent or unusable hints use stable defaults:
one two-hundredth of a finite declared range per pixel, otherwise one hundredth
of the larger of the declared default's magnitude and 1. Neither rule depends
on the current manual value or an incoming connected signal.

For example, a module can author its frequency control as:

```cpp
auto frequency = g.input<Sample>({
    .default_value = Sample{220.f}, .min = Sample{0.f},
    .scrub_speed = Sample{2.f},
});
```

Ctrl-click starts a drag at ×0.25 sensitivity. The existing wheel interaction
still doubles or halves that modifier per step, without changing the current
value. A later ordinary drag starts at ×1 again. Keyboard steps also use the
declared speed rather than recalculating sensitivity from the current value.
Keep the modifier and the Ctrl-click shortcut discoverable in status information.

Sample Sequential and Coverage inputs can declare `enabled_by_default` for
an instance's outer input knobs. A module's internal input knobs start
disabled. An instance's saved setting takes precedence. `default_value` sets
the initial number and the double-click reset value. Double-click changes the
number and preserves the enabled state. See the [input knob rules](node_presentation_and_manual_controls_direction.md#input-override-rule).

### Folded inputs extend the existing lane header

The owner chose cropped sockets on 2026-11-19.
In editable-height headers, show a half socket at the bottom of each column
that has hidden ports. Cables for folded ports attach to the half socket
while those ports are out of view. Visible rows retain their own knobs,
sockets, names, and interactions. Their geometry stays fixed while opening
or closing the continuation.

The folded socket shares the knob center for its direction, regardless of
which ports it represents. Its bottom touches the lane header edge, including
on headers with a module footer. The footer remains clickable beside the socket.

Hover over the input or output controls to open that column's popover.
The gap between the columns closes the popover when the pointer enters it.
The popover stays open while the pointer moves into it or while a knob is in use.
Hidden rows use the same knob size, height, and spacing as the visible rows.
The popover can widen for port names and start below visible rows to keep their
controls clear. Each revealed port uses its own socket for cable endpoints.
Scroll the additional rows when they exceed the available viewport height.
During a cable drag, hover over compatible controls to reveal their column.
Shift retains the separate per-port connection chooser.
[Earlier shared popover](./historical/vue_port_popover_before_directional_hover_2026-11-21.md).
[Earlier header-bottom placement](./historical/vue_folded_ports_before_row_alignment_2026-11-19.md).

As the lane shrinks, step from full-size knobs to compact knobs and then to
folded port symbols. At mini and nano heights, use read-only port symbols or
stacks to show connection state and anchor cables. Event symbols stay rounded
squares and mixed stacks show both socket shapes. Port editing and popovers
are available at editable height.
[Earlier mini and nano popovers](./historical/vue_mini_port_popovers_before_read_only_2026-11-19.md).
[Earlier placement and interactions](./historical/vue_folded_port_layout_before_cropped_sockets_2026-11-19.md).

In `editable`, keep the port rows below the lane name and show meter scale
labels. At the transition to `mini`, place the port symbols beside the lane
name and hide the meter scale labels as automation becomes read-only.

Do **not** render the knob's numeric value permanently beside the input. The
existing VS Code status item remains the lightweight value/shortcut readout
during hover and manipulation. Explore a temporary knob popover, shown on
hover or drag, with a larger control and its value **above the knob**; this is
an interaction direction, not a requirement for a permanently open panel.
Respect the existing project-owned manual-control value, per-input fallback,
gesture history, and temporary override semantics. The UI merges appearances,
not input identities or backend state.

An authored knob may have a minimum, a maximum, both, or neither. Make **each
bound independently visible** so a min-only knob is distinguishable from a
max-only or unbounded knob. Keep the knob's **available orientation arc fixed**
regardless of which bounds exist. In particular, a min-only knob at its minimum
must line up visually with the left/minimum end of the drawn arc. The value
mapping may account for the bounds, but bounds must not silently change the
orientation endpoints or compress the displayed arc. Preserve useful
magnitude differentiation for unbounded controls.

Draw a short radial bar at each bounded end, using the same style as the
default-value marker. At each unbounded end, cut two short gaps into the track.
Keep the same end positions for every knob size.

When a knob declares a default value, use a dim fill from its default marker
to the current pointer. Follow the available arc in both the dial and its
preview. At the default angle, omit the fill.

### Hover-keyboard clipboard transfer for knob values

While a knob is hovered (or is the active/focused knob), **`c` copies** its
actual current numeric value as plain text to the **system clipboard**, and
**`v` pastes** the current system clipboard's plain-text numeric value into
that knob. The copied text is a round-trippable representation of the value,
not the knob's normalized `0`–`1` orientation or pixel angle. The same text
can be pasted from an external application, and copied values can be pasted
into any compatible knob. Validate type, finite-value rules, independently
optional bounds, and any numeric-entry restrictions with the ordinary exact
value-edit path; report invalid or unavailable clipboard content in the status
bar rather than silently normalizing, clamping, or changing the wrong knob.
Do not intercept plain `c`/`v` when a text field or other editor owns text
input. Preserve native OS shortcuts.
If webview clipboard access needs extension-host mediation, use the actual
host/system clipboard rather than a private in-memory copy buffer.

**Pasting is a complete knob interaction, not a direct project-field edit.**
Hover has already requested the pre-built manual *live/dynamic-input* GraphJit
variant. On `v`, activate that matching prepared live variant, apply the
pasted value to its live target as soon as it is active, then perform the
same gesture completion as moving the knob and releasing the mouse **while
still hovered**. The value becomes effective on the audio thread through the
existing live manual-control and interpolation path; publication/activation
must preserve normal graph-revision ordering. Commit **one undoable gesture**
for a changed value, then request/precompute the next constant-specialized
GraphJit variant for the pasted settled value. Keep the dynamic variant
available while hovered, just as after an ordinary release; permit the
settled variant's normal activation when the hover ends. A paste must not
write through the old constant-specialized executable or wait for the next
unrelated pointer gesture to use the value. If the matching live variant is
not ready, retain the requested target and make the transition once it can
be safely activated, with truthful pending status; never pretend the new
value is already audible before activation. Do not synthesize pointer
movement or an actual mouse grab to implement this lifecycle.

Apply equivalent interaction semantics to any compatible output gain knob
when that feature is implemented, preserving its unity specialization rule.
Copy-only is not a graph edit and must not create a history entry or a new
compilation request.

## 3. Output gain controls and GraphJit specialization

Add a gain knob to every output for which multiplication by a scalar gain is
semantically defined. Its backend authored/default value is **exactly `1`**,
with **no minimum or maximum** on the signed gain. The client presents a
magnitude knob with minimum **`0`**, no maximum, and default **`1`**. A separate
polarity toggle selects normal or inverted output. The client combines
magnitude and polarity into the existing signed backend value.

Place output names to the left of their port circles in direct rows and
folded rows. Show `+` for normal polarity and `−` for inverted polarity on
the toggle beside the gain knob. Keep `+` subdued and highlight `−`.

This UI contract is the owner's 2026-11-17 revision. The earlier signed-knob
presentation is preserved [historically](./historical/output_gain_ui_before_2026-11-17/vue_presentation_refinement_direction.md).
Dragging upward increases magnitude in either polarity. Numeric entry accepts
nonnegative magnitude. Reset restores magnitude `1` and preserves polarity.
Toggling polarity preserves magnitude and uses one ordinary control gesture
when the signed value changes. Undo and redo continue using the signed gain.

At exact zero, polarity has no distinct backend value. Keep that choice as a
presentation preference in VS Code webview state, so reducing an inverted gain
to zero and increasing it again preserves inversion. Changing polarity at
zero does not create a backend edit. A nonzero signed value remains authoritative
when loading or observing the project. No new RPC or project-file fields are
required.

Avoid claiming a numeric gain operation for event or
other output value types without a defined scaling meaning; the supported
value-type behavior must be explicit. Output gain is a port-owned control:
its effect applies consistently to the output value seen by its consumers,
not independently per outgoing wire. Neither output shape nor coverage is
changed by changing gain.

**At settled unity gain, the generated executable must be equivalent to a
graph with no output-gain operation:** no gain multiply, gain load, wrapper,
extra per-sample branch, or gain-specific runtime work in the resulting
GraphJit binary. Specialize `1` away. Only a non-unity value (or an active
interactive gain gesture) introduces necessary execution. Use the existing
ordered control-compilation/activation, immutable storage compatibility,
undoable gestures, and parameter publication model; do not insert an always-on
generic multiply merely to preserve one code path. Background evaluation
must likewise apply the selected gain consistently wherever that port's values
are consumed, without compromising version consistency. The exact handling of
non-numeric and event-valued outputs is an explicit implementation-design
question, not permission to multiply arbitrary payloads.

**Owner clarification, 2026-11-17:** the gain is a non-destructive transform
after the public output, including buffered outputs. It retroactively scales
previously stored contents when those contents are read. Retained source bytes
and node state are unchanged. This applies to all output shapes/realizations
with a defined scalar gain operation, not only Sequential/computed outputs.
Each consumer/evaluation uses a coherent selected gain snapshot; scaling must
not change Region identities, coverage or materialized-valid positions.

The owner explicitly selects `Sample` and `FFTBlock` as gain-capable types:
multiply each Sample, or each real/imaginary FFT-bin component, across all
channels. Event and `FractionalGlobalIndex` position outputs receive no gain
control. Additional value types require their own explicit scaling capability.

## 4. Connection-cable legibility and future subgraphs

Selection should **emphasize** the selected lanes' connections; it should not
be the sole condition for their visibility. Hiding all cables of unselected
lanes loses graph context, while drawing every cable at full opacity over
lane names makes the editor unreadable. Explore **transparency/opacity** and
appropriate foreground/background layering, especially where wires cross
labels and input names. Preserve port hit targets and established connection,
connection-preview, and collapsed-slot endpoint semantics. The exact opacity
and interaction treatment is not settled by this note.

Do not solve the present problem by requiring every wire at full opacity.
Project-authored subgraphs with explicit custom inputs/outputs are intended to
provide a scalable organizing mechanism. Connections targeting lanes hidden
inside such subgraphs should merge visually at the exposed subgraph boundary,
while retaining their underlying authored identities and unmergeable endpoints.
That future capability should inform cable presentation without blocking
near-term legibility improvements.

### Cable opacity near labels

Keep cables at a steady low opacity when neither endpoint belongs to the
selected lane. When either endpoint is selected, raise the cable opacity
away from text. Fade it back to the low opacity as it approaches a label.

### Port color variants (mapping still to be selected)

Explore a second hue for each port direction: **orange alongside the existing
yellow for inputs**, and **purple alongside the existing blue for outputs**.
Within a selected theme/palette, the RGB-space distance from yellow to orange
should match the RGB-space distance from blue to purple, so the two variants
carry comparable visual differentiation. Maintain contrast with the lane
background, cable opacity states, and subdued incompatible endpoints.
The semantic assignment is **not yet decided**: distinguishing event versus
continuous ports, or `computed` versus `buffered` outputs, are possibilities to
evaluate. Do not silently commit either interpretation in connection planning
or encode a new backend port distinction solely for the palette. Direction
remains clear and type/realization must be readable without depending on color
alone; carry the eventually selected scheme consistently into port circles,
knobs, cables and legend/status descriptions.

## 5. Automation editing and graph-authored snapping

The Tock interpreter computes samples from cyclic `CurvePointEvent` events.
It reads each point's outgoing `empty`, `hold`, or automatic `bezier` segment.
Point IDs stay stable when points move. The project stores the chosen order at equal indices.

- Left-drag a single point to move it with its curve.
- Hold Shift during a single-point drag to keep the point's original type
  and tension. Press or release Shift to change modes at the current sample.
- Drag an unselected point to move it without changing the selection. Drag
  a selected point to move the selected points together.
- **Left-drag empty space** to select the points inside the rectangle. Ctrl
  toggles those points and Shift adds them.
- **Shift+click a point** deletes it.
- **Right-click an existing point** opens the segment menu for its outgoing
  curve type, tension knob, and **Delete point**.
- **Right-press empty space** adds a point at the nearest sample and starts
  placing it immediately. Point hit testing takes precedence. The first point
  on a blank curve is empty. Shift keeps the new point's initial type and tension.
  The [previous gestures](historical/vue_lane_and_point_selection_before_2026-11-18.md)
  remain available for context.
- Provide a **tension handle near each applicable control point**, manipulable
  without entering a separate form. Display and edit the same `[0,1]` tension
  parameter used by the backend, keeping exact linear interpolation at `0` and
  the automatic clamped curve at `1`. Do not introduce manual Bézier handles.

Use knobs for numerical settings like tension that change continuously during
a drag, including in menus and popovers.

When adding a point in a gap, connect it to the nearer edge by sample distance
along the index circle. At equal distances, connect it to the following edge.
Keep the other side empty. Use the neighboring covered segment's type and
tension. Use Bézier with tension 1 if the edge has no covered neighbor.

When dragging an existing endpoint through a gap, keep it attached to its curve.
The point can touch another curve while keeping its place in the first curve.
When it crosses into the second curve, the previous neighbor becomes the end of
the first curve. The dragged point stays attached to the second curve if it
moves back into the gap. It rejoins the first curve when it crosses that edge.

A point within its original covered span keeps its type and tension. If it
moves into another covered span, it uses that span's type and tension. Keep
track of crossed edges while Shift is held. When Shift is released, use the
current attachment at the same sample.

In both single-point drag modes, each neighbor has a time detent. A detent
holds the point at the neighbor's sample while the pointer moves. In the
first half, the dragged point comes before the neighbor. In the second half,
the dragged point comes after it. If several neighbors share the sample,
the detent treats them as a group and preserves their relative order.

Point edits continue through typed node messages, draft reconciliation and one
undoable gesture per completed manipulation. Insertion publishes on press and
sends periodic updates as the pointer moves. Each placement update applies
the point and related segment edits together. The handle, menu, and drag
interactions must remain coherent across presentation remounts and accepted
backend revisions; no direct mutation of DSP storage from Vue.

[Previous point placement and drag modes](./historical/automation_point_drag_before_time_detents_2026-11-18.md).

### Value display and panning (2026-11-18)

The Scale panel selects Curved or Cropped display. Curved uses an arctangent
scale. Cropped uses a linear window and clips values outside it. Pan and zoom
apply to the lane view independently of the `min` and `max` inputs.
[Previous navigation rules](./historical/automation_navigation_before_independent_value_axis_2026-11-18.md).

The project saves each curve's display mode, value ruler, pan, and zoom in
`iv_project.jsonl`. Graph-wide pan and zoom stay in the current editor session.
[Previous storage choice](./historical/project_presentation_before_jsonl_2026-11-19.md).

A custom presentation can register `serialize` and `restore` callbacks with
`usePresentationState`. When the view changes, the presentation tells the host
that state is ready to save. The graph editor calls `serialize` and stores the
JSON object by node address and presentation ID. When the presentation opens,
the host passes that object to `restore`.

**Alt+Shift+middle-button drag** across an automation lane changes its value
zoom. Horizontal motion changes the scale around the value at the pointer's
height. Drag left to zoom in or right to zoom out, as with Alt+middle-button
drag on the graph. The timeline supplies the same zoom factor to every lane
editor that supports this gesture.

**Alt+Shift+scroll** over the lane also zooms its value axis around
the pointer's height. **Shift+middle-button double-click** resets both value
pan and zoom.
In Curved display, zoom-out widens the arctangent scale.
The value anchor continues in the same direction when the pointer wraps.
The status bar shows how many screens above or below the visible lane the
anchor has moved.

Horizontal value guides span the full automation content width, with labels
beside their strokes. Both Curved and Cropped emphasize zero and unit values
and grade other guides by multiples of the finest displayed value step.
Opacity uses the same pixel-spacing falloff as the beat grid. The Scale
button shares the ruler settings button's extended hit area.

While a dragged point is held at a value snap, draw a thin blue guide that is
brightest under the point and fades horizontally to a visible stroke at both
edges. Brighten the matching value label on the left. Clear both highlights
when the point leaves the snap or the drag ends.

When signals supply bounds, observe the public `min` and `max` inputs over
the visible window. Draw the bound traces, the authored curve, and the clamped
output preview on the same value axis. The preview uses representative bound
samples. Playback uses the exact bounds at each output sample.

### Automation signal inputs and optional transforms

`builtin.automation.curve` exposes `min` and `max` as Sample Coverage
inputs. The sampler interpolates the authored curve, then applies the highest
lower bound and the lowest upper bound at each output sample. If the bounds
cross, the sample equals their midpoint.
[Previous point clamping](./historical/automation_point_clamping_before_output_bounds_2026-11-18.md).

_Deferred._ Expose `scale` and `offset` as connectable inputs. Apply scale
and then offset to sampled values. In the absence of a contribution, use an
identity transform. Preserve gaps in the curve.

Allow an optional `NodeRef` constructor argument to supply a subgraph that
transforms the curve's authored point events before sampling. A second optional
`NodeRef` can supply a subgraph that processes sampled values. The presentation
can draw the original and transformed curves in the same lane. Implementation
must preserve the original curve and make both sampled signals observable.
The arguments must follow the
[builder ownership rules](./configuration_expression_language_direction.md#implemented-decisions-and-remaining-dependency).

An unconnected public Coverage input routes as empty. The compiler retains
the public route for window queries.
[Previous constructor bounds and routing work](./historical/automation_constructor_bounds_before_coverage_inputs_2026-11-18.md).

### Automation clip ruler and sample-accurate editing

Draw a **time ruler and its gradations in the automation clip's background**,
using the clip's existing horizontal time transform and the project's sample
rate. By default, the ruler shows **seconds and project sample positions** as
two views of the same timeline, not an unrelated seconds offset or an
independent tempo grid. Reuse `TimeRuler`/timeline grading geometry and styling
where possible, keeping clip marks aligned with the enclosing time ruler
through scroll, pan, resize, and zoom. Keep background labels and lines subtle
enough that the automation curve and points remain legible. Adjust mark/label
density to avoid overlap. When the zoom makes individual samples visually
separable, expose the **sample subdivision grid** in the clip background.

**Every authored control-point time is an integer project sample index.** The
UI may display seconds, but it must not treat a free-form floating-point
seconds offset as the authoritative edited time. Creation and every drag-draft
update round to the nearest sample position using the project sample rate;
quantization takes effect **while the pointer moves**, not only after release,
RPC acceptance, or a read-model refresh. The displayed point must follow that
same sample-aligned draft, so sufficiently zoomed-in drags visibly step between
sample marks with no release-time jump. At low zoom, sample precision remains
an invariant even though per-sample lines should be hidden when unreadable.
Preserve the exact `GlobalIndex`/sample-index identity and cyclic positioning;
do not rely on imprecise floating-point seconds to serialize large indices.

The sample grid is the **always-present minimum time quantization**. The
project-selected, graph-generated trigger/event grid specified below is an
additional, optional snapping source. Its chosen targets must ultimately be
sample positions too; disabling or lacking that source never permits
subsample-time point placement. Do not add an implicit BPM/beat grid or snap
the point only in the backend after the drag ends.

### Small, filled control-point markers

Render points as **small, filled dots on the curve**, not large hollow rings
that resemble holes or excluded intervals. Keep their existing forgiving
pointer hit area by separating the visible marker from an invisible/larger
hit target. Hover or active interaction may subtly enlarge or highlight the
dot, in the same restrained visual language as knob hover. Preserve
unambiguous Shift+click, drag, and context-menu targets. The
nearby tension handle remains distinct from the point and does not need to
make the point marker larger.

### Explicit value bounds and lane zoom categories

The lane header shows the connectable `min` and `max` knobs. Each knob has an
enable button. Both knobs start disabled. Their reset values are 0 and 1.
Set each knob independently. Enabling a knob uses its stored value. When the
effective `min` exceeds `max`, the output at that sample is their midpoint.
The project stores the manual input values and their enabled states.
[Previous linked knob limits](./historical/automation_knobs_before_independent_bounds_2026-11-19.md).

The Scale panel controls display and snapping.

Shift and middle-drag pans the value scale and moves the timeline horizontally.
The cursor wraps inside the visible lane content to the right of the header.
Shift and a double middle click restores the default mapping.

Bounds affect the output. Control points stay in the authored value domain
during creation and dragging. Point edits do not rescale the axis to fit the
points.
[Previous point clamping](./historical/automation_point_clamping_before_output_bounds_2026-11-18.md).

For a later UI pass, add a faint fill between the `min` and `max` traces.

Honor the lane's existing **`nano`**, **`mini`**, and **`editable`** vertical
zoom categories. Both `nano` and `mini` display the curve shape scaled to the
available lane height, including its exact gaps. Nano does not substitute a
presence-only fill: at small heights the curve naturally approaches a line.
An outgoing `empty` segment means no curve until the next appropriate point,
including the circular wrap and equal-index rules. Do not draw interpolated
values in those gaps. Both modes disable point/tension/segment editing.
This owner refinement supersedes the earlier
[coverage-only nano contract](./historical/vue_automation_nano_presence_before_curve_scaling_2026-11-16.md).
Only `editable` displays interactive points,
tension handles and the editing controls. Changing zoom level does not change
point data, bounds or selection identity. Sample-accurate time snapping remains
required for all edits in `editable`.

The project time ruler uses the editor-managed Timeline input as described
in [Timeline nodes and project snapping](./project_playback_and_presentation_grids_direction.md#timeline-nodes-and-project-snapping).
The automation editor draws the ruler's visible event marks as vertical guides
and snaps to those same marks. Use a screen-pixel radius when creating a point
and pointer-travel detents during a drag. The active guide uses the same blue
fade as value snapping. The beat source supplies timestamps in project samples,
and the point editor saves the chosen sample index.
[Earlier radius-only drag snapping](./historical/automation_beat_snap_before_detents_2026-11-19.md).

## 6. Contextual information: status bar before tooltips

Ordinary controls, ports, automation points and meters should not fill the UI
with native `title` tooltips or repeated hover balloons merely to state a
name, current value, sample index, shortcut or connection fact. Such immediate
context belongs in the existing VS Code system status item/status bar while
hovered, focused or manipulated; it should be updated and cleared with that
context. Keep the screen visually quiet by default. A deliberate detailed
popover or help action may still expose genuinely additional information, and
an error may warrant a focused diagnostic, but neither should recreate a
default tooltip on every interactive object.

Retain semantic names, screen-reader labels and accessible descriptions.
Follow the [webview focus policy](./node_presentation_and_manual_controls_direction.md#webview-focus-and-keyboard-shortcuts).
Keep the status item consistent with the pointer and active gesture, especially
while moving sample-quantized control points or adjusting knob values.

The user setting `intravenous.hideStatusBarShortcuts` hides shortcut reminders
in the status bar. Values, port details, active gesture readouts, and errors
remain visible. Shortcut reminders are visible by default.

## 7. Scope and sequencing

Acceptance checks must also cover stable nested-row knob coordinates with
the header continuation open/closed, including a knob on the folding anchor;
copying a raw value to the OS clipboard, pasting a value from another app,
and observing the same live-graph activation and settled specialization as a
completed drag; rejected clipboard content must not modify the graph. Test pan
and zoom with either bound, both bounds, and neither bound. Test scaled read-only
curve paths in `nano` and `mini`, and editable points and handles in `editable`.
For Timeline inputs, test multiple nodes inside different subgraphs,
shared timestamps at different depths, and unavailable windows.
Check that hiding marks at a coarser zoom also removes their snap targets,
and that adding or dragging a point commits the displayed sample index.

Acceptance checks for the coherent UI implementation should verify that a
sample-aligned draft visibly changes at a one-sample pointer movement when
zoom permits, never jumps to a different sample only on release, remains
aligned across viewport zoom/pan, and preserves exact authored sample indices
through message acceptance. Check point marker appearance and hit targeting
separately, including keyboard access and Shift+click deletion. Verify the
meter fills the flexible lane remainder with both stereo channels visible,
and that ordinary hover feedback reaches the status item without default
native tooltip clutter.

These requests are accepted design intent, **not evidence that the current
client or GraphJit implements them**. The near-term task is to finish and
verify the Stage 4 observation path, including correct stereo capability,
payload and metering. Then address the related lane presentation issues, with
full-width meters and quiet status-bar feedback as part of near-term polish.
The automation
clip ruler, sample-aligned drag updates and smaller point markers are basic
curve-editor correctness/usability work; do not block them on the later
project-selected graph-event snapping query. Integrate that optional grid as
a coherent subsequent observation/editing feature. Output gain
requires a separate whole-path control/GraphJit implementation with explicit
unity-codegen verification. Full cable/subgraph presentation is longer-term
work; opacity exploration is a near-term readability investigation. Input-value
observation, arbitrary new DSP event conversions, and Stage 5 background-window
observation do not become prerequisites merely because this document records
these UI improvements.
