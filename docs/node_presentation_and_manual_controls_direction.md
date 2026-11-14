# Node Presentation And Manual Controls Direction

> **Communication model:** backend-node typed events, coherent semantic
> synchronization, Vue presentation bindings, reflected runtime observations,
> and batched live-subscription constraints are consolidated in
> [Node Interaction And Presentation Communication Direction](./node_interaction_and_presentation_communication_direction.md).
> This document remains the home for display modes, interaction lifetime,
> persistent manual controls, and control/JIT-specialization direction.

_Status: normative Front 14 direction. Front 13 supplies stable `NodeAddress` and
selector semantics. Front 6 supplies revision continuity. Front 12 can improve later
scoped construction, but it does not block the initial Front 14 implementation._

The superseded JIT scheduling and dependency text is preserved in
[historical documentation](./historical/front_14_manual_control_jit_policy_before_demand_activation_2026-11-04.md).

This document consolidates the planned node-presentation, interaction-lifetime,
manual-control, and control-specialization model.

## Frontend-owned presentation selection

A node can have multiple Vue presentations, and one Vue presentation can represent
multiple node definition types. There is no backend `NodePresentation` extension
family and no `IV_NODE_PRESENTATION` registration in the intended design. Package
finalization, `PackageRevision`, the package catalog, and socket RPC should not
retain or publish frontend-provider compatibility records. The currently landed
registration/registry implementation is superseded, not a compatibility surface to
maintain indefinitely. The old contract is preserved in
[historical documentation](./historical/node_presentation_and_live_observation_before_vue_direction_2026-11-12.md).

The frontend owns the selection of a registered Vue component, including the choice
of a specialized component by stable leaf or module definition ID, a generic
component usable for any node, and a user's preferred component when several are
available. It must always have a usable generic fallback. The root node uses the
same presentation host rules as other nodes; it is not a distinct backend provider
kind. Backend state, GraphJit, and `IV_NODE` / `IV_MODULE` definitions neither name
Vue components nor depend on presentation selection.

### Initial Vue host, component distribution, and preferences

The first replacement Vue client runs inside the existing `vscode/` extension's
webview. Retain the extension host's useful project/session discovery, server
connection, and VS Code integration; replace the old webview presentation/UI
implementation rather than preserving its renderer as a second frontend. Keep
the Vue application's application-facing interface independent of VS Code-only
APIs where practical, so a standalone host can be added later without changing
node presentation semantics. A standalone browser client is **not** a prerequisite
for the first Vue slice.

Initially, Vue components ship with the frontend and register compatibility
by stable leaf or module definition IDs in a **frontend-owned registry**. A
single component may support many IDs; a node may have several candidates;
missing or incompatible specialized components fall back to the generic
presentation. Package-finalization metadata, C++ `IV_NODE`/`IV_MODULE`
declarations, and backend socket records do not name or distribute Vue code.
Third-party package-associated Vue bundles, their discovery, trust/isolation,
hot-reload/versioning, and the binding-generation workflow for rebuilt packages
are future integration questions, **not** reasons to restore backend presentation
registration or require package authors to modify C++ node definitions.

#### The graph editor is opened on one node

_Settled by the repository owner on 2026-11-12._

The Vue client is a graph editor opened on one node, the editor's **root**. VS
Code shows each such editor as an editor tab. Opening a node as a root never
changes the root of an existing editor; it opens a new editor. An editor's
initial root is the project's root graph, whose children are the project
instances.

**Primary presentation clarification (2026-11-12).** The primary project UI
uses a timeline-like presentation. The current free-position node canvas is an
implementation checkpoint, not the intended primary presentation. The repository
owner will specify the timeline interactions; implementing a graph scope read
or a presentation host does not settle those interactions.

The owner confirmed that a free-position graph editor can later be offered as
an alternative presentation for graph/subgraph nodes, including the project
root. It does not replace the primary timeline presentation.

**Confirmed child layout (2026-11-12).** The primary presentation places child
nodes vertically, one per horizontal row, under a shared horizontal time axis.
Each row allocates a display region to its child's presentation. This confirms
the layout; it does not assign new mouse or keyboard interactions for zoom,
clip editing, or connections.

### Lanes and reusable timeline display

The owner calls each such row a **lane**. The default lane presents any node
whose stable definition ID has no specialized presentation. Specialized
presentations may use the same lane frame or provide a different layout.

Vertical zoom stays fixed when the viewport is resized or the visible node list
changes. If the lanes do not fit, show a vertical scrollbar. Do not change the
lane heights to fit every node on screen.

The owner specified these viewport interactions on 2026-11-12:

| Input | Action |
| --- | --- |
| Scroll | Vertical scroll |
| Alt + scroll | Vertical zoom |
| Shift + scroll | Horizontal scroll |
| Ctrl + scroll | Horizontal zoom |
| Hold the middle mouse button and move | Simultaneous horizontal and vertical scroll |
| Quick middle click | No special action; never enter a persistent movement mode |

Middle-button movement ends on release or cancellation. Viewport navigation
does not modify graph nodes or port data. Zoom preserves the position under the
pointer where the scroll bounds permit it.

A timeline is a reusable client display feature that a presentation can use
when appropriate. It is not required for every node. The default lane must
handle arbitrary input and output port counts; it must not assume that a node
has one output that can stand for the whole node as a single meter, sample
buffer, or event buffer. A shared time ruler in the enclosing presentation
does not imply that every child renders time-dependent data.

Ruler labels keep the same numeric spelling across horizontal zoom levels.
For example, the mark at one second stays `1 s`; finer tick spacing must not
change it to `1.0 s`. Fractional positions still receive fractional labels.

The playhead displays backend playback position independently of viewport
navigation. Samples and seconds display the same position. The playback
contract and the separate proposal for grids from node outputs are in
[Project Playback And Presentation Grids](./project_playback_and_presentation_grids_direction.md).
Neither requires every lane to display a timeline or every project to define
a tempo map.

Lane names should be readable. Do not display UUIDs as ordinary lane text.
Keep full semantic identity and diagnostic information available through a
small copy-debug-details button in the left area. Use a compact visual failure
indication, such as a red name, outline, or dot, instead of adding a status line
that changes the lane's layout. A pending realization is not by itself evidence
of failure.

The default lane is one node presentation, not the old per-port panel nested
inside another lane system. Port controls belong in a compact port area or its
temporary expansion. This does not merge port identities or require a single
output visualization for the node.

### Proposed port area and temporary expansion

The owner clarified the appearance on 2026-11-13. Inputs use yellow circles
and outputs use blue circles, with the same theme colors as the side panel.
A grouped slot has no visible arrow/count label. Its panel extends from the
slot on hover, over the lane's display area. It does not require a permanent
lane-height increase or a separate action button.

Creation, configuration, deletion, package creation, source navigation, and save
are extension actions available through the Command Palette or user shortcuts.
Do not add a permanent toolbar or lane edit button for those actions. The
previously requested copy-debug button remains separate from these actions.

The owner settled the grouping rule on 2026-11-13. Input and output controls
always occupy separate locations. For each side, one port has a direct handle;
more than one port uses a grouped slot. A side with no ports has no handle.
Hovering the grouped slot immediately exposes all ports on that side in a
temporary panel over the display area. This does not require Shift, increase
lane height, or depend on how many icons could fit beside one another.

The first port circle occupies the grouped icon's position, using the same
first-item alignment as a connection chooser. The list has no decorative
header or connection summaries beneath its ports. It contains only the port
rows. Near the bottom of the viewport, the remaining rows grow upward while
the first port remains at the opening control. Port details remain available
in tooltips. The preceding layout is retained in
[Vue Port Lists Before First-Item Alignment](./historical/vue_port_lists_before_first_item_alignment_2026-11-13.md).

`NodePorts` renders these temporary lists as `.port-panel` elements. In UI
discussion, a **popover** means a temporary panel above the existing display:
the ordinary list is a port popover, and the connection chooser is a connection
popover. Inputs and outputs remain separate in either form.

During pointer interaction, a popover stays open while the pointer is over
that popover or the port group that opened it. Leaving both closes it. Empty
space between the input and output groups is outside both groups. A layout
transition must not discard the final check of where the pointer ended up.

When a row press starts a connection gesture, close the open popovers before
release. Keep the captured drag active so the pointer can reach ports that
were covered by the source popover. Do not reopen the starting port group
until the pointer leaves it. Compatible destination groups can still open on
hover; leaving and returning also permits another port in the starting group
to be selected when moving an existing connection end. Graph mutations still
occur only on release, and a quick click alone makes no connection change.

During a connection drag, a destination popover opens only over its own port
group, when that group contains a candidate target. Starting at an output and
passing over another output must not open that node's inputs. Hover its input
group to open them. Starting a new connection at an input uses the reverse
rule. Moving an existing connection end uses the direction of the end being
moved. Grouping changes presentation only: each port retains its identity,
and there is no limit on the underlying port count.

Only ports on the destination side rejected by the current drop checks are
muted during the drag, using a blend of their normal colour and the theme's
disabled grey. Candidate ports and ports on the other side retain their normal
colour. For example, dragging a new connection from an output leaves other
outputs blue; incompatible inputs are muted. A destination-side grouped icon
is muted when none of its ports is a candidate. Filled and dashed connection states remain visible through
the colour change. This feedback uses the existing direction/kind and
reattachment checks; it does not add converter discovery. The preceding ring
and automatic opposite-side expansion are retained in
[Vue Port Targets Before Muted Unavailable Ports](./historical/vue_port_targets_before_muted_unavailable_ports_2026-11-13.md).
The preceding broader muting rule is retained in
[Vue Port Muting Before Destination-Side Feedback](./historical/vue_port_muting_before_destination_side_only_2026-11-13.md).
The earlier fit-based proposal is retained in
[Vue Ports Before Single-Connection Choosers](./historical/vue_ports_before_single_connection_choosers_2026-11-13.md).

### Lane selection and connection display

The owner requested this display on 2026-11-13. Selecting a lane displays its
incoming and outgoing connections. Hovering another lane's grouped port slot
does not transfer selection to that lane.

If a connected port is hidden inside a grouped slot, draw that wire endpoint
at the slot. Connections with the same visible endpoints share one drawn wire.
When the slot opens, separate the wire into branches to its exposed connected
ports. Closing the slot combines those branches again. Distinct visible ports
of the selected lane retain distinct wires to the same collapsed destination.

This changes the presentation only. It preserves each port and connection's
semantic identity and does not create a new graph connection or fan-in operation.
Selection and hovering do not request graph compilation or an undo entry.
The wires must not intercept port dragging or hover expansion.

The owner added two display rules on 2026-11-13. A grouped input or output
slot uses its two circles to show how many of its ports have connections:
both empty means none; only the right circle filled means some; both filled
means every port in the group has at least one connection. These states count
desired connections; dashed outlines continue to identify pending connections.
The grouped icon is replaced by the first port while the list is open.

All wires use one cubic Bezier rule. For endpoints `(x1, y1)` and `(x2, y2)`,
both control points use `middle_x = (x1 + x2) / 2`: the first is
`(middle_x, y1)` and the second is `(middle_x, y2)`. This applies to established
connections, self-connections, and drag previews. Aligned ports can appear
straight. Node identity and other cables do not change the path.

Cable endpoints attach to the outside edge of each port circle, halfway up:
the right edge for inputs and the left edge for outputs. This applies to
established connections, the fixed end of the cursor cable, and both ends of
the faint destination preview. A collapsed two-circle icon uses its rightmost
circle for inputs and its leftmost circle for outputs. Opening a popover uses
the same edge rule on each exposed port or connection-entry circle.

The preceding self-connection routing is preserved in
[Vue Wires Before The Single Cubic Rule](./historical/vue_wires_before_single_cubic_rule_2026-11-13.md).

### Disconnection and endpoint movement

The owner accepted these gestures on 2026-11-13. The earlier proposal is
preserved in [Connection Gestures Before The Shift-Only Chooser](./historical/vue_connection_gestures_before_shift_only_chooser_2026-11-13.md).
A left click below means a primary press-drag-release gesture. A quick click
alone does not commit a connection edit.

A normal primary drag starts a new connection, including when the port already
has connections. Shift-hover over any connected port opens a chooser, including
when the port has exactly one connection. Pressing Shift while already hovering
the port also opens it. The first connection circle occupies the hovered port
circle's position; it replaces the chooser's decorative icon. Its entry can
therefore be picked up without moving down the list. Other entries remain
available for explicit selection. Near the bottom of the viewport, the remaining
entries grow upward with the first connection still at the hovered port.

The fixed end of a new connection's preview starts at the port circle, even
when the press is on another part of its row. If that port's list closes, the
preview follows its grouped slot. The mouse press position determines the
drag threshold, not the cable's starting point.

The preview uses the same Bezier rule as a finished connection. Its fixed
end keeps that port's color: yellow for an input and blue for an output. The
cursor-end color depends only on mouse position relative to the closest port
row's measured input and output columns. It blends continuously from yellow
to blue across those columns and clamps to those colors outside them.
Hovering a port or popover does not override the positional color, and color
does not affect the curve's shape. These rules also apply when moving an
existing connection end. Preview color does not change connection compatibility
or commit a connection before release. The owner retained the fixed-to-cursor
gradient when clarifying these rules on 2026-11-13.

Hovering an eligible destination port also shows a faint preview of the completed
connection, attached to that port's circle. The cursor cable remains visible
and keeps following the pointer; its position-based colour rule stays unchanged.
The attached preview uses the actual endpoint colours and the same cubic as the
completed connection. The destination popover stays open while hovered. Moving
away removes the attached preview. Both previews end on release or cancellation;
only a valid release can edit the graph. The editor's existing eligibility checks
still apply; this display does not add converter discovery.

Drag a chooser entry with the primary button to move that connection end.
Direct Shift-drag can still pick up a sole connection. Shift-click remains a
fallback, not a required opening click. Opening the chooser does not select a
connection or change the graph. Right-click receives no new menu.
The connection chooser stays open only while Shift is held. Releasing Shift
hides it. An ordinary grouped-port list remains governed by its normal hover
rules. Once a chooser entry has been picked up, releasing Shift does not
cancel that drag.

While the chooser is open, each visible entry receives its connection's wire
at its own port circle, like the individual ports in an expanded grouped slot.
These attachments use the connection ID and port identity, not display names.
Opening a chooser shows the connections of that port even if its lane is not
selected, and does not change lane selection. Other ends of a multi-ended
record are not revealed unless they connect to that port or the selected lane.

The other end stays fixed. For `A.out → B.in`, moving the `B.in` end to `C.in`
produces `A.out → C.in`. Moving the `A.out` end to `D.out` produces
`D.out → B.in`. Sample and event ports follow the same gesture rules.

The graph stays unchanged during the drag. Release in empty editor space
removes the chosen end. Release on a compatible port of the same direction
moves it there. Release on the original port does nothing. Release on an
incompatible port, a grouped slot without a chosen individual port, another
control, or outside the editor cancels. Escape, lost pointer capture,
pointer cancellation, or window blur also cancels.

The editor operates on the selected connection record and port, not an
aggregate drawn wire. Other connections and endpoints remain unchanged.
For a record connecting `A.out` to both `B.in` and `C.in`, removing the `B.in`
end retains `A.out → C.in`. Removing the last end on one side deletes the
record. A selector that matches several visible ports is expanded into exact
port matchers when one of those ports moves; its other matched ports and
channel selections remain in order. Unrelated matchers remain unchanged.

An endpoint move preserves its connection ID and uses one
`graphConnections.upsert`. It must not submit a deletion followed by a separate
upsert and expose an intermediate disconnected generation. The editor checks
the current record before submission and rejects a stale gesture. Any future
edit that needs several record changes must use one coordinated project action.
The completed gesture is one future undo entry.

Each connection record has one `sourceType` and one `targetType`. Moving a
whole-port end can update its side's type when it is the only end on that
side. Ends that share a side must keep its type. Moving an end with a selected
channel preserves that channel selection and requires the same port type.
Type conversion between the output and input sides remains a backend decision.

### Subgraph presentation layout

The root is displayed by the first compatible presentation for its definition,
by the same selection rules as any other node. A subgraph's presentation
displays its children and gives each one the display region its zoom level
implies; each child's presentation then receives the display mode for that
region. The children of a project-local module instance come from authored C++,
so inside a module instance the editor shows structure it cannot edit; project
instances and project connections are the editable graph.

**Project-authored groups (planned; requested by the repository owner on
2026-11-12, not yet scheduled).** A project can nest its own nodes into
groups: subgraphs defined in the project, not hard-coded in a C++ module node.
A group defines its own inputs and outputs, like a module node or subgraph. It
can be expanded, to manage its contents, or collapsed, to be used as a node
itself, so a large project stays tidy while it is maintained. A group is
therefore one more editable scope of the graph editor; its durable record,
addressing, and realization are to be designed when the feature is scheduled.
Groups organize project structure. Filtering remains a presentation feature:
source selection can dynamically change the visible lanes without changing the
project graph. The prior prohibition is preserved in
[Lane Layout Before Fixed Vertical Zoom And Source Selection](./historical/vue_lane_layout_before_fixed_zoom_and_selection_2026-11-12.md).

The intended side-panel replacement is another host of the same lane UI, filtered
by source selection. It should share presentation and interaction state with
editor webviews through the client session, rather than maintain a second lane
implementation or create a separate graph. This is planned work; the existing
source-inspection panel has not yet been replaced.

Node positions are frontend-owned presentation layout, and they **survive a
reload**: the frontend persists them in the project directory, keyed by stable
node addresses, so a project's organization stays with the project. The
backend neither reads nor validates them, and they are not part of
`iv_project.jsonl`.

For the initial client, user-selected preferred presentations are frontend-owned
workspace preferences, not canonical `ProjectPersistence` graph data. Store a
preference only for a stable frontend component identity and applicable node
identity/definition; recover with the generic fallback when the component is
unavailable. The backend must not interpret a Vue component name. Later
project-sharing or per-user preference semantics can be designed when needed.

The generic presentation discovers all available ports, including their resolved
identities, types, channel structure, access semantics, and connection state. It
can choose which ports to visualize at a particular scale, but **all ports must
remain discoverable and connectable** in graph-editing contexts. A generic
presentation may also generate controls from Front 9 configuration metadata and
provide direct source editing when needed. Specialized Vue presentations use the
same configuration/typed-value operations; users do not have to edit expression
source for every construction-argument change. See
[Configuration Expression Language Direction](./configuration_expression_language_direction.md).

Presentation selection is independent of observation capabilities. A backend node
need not be altered to support a new Vue presentation of its reflected state or
ports. The observation mechanism and its limits are specified in
[Vue Output Observation And Query Direction](./vue_output_observation_direction.md).
Input *connection metadata* belongs in every graph presentation, but input
*value* observation is postponed. The generic presentation initially observes
outputs only, and supported reflected Tick state may be requested by custom
presentations. Tock state observation remains out of scope.

## Four global display modes

A presentation receives exactly one of four global display modes:

| Mode | Contract |
| --- | --- |
| `compact` | Width is below the globally guaranteed normal-presentation width. |
| `nano` | Read-only temporal/activity-oriented presentation. |
| `mini` | Read-only structured/spatial presentation. |
| `editable` | Full editing model from the editing threshold through fullscreen. |

`compact` supersedes the normal height/profile modes at narrow width. It does not
combine independently with `nano`, `mini`, and `editable`; there are four modes,
not a width x height Cartesian product.

There is no required left-anchor rule. Presentation layout is responsible for
responding to the supplied mode and available host geometry.

The host chooses the effective mode from the available width/height, available
presentation capabilities, and any supported user view choice. It supplies one
mode, not two orthogonal mode flags. The precise size thresholds and switching
hysteresis are Vue layout/tuning decisions, to be fixed against real controls
rather than as a backend protocol invariant. The component should not infer
backend observation interests solely from the existence of a mode: it declares
the operations its rendered state actually needs.

## Presentation interaction state outlives Vue components

An active gesture must not be owned by the Vue component that happened to
render the control when the gesture began.

A longer-lived presentation interaction context retains at least:

```text
stable authored node identity
stable input/control identity
pointer/scrub interaction state
latest desired manual value
required `dynamic_inputs` membership, when applicable
```

Rendering components bind to that context. Therefore all of the following may
occur without cancelling an active scrub merely because a component remounted:

- resizing a presentation;
- crossing a display-mode threshold;
- selecting another presentation profile;
- hiding/replacing the concrete knob widget;
- rerendering/remounting Vue components;
- publishing a new project realization that preserves the semantic target.

When a new project revision publishes, both ordinary presentation hosts and
active interaction contexts re-resolve their stable semantic identities against
the new realization.

If the target no longer exists or is no longer compatible, the interaction ends
through an explicit invalidation path rather than through incidental component
lifetime.

For socket RPC clients, the server assigns the interaction's client identity from
the authenticated connection. A request cannot claim another client's identity.
Disconnect removes that client's hover and active-interaction state. The durable
manual value remains the last value accepted before disconnect.

## Source navigation uses `NodeAddress` and its selector path

C++ source annotations resolve presentation and source focus to the semantic pair:

```text
NodeAddress::instance_id
+
NodeAddress::path
```

Together, these fields form the stable `NodeAddress` supplied by Front 13. Presentation
code does not retain one generation-local `NodeBundleHandle` or concrete member index.

`NodeAddress::path` contains `ProjectNodePathSelector` values. Each value is a
`ProjectVirtualNodeSelector`, `ProjectVirtualMemberSelector`, or
`ProjectTiledChildSelector`. The sidebar can open the authored module instance and
focus its generic subgraph presentation on that semantic target.

If the virtual node currently has several concrete members, the presentation may
show those members as rows/children. If a later realization changes the
member count, the source-level focus may survive because it addresses the virtual
identity rather than one concrete index. If the virtual node disappears, the
enclosing module presentation remains valid and only the focused child target is
lost.

Front 12 can add same-session scoped construction and `SubgraphClosure`. It must
preserve these semantic selector paths. Front 14 does not wait for Front 12.

## Reconstruction and state continuity

Changing construction/configuration arguments produces a new graph realization
while retaining the authored project node instance identity:

```text
same authored node identity
new construction inputs
new concrete realization
```

That stable identity means "candidate continuation", not "reinterpret arbitrary
old state as the new type".

Leaf state migration remains subject to compatible generated/native state
identity and migration rules. Module state reconciliation recursively uses its
stable virtual/concrete child identities. Node-owned input-history and
output-history/latency semantics remain governed by GraphJit/executor state
reconciliation, not by presentation ownership.

Presentations simply rebind to the newly published compatible realization.

## Persistent manual controls

An eligible sequential input may have persistent project-owned manual-control
state.

The state contains at least a stored manual value plus the input's manual
participation policy. The intended behavior is:

```text
eligible sequential input
    has persistent manual-control value

no external source + manual mode enabled
    manual value participates

external source present + disconnected-only mode
    manual value remains visible/editable/persisted
    manual source does not participate

external source present + participate-with-connections mode
    manual source participates according to the input's defined
    composition/fan-in semantics
```

Connecting an external source does not erase the manual value. Disconnecting it
does not recreate a default from scratch. The stored value survives ordinary
connection edits and belongs in durable project state/JSONL once that persistence
surface is implemented.

The live scalar may reside in `NodeStorage`; one scalar per eligible/manual control
is not a reason to create a separate runtime storage architecture.

### Temporary connected-input override

_Settled by the repository owner on 2026-11-14._

Only Sequential inputs receive tweakable manual controls. During a live
interaction, the manual value replaces the selected input's incoming signal,
including when its settled participation policy is disconnected-only. The knob
retains its independent manual value; it does not have to follow the connected
signal. Releasing the mouse closes the undo gesture. While that client's pointer
remains over the knob, retain the dynamic code and temporary override. Leaving
the knob requests settled code immediately; the authored connection resumes when
that replacement activates. Neither the authored wiring nor the durable
participation policy changes. Hover alone prepares code and never activates an
override. This hover-leave policy was accepted on 2026-11-14; the
[earlier settling policy](./historical/manual_control_debounce_before_hover_settling_2026-11-14.md)
is preserved for provenance.

Represent the override as an internal Tick/ephemeral producer at the selected
public input. Preserve module-internal conversion, fanout, and authored temporal
cuts. Input history records the values actually consumed, including manual
values that remain in the history window after the gesture ends. A recording
node can explicitly persist the gestures when desired.

Reuse existing input-owned storage where possible. Shared producer storage
cannot be overwritten to override only one consumer. Compatible code variants
keep their existing `NodeStorage`; a variant that changes storage requirements
uses an ordinary accepted successor with node-storage and semantic port-state
migration. The owner accepts this initial migration cost. Measure real project
costs before introducing shared page-backed node state or another allocation
scheme; do not reserve every possible override's history permanently.

### Knob gestures and discovery

_Settled by the repository owner on 2026-11-14._

The default presentation places one radial knob beside each eligible input
that fits directly on the lane. No hover is needed to reach that knob. A hidden
input exposes its knob through the folded port popover, or once in its connection
popover. Do not duplicate a directly visible knob in the connection popover.
Keep the lane's wiring interface compact and avoid a second list of input
identities in its content area. The owner accepted default inline knobs on
2026-11-14. The
[previous popover-only contract](./historical/inline_lane_knobs_before_default_display_2026-11-14.md)
is retained for provenance.

_Accepted clarification, 2026-11-14:_ use the available height of the lane's
left area to show ports directly. Inputs and outputs keep separate columns.
Each column retains reflected definition order. If every port fits, show all
of them. Otherwise, reserve one handle for a popover that contains the remaining
ports. Compute each direction independently. Hidden-port connection indicators
summarize only that remainder. Existing connections and drag previews attach to
the direct handle when visible, or to the folded handle when hidden.
Fitting must account for the complete row height, including its knob. Knobs
and wire handles keep separate drag targets. In narrow lane labels, keep those
targets reachable even when names or numeric readouts need to be hidden.

Place the control between its port circle and displayed name. Keep the knob
and wire handle as separate pointer targets. Show the logical value prominently;
when finite minimum and maximum bounds exist, also show the normalized 0–1
value with less emphasis.

_Accepted visual feedback, 2026-11-14:_ distinguish idle, hovered, and dragged
knobs with subtle stroke and fill changes in the knob's own color. Avoid a blue
outline around yellow knobs. Drag feedback follows the local pointer gesture,
independent of server acknowledgement. Keyboard focus remains visible in the
same color family.

Drag up to increase the value and down to decrease it. Lock and hide the mouse
pointer during a drag, using relative movement so screen edges do not limit it.
Release or Escape unlocks it; leaving the application also ends the drag. Each
wheel event with nonzero vertical movement halves sensitivity when scrolling
down and doubles it when scrolling up, independent of the event's delta magnitude.
Use horizontal movement only when the wheel event has no vertical movement.
Changing sensitivity must not change the current value or reset the pointer
origin. Wheel events used by a knob must not scroll or zoom the timeline.

The exact authored default receives additional mouse travel. Preserve every
nearby numeric value; do not clamp a numeric interval to the default. The
extra travel has a fixed width in pointer pixels, independent of sensitivity.
Changing sensitivity inside that region preserves progress through it.

Right-click opens a numeric-value popover. Typing remains local until Enter
accepts one exact-value gesture. Escape, clicking outside, or leaving the
editor cancels the draft. Numeric entry bypasses drag snapping. Double-clicking
a knob resets it to the authored default. A drag must not count as the first
click of a reset. Arrow keys still adjust a focused knob. The owner replaced
Hover + R with double-click on 2026-11-14. The
[earlier reset contract](./historical/knob_reset_before_double_click_2026-11-14.md)
is retained for provenance.

A compact VS Code status item reports the hovered knob's shortcuts and the
active drag's value and sensitivity; its tooltip holds fuller help. Clear it
when the relevant context or focus is lost. Hover never steals keyboard focus
from another editor. Gesture and numeric-entry ownership belongs to the webview
root, so remounting a port widget does not silently end its interaction.

### Manual-control history is grouped by completed interaction (planned)

The durable `ProjectManualControl` value is eligible for project undo, but
scrub updates, hover, interpolation, and control-specialization requests are
not each separate undo steps. The long-lived interaction context groups one
user gesture: retain the original accepted stored value and, when the gesture
settles, record one reversible change to the final accepted value. A no-op
should create no history entry. The interaction context already outlives an
individual Vue control component and must remain the grouping authority
through remounts and presentation-mode changes. On disconnect, the last
accepted durable value remains authoritative; any unfinished history grouping
must be resolved consistently rather than inventing an audio-thread rollback.
See [Project Undo, Redo, And Backup Direction](./project_undo_redo_and_backups_direction.md).

## Manual values are Tick/ephemeral sources

A participating manual control is semantically a live Tick/ephemeral source,
analogous to another live real-time input.

Therefore it can satisfy ordinary Sequential consumption, but it does not by
itself satisfy a Random Access requirement. Random Access behavior follows the
normal graph capability/retention rules; do not add a manual-control-specific
exception that makes an ephemeral live scalar retrospectively addressable over
arbitrary positions.

This keeps manual controls within the existing port/access model instead of
creating a second control-only execution semantics.

## Interpolation exists only during active scrubbing

A settled manual value is eligible for exact constant specialization. The stored
manual value is the latest value retained by the project and UI. The effective manual
value is the value used by the active executable graph. These values can differ while
a replacement graph compiles.

Hover requests compilation of dynamic-input code. Hover does not activate that code.
If scrubbing starts first, the UI updates the stored value while the active graph keeps
using its existing constant. No generic execution path applies the new value to code
that does not support a dynamic input.

The intended lifecycle is:

```text
settled:
    active graph uses the exact constant value

hover:
    request dynamic-input compilation
    keep the active graph unchanged

scrub begins before dynamic code is active:
    retain each latest desired value
    keep the active graph's constant effective value

dynamic graph becomes active during the scrub:
    make the latest desired value effective
    begin audio-thread interpolation from the prior effective value

for each audio callback while dynamic scrubbing is active:
    latch the newest UI target
    interpolate sample-by-sample from the previous effective value
    to the latched target over the callback

mouse releases while still over the knob:
    close the undo gesture
    keep the dynamic graph and latest target active

pointer leaves the knob after release:
    capture the latest stored value
    immediately request a settled replacement (no debounce timer)

settled graph becomes active:
    use the manual value as an exact constant where it participates
    otherwise restore the authored connection
```

If the pointer already left before release, release requests settled code. Client
disconnect clears its retained controls too. Another client's hover cannot retain
an override after the originating client leaves. Keyboard and exact-value gestures
use the same hover lifetime. Regrabbing an unchanged dynamic variant reuses its code;
changes to other settled constants still require a matching replacement.

The UI-to-audio target scalar requires defined C++ concurrency. The audio thread can
observe the latest coherent target that is available. A relaxed atomic is sufficient
for that scalar. The audio thread owns the prior value, effective value, and
interpolation state. A target update does not require an RPC or queue handshake.

## Predictive compilation and demand-driven activation

Settled controls are candidates for constant specialization under GraphJit. Hovering
over a participating control requests code whose `dynamic_inputs` set contains that
input. Hover alone does not activate the compiled code.

Front 14 initially keeps one active executable graph and one logically current
replacement request. Compiled code can overlap temporarily during safe activation and
reclamation. Front 14 does not keep a cache of old control specializations.

The required `dynamic_inputs` set contains every control with an ongoing interaction
or a released gesture retained by its own client's hover. With no required controls,
hover can request predictive compilation without activation. A later hover can replace
predictive work, but it cannot remove a required input. Predictive work must not
supersede a required settled replacement that restores the authored connections.

The initial scheduling policy is:

1. A settled control can be an exact constant in the active graph.
2. Hover requests a dynamic replacement but keeps the active graph unchanged.
3. An interaction makes its control a required member of `dynamic_inputs`.
4. If matching dynamic code is ready, `GraphExecutor` activates it safely.
5. If matching code is not ready, the UI retains the latest desired value.
6. The active graph continues to use the value representation that it supports.
7. Release ends the undo gesture but retains the required dynamic input while its
   client still hovers the knob. A late dynamic result remains useful in that case.
8. Leaving requests code with the latest settled values and authored connections
   immediately. If release also ends the required lifetime before dynamic code is
   ready, discard that obsolete dynamic result.

Each replacement request carries the exact project revision, request identity,
`dynamic_inputs` set, and settled values. A newer relevant request supersedes the
older request. The compilation worker cancels queued or active work when its LLVM
operation supports cancellation. Otherwise, the worker finishes the obsolete work
and discards its result.

Compilation below approximately 200 ms is a performance target from an early proof of
concept. It is not a guaranteed latency. Front 16 can add reuse or more scheduling
machinery only after measurements show a benefit.

This process changes code specialization. It does not replace graph or node state.

## Compatible control code shares NodeLayout and state semantics

Dynamic and constant-specialized code for one structural graph realization must use
the same canonical `NodeLayout` and `NodeStorage`. A compatible activation is code-only
and requires no storage migration.

The compatibility rule is stronger than identical offsets:

> Compiled code declared layout-compatible must preserve compatible persistent-
> state semantics, not merely the same byte layout.

A constant specialization cannot stop updating persistent state that compatible
dynamic code expects to continue from. A specialization is structural if it changes
graph structure, state identity, initialization, lifecycle, or `NodeLayout`.

## Relationship to GraphJit value specialization

Manual controls are one concrete application of the broader value-specialization
mechanism described in [graph_jit_direction.md](./graph_jit_direction.md).

The specialization key needs both:

```text
structural graph revision identity
exact dynamic-input set / settled specialization values
```

Stale-result rejection follows the same exact-generation principle as other GraphJit
work. Control hover and scrub state can request compilation. They never directly
activate native code. `GraphExecutor` owns safe activation of compatible compiled
code.

## Relationship to project persistence

Manual-control values and their participation policy are semantic project state.
They must eventually serialize with the owning project node/input identity.

Do not persist:

- the currently active code specialization;
- hover state;
- pointer/scrub state;
- interpolation progress;
- pending LLVM requests;
- generated machine code.

Those are realization/interaction details reconstructed from the durable graph and
current UI interaction.

## Live observation is outside Front 14

Front 14 did not implement the observation API. The current normative direction is
[Vue Output Observation And Query Direction](./vue_output_observation_direction.md):
output-first type-registered operations, reflected supported Tick `State` fields,
selective live probes, sparse/persisted Tock output queries, and the planned
`Observations` application module for batched subscriptions/results. Input values
and Tock `State` diagnostics are explicitly outside the initial scope. Concrete
Vue and transport interfaces still follow real client requirements. Manual controls
do not depend on implementing observations first.

## Implementation order

Front 14 uses the completed identity, persistence, GraphJit, and executor contracts:

1. Remove the obsolete backend `NodePresentation`/`IV_NODE_PRESENTATION`
   registration and socket/package plumbing **before** integrating the first Vue
   presentation slice; remove affected tests and old webview registry assumptions
   rather than adding a compatibility adapter. Define the frontend-owned registry,
   generic Vue fallback, and four display modes as part of that replacement.
2. Rebind presentation hosts and long-lived interaction contexts through stable
   semantic identities.
3. Store manual values and participation policies in durable project state.
4. Realize participating manual values as Tick/ephemeral inputs.
5. Add audio-thread-owned interpolation for active dynamic scrubbing.
6. Add one active executable graph and one current replacement request.
7. Add predictive hover compilation, demand-driven activation, stale-result rejection,
   and hover-leave return to constant specialization.
8. Measure compile latency and interaction latency before Front 16 adds reuse or more
   scheduling machinery.

### Implementation status (recorded 2026-11-04, verified)

The manual-control backend is implemented and verified: the repository owner's complete
test suite has passed with it included (status corrected 2026-11-06). `ProjectManualControl` is durable project state keyed by
`ManualControlId`, which contains a stable `NodeAddress` and authored input name.
`ProjectGraph` resolves that identity for each `ConfiguredGraph`, owns client-scoped
hover and interaction lifetimes, and runs the one-request predictive/required
specialization worker. Project persistence and socket RPC preserve the durable value
and participation policy; the socket server supplies the client identity.

GraphJit currently accepts scalar `Sample` Sequential Tick inputs. This is an initial
implementation limit, not a rule that all future manual controls must use `Sample`.
It uses the node's existing Sequential combination callback for a connected input,
maintains one canonical input timeline across constant and dynamic code, and performs
scrub interpolation across the complete audio callback even when the node runs in
smaller primitive slices. Constant code uses compiler-owned immutable sample storage.
Dynamic code reads the latest atomic target and owns the effective interpolation
value in `ManualControlRuntimeState`.

`GraphExecutor::stage_realtime_code()` accepts only code whose project generation,
`NodeLayout`, state structure, lifecycle callbacks, array bindings, and manual-control
bindings are compatible with the accepted structural generation. It changes the
steady Tick operations at `activate_pending()` without rebuilding `NodeStorage`.

The presentation registration and selection path was implemented and verified
(status corrected 2026-11-06): `IV_NODE_PRESENTATION` emits a neutral package
record, and the package finalizer, loader, accepted revision, package catalog,
socket RPC, and VS Code `NodePresentationRegistry` preserve/select it. **This is a
historical implementation fact, not the current destination.** The 2026-11-12
accepted Vue direction removes this backend registration path and allows the
frontend to select presentations using stable node definition IDs. No source
removal is claimed here. The currently landed frontend does not implement live
observation or the intended Vue presentation host.

Status update (2026-11-12, not yet built or tested): the backend registration path
and `NodePresentationRegistry` were removed from source, and the
`intravenous.liveGraph` webview became a Vue application with a frontend-owned
registry, the generic presentation, and the four display modes (Vue client stage 1
in [Vue Client Implementation Stages](./vue_client_implementation_stages.md)).

The key architectural boundary is that presentations and manual controls consume
stable graph/runtime identities; they do not become an alternate owner of graph
construction or runtime state.

### Public module inputs and implicit defaults (2026-11-13)

The owner selected module-input support as part of the first Vue manual-control
feature. A manual value on a module's public input enters at that input, before
its internal routing and channel conversions. It is not a replacement for the
values of whichever internal leaf inputs happen to consume it. Internal fanout,
projection, conversions, and detach operations retain their semantics.

An eligible input's authored default is an implicit manual value until a user
changes it. Hover and an unchanged gesture do not create persisted overrides or
undo entries. Undoing the first changed gesture can restore absence of an
override, allowing the authored default to apply again. Runtime control bindings
can exist for such implicit defaults without requiring saved project records.
