# Module Interoperability And Graph Macros Direction

_Status: design clarifications from the 2026-11-14/15 discussion. Project-owned
nested scopes, module conversion operations, expanded module interaction, and
macros described below are future work unless an existing document explicitly
identifies an implemented part. The macro system is deliberately **deferred**
until real audio-production workflows justify it; this document is not a new
prerequisite for the active Vue implementation stages._

Related: [Node Definitions And Instances](./node_definitions_and_instances_direction.md),
[Node-Reference DSL](./node_ref_dsl_contract.md),
[GraphBuilder Embedding And Project Port Matchers](./graph_builder_embedding_and_matchers.md),
[Iv Module Source Introspection](./iv_module_source_introspection_direction.md),
[Node Presentation And Manual Controls](./node_presentation_and_manual_controls_direction.md),
[Project Undo/Redo](./project_undo_redo_and_backups_direction.md), and
[Project Playback](./project_playback_and_presentation_grids_direction.md).

## One graph, different construction ownership

C++-authored module structures and project-authored structures are not distinct
classes of executable graph. C++ offers programmable graph construction; the
project editor offers interactive construction. Existing nodes should retain the
same capabilities for connection, automation, observation, introspection, and
presentation regardless of which construction mechanism authored them.

Construction ownership still matters: editing a C++-owned node's existence or
C++-owned connection means changing its source (or converting it to project-owned
structure). Project editing may add its own connections to addressable internal
ports without deleting those source-owned nodes or edges. Existing input
composition and neutral-value semantics govern multiple connected sources;
this does **not** introduce a new override or conflict rule for such connections.

Every semantically addressable internal port is connectable without an explicit
"activate", "expose", or "promote" step. Collapsing a module or grouping its
ports changes discovery/presentation only. GraphJit already handles unconnected
ports through its normal specialization path; UI visibility is not a separate
runtime port-enablement state. Direct automation wiring should use ordinary
port connections, including to nested virtual-node ports.

## Nested module presentation and project-owned scopes

A project module lane may expand to show the module's C++-constructed subgraph
inside a collapsible nested region. Source-owned child nodes and connections
remain individually non-deletable through ordinary project graph edits, but
project-authored connections may address their ports through the existing
`ProjectNodePortMatcher` hierarchy. A source-selection side panel can become a
filtered presentation of that same module instance using the common lane UI;
it is not a second graph or a less capable interaction system.

**Accepted visibility rule (2026-11-20).** A C++-authored child is addressable
when the existing project node selector syntax can reach it. This includes
named tiled children reached with `#name`. Expanded modules show addressable
children, and project connections can target their ports. Children that no
selector can reach stay hidden. A future inspection or module-copy operation
can revisit their visibility.

### Module Ports and child lanes

Click the footer of a module header to expand or collapse its children. A faint
gradient fades upward from the footer when the pointer enters the header.
The gradient brightens slightly and the cursor changes to a hand over the footer.
The footer sits below the ports.
The click area extends just below the seam into the next lane header. A name
that links to source takes priority in that area.
Double-click an editable name to rename it. Click a source link to open the source.
[Earlier double-click expansion](./historical/module_expansion_before_header_footer_2026-11-21.md).
[Earlier footer highlights](./historical/module_footer_before_inset_line_2026-11-21.md).

When a module with public ports expands, Module Ports is its first child lane.
The lane shows the sockets on the inner side of the module boundary. A public
input becomes an output socket inside the module. A public output becomes an
input socket inside the module. Project connections can use either side of the
boundary and store which side each endpoint uses.

The module header shows the outer sockets, manual input knobs, and output gain
knobs. The Module Ports header has its own shading and contains only sockets and
their labels. The module address identifies both sides of the boundary.
C++ connections appear as solid gray cables. Edit those cables in the module source.

C++ child lane headers use the same diagonal stripes as VU meters that are
waiting for data. Module Ports also uses this texture. The knobs stay editable.
Each expanded group has an inset shadow around its descendants, with a visible
edge above its first child and below its last descendant. The shadows and lane
headers share a left edge at every depth.

### Project lanes within modules

A project lane can sit under a module instance in the graph editor. The
project presentation stores the parent address and the lane order. The node
keeps its project instance ID, ports, and connections. Operators can still
edit, wire, duplicate, or delete the project node from its nested lane.
If the parent address becomes unavailable, the editor shows the lane at the
root until the address resolves again.

Shift+Right moves each selected run into the module immediately above it at
the same level. Shift+Left moves nodes one module up. Root lanes stay in
place on Shift+Left. Shift+Up and Shift+Down reorder siblings. Shift+drag
from a selected header moves the selection to a module or a sibling insertion
point. Module Ports and C++ children keep their source-defined order. A
project edit saves the parent changes and order together for undo and redo.
Project lanes can move past C++ siblings. Module Ports stays at the top.

Drag the header separator to resize the headers in the current webview. The
timeline opens at the smallest editable height on the vertical wheel zoom grid.

The Add Node picker lists exported definitions. An addressable package-private
child can appear in its module's expanded lanes and take part in project
connections. Only project instances can be duplicated. Duplicating a module
runs its constructor to create its children.

Each virtual node has a lane with the C++ declaration name as its primary
label and the node's display name as its secondary label. Expanding the lane
shows its concrete members. A member lane puts the declaration name and
one-based member index first and its display name second. When a tile expands,
its children put their channel names first.

A concrete member can appear under several virtual nodes. Those rows
share project edits and saved presentation settings.
Each virtual port maps to the matching ports of its members.

When a C++ declaration produces several concrete types, each type has
its own virtual lane. Each lane exposes the inputs, outputs, and members
for that type. A virtual output meter measures the sum of the members on
each shared channel.

Project-authored nested subgraph scopes remain planned separately; see
[Subgraph presentation layout](./node_presentation_and_manual_controls_direction.md#subgraph-presentation-layout).
Their nodes and connections are project-owned and editable. Scope boundary
interfaces, identities, and persistence must be designed as part of that
feature, rather than assumed to be implemented already.

### Inline Module — agreed operation name; not yet implemented

`Inline Module` replaces **one** C++ module instance's outer node shell with a
project-owned subgraph scope. The immediate children of the configured module
become project-owned instances *inside that scope*; nested module instances
remain modules. This is **shallow** inlining, not flattening of the entire
descendant tree. Preserve the configured structure, connections, relevant
identities and configuration when materializing project intent, and retain the
original module definition/instance provenance as queryable metadata.

A built-in breadth-first application of shallow inlining should allow further
levels to be converted progressively. The exact durable record schema and
identity remapping are not specified here; they belong to project-owned scope
and conversion implementation work. One accepted transformation should be a
single project action that ordinary before/after undo restores without an
"inverse module constructor".

### Extract Module — agreed operation name; not yet implemented

`Extract Module` produces GraphBuilder-based C++ module construction code from a
project-owned subgraph, including its supported authorable construction data.
The desired contract is lossless graph-construction equivalence without
node-specific exceptions. If some authorable information cannot be represented,
report the limitation explicitly rather than silently omitting or approximating
it. This does not require recovering loops, conditionals, or other source-level
logic from a realized graph; explicit generated construction calls suffice.

This conversion does not promise preservation of volatile running execution
state, and inlining does not imply that the implementations of leaf definitions
are copied into the project. An extracted module's publication/registration
remains a separate concern.

There is **no** proposed module inverse-function registration. Creating an
ordinary module instance is already the straightforward way to use an existing
module. Any bespoke matching/restructuring of project graphs can eventually be
written as a macro rather than defining a second module-construction protocol.

## Future GraphBuilder-based graph macros — explicitly deferred

The long-term goal is one C++ graph-manipulation vocabulary shared in
*implementation*, not just spelling, by transient `GraphBuilder` construction
and a persistent project-edit transaction interface. This is a design constraint
for future refactoring, **not** a request to refactor GraphBuilder or implement
macros during the current Vue stages. Builder-local `NodeRef` handles and stable
project identities have different lifetimes and may need different backends.

A macro operates on a supplied scope; invoking the same macro on the project
root is a project-wide script. A macro can invoke smaller macros. Nested calls
should inspect their shared pending graph edits and ordinarily commit the
persistent portion as a single authoritative project action, using the existing
project mutation/history mechanism rather than a second source of project
state.

### Inspection and transformation coverage

Every project-authorable property, **including authored tags and metadata**,
should eventually be inspectable and modifiable through the macro interface
subject to the property's ordinary ownership rules. The target includes:

- project instances, nested scopes, construction arguments, node replacements,
  connections, and project-owned manual controls;
- child and connection enumeration, port kind/name/type/layout and connectedness,
  event and continuous inputs/outputs, and relevant configured execution
  capabilities (Tick, Tock, replayability, history requirements);
- structural predicates over those facts, not only name- or tag-based lookup;
- creation, wiring, disconnection, deletion, reconfiguration, tagging, and
  higher-level graph operations such as redirecting connections or shallow
  module inlining.

The exact reflection surface is not designed here. For example, authored
history requirements and currently resolved execution/storage history are
potentially different facts; the future API must identify which one a query
reports. Structural inspection should not pretend to mutate C++-owned source
structure. It may inspect those nodes and create permitted project-owned edits
that refer to them.

### Node metadata and provenance

Leaf and module definitions should be able to contribute metadata/tags, and
module builders should be able to tag constructed nodes. Definition identity,
package identity, node kind, and other objectively derivable facts should be
queryable automatically, without requiring duplicated mutable tag entries.
When an instance is inlined, its originating module identity/provenance should
remain inspectable on the resulting project-owned structure. Separate derived,
source-authored, and project-authored facts by ownership while allowing one
query interface.

The historical lane metadata/query design already describes typed metadata,
query syntax, and completion tooling; see
[Lane Filters Query Design](./historical/lane_filters_query_design.md) and
[Lane Query Tooling Direction](./historical/lane_query_tooling_direction.md).
Reuse/adapt the useful parts rather than assume the old lane-specific runtime is
the current project-graph implementation. Arbitrary C++ structural predicates
are a complement to, not a replacement for, simple tag queries.

### Invoking-client, runtime, and transport state

A macro can be useful without changing any project declaration. The future
context should expose the invoking client's editable lane/node selection (a
lane may correspond to **a set** of semantic nodes), viewport position, and
horizontal/vertical zoom. It should also support reading and controlling the
playhead/transport, and the appropriate manual-input/knob state. These are
additional to authorable project configuration and connection state.

Selection and viewport state belong to the invoking client; transport follows
its backend/session ownership; project declarations follow the persistent
project mutation boundary. A macro that navigates to a sample index, zooms,
seeks, or changes selection should **not** create a project undo entry merely
because the change was triggered by a macro. Persistent project edits still
use normal action-based history; transient knob gestures retain their own
semantics. Combining these state domains in one macro must not silently make
all of them one undoable durable transaction.

### Use cases and timing

Macros can select nodes by functional properties—for example, the number of
sequential inputs satisfying a required-history condition—and focus them in
the editor; they can also redirect wiring, restructure project-owned subgraphs,
or apply recurring production edits across projects. This is a **future
production-workflow feature**, not part of the immediate automation-curve or
observation milestone. As project-owned scopes are implemented, avoid
unnecessarily duplicating the GraphBuilder connection/reference DSL, but do not
prematurely freeze a shared class hierarchy before the actual edit operations
are understood.
