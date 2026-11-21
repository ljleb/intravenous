# Undo/Redo Direction Before The First Implementation

_Historical, non-normative. Captured on 2026-11-13 before the first implementation
status replaced the introductory “not yet implemented” statement. The accepted
contract continues in [Project Undo, Redo, And Backup Direction](../project_undo_redo_and_backups_direction.md).
The implementation status is in [Vue Client Implementation Stages](../vue_client_implementation_stages.md).
The remainder is the preceding document in full._

# Project Undo, Redo, And Backup Direction

_Status: accepted design direction, 2026-11-12; not yet implemented. This document
specifies reversible user edits and their ownership, not concrete RPC routes, C++
record types, or a backup format._

Related: [Project Graph Application Architecture](../project_graph_application_architecture.md),
[Project File Serialization Direction](../project_file_serialization_direction.md),
[Event Propagation Tree Constraint](../event_propagation_tree_constraint.md),
[Node Presentation And Manual Controls Direction](../node_presentation_and_manual_controls_direction.md),
and [Project Session Server Direction](../project_session_server_direction.md).

## Reversible project mutations, not custom undo implementations

Undo and redo apply ordinary typed project-state mutations. One **user action** may
change several records, while one project mutation changes one part of the durable
project model. One undo-history entry describes the accepted effect of the **whole
user action**, with enough information to apply both its forward and reverse changes.
No separate undo-specific operation needs to be authored for each node type or UI
widget. A generic reverse transaction restores the relevant previous durable
records; it does not have to replay a semantically opposite node event.

Examples:

- A completed manual-control gesture records its previous settled stored value and
  its final accepted value, so Ctrl+Z restores the former in **one** step rather than
  stepping through every intermediate scrub update.
- Creating a project instance is reversed by removing the newly created durable
  declaration. Updating an existing declaration is reversed by restoring its
  previous full record; an upsert's inverse is not necessarily a delete.
- Deleting several nodes is one action. Its reverse restores the affected complete
  `ProjectNodeInstanceDeclaration`, `ProjectNodeReplacement`,
  `ProjectConnection`, and `ProjectManualControl` records **that actually changed**,
  preserving stable identities. Do not infer deletion of connections merely from
  losing an endpoint: `GraphConnections` intentionally retains dangling desired
  matchers where applicable.
- An ordered `nodeMessages.dispatch` interaction may make several provisional
  replacements. History records the resulting *committed durable state difference*,
  not a fabricated inverse C++ node-message type, and not its provisional previews.

The initial reversible scope is **project-owned durable state**. Realtime DSP state,
compiled generations, hover status, temporary interaction state, observation data,
external device actions, arbitrary node-message side effects, and filesystem or
network effects do not become automatically undoable. A feature with effects outside
the project model must explicitly define its own semantics before it claims undo
support; do not silently advertise incomplete reversal.

## Capture and apply through one authoritative batch

The server constructs reversals from **authoritative accepted before/after project
state**, not from a Vue read model or an assumed inverse inferred solely from the
requested command. Each owning application module remains authoritative for its
records: `NodeInstances` for instances, `GraphConnections` for connections, and the
appropriate existing owner for node replacements, manual values, or other project
settings. `ProjectGraph` coordinates execution-affecting changes; neither the
history facility nor `ProjectPersistence` becomes a second owner of desired state.

One user action, including undo and redo, must enter every affected application
module **at most once** in its `IV_EVENT_SOURCE` propagation tree. A multi-node
undo therefore cannot be implemented as a series of individual socket RPCs or as
repeated root-build events. The coordinated project-mutation path accepts the
complete proposed batch, computes its authoritative effect and reverse records,
and reports one committed outcome and read-model publication. It uses the existing
project realization/migration pipeline where necessary, rather than a separate
undo graph compiler. Module-local transaction sessions may perform legitimate
intermediate computation without repeatedly propagating events; see the tree
constraint's [module transactions](../event_propagation_tree_constraint.md#module-transactions).

**Atomicity is at the durable project-action boundary:** no partial collection of
accepted project records is exposed as several user edits. Preserve the existing
distinction between *accepted desired project state* and an *executable generation*.
Configuration/JIT failure may retain accepted desired records while leaving the
previous executable graph running; reversals concern those accepted records and use
the same status/diagnostic semantics as normal edits. Do not describe audio cutover
as synchronous with the user's request.

The exact batch record shape, project-history owner/module placement, and event/RPC
signatures follow the first graph-editor undo feature. They must preserve the
existing module ownership and event-tree constraint; do not introduce a parallel
mutation API or duplicate canonical state solely for history.

## User-action boundaries and continuous gestures

A gesture is not one undo entry per update. The host's longer-lived interaction
context identifies the action independently of component mounts, display-mode
changes, or presentation replacement. At action start, retain the pre-action
accepted value; on completion, append one record with the accepted final value.
If nothing changed, append no record. Hover, predictive compilation, and transient
interpolation are not independent history actions.

Cancellation/disconnection must have a deliberate result: either restore the
previous value through the normal accepted mutation path or finalize the last
accepted durable value, consistently with the existing manual-control disconnect
contract. Do not invent a second rollback path through DSP runtime state.

A global history must respect the order in which actions become accepted. In
particular, an open gesture must not later coalesce updates across an unrelated
intervening committed action from another client and silently reorder history.
The precise interleaving/sealing policy can follow real Vue interaction handling,
but history must remain chronologically valid and its reversals must be checked
against the current accepted project state.

## Session history, redo, and multiple clients

Start with **one server-owned, project-session-global undo/redo timeline**. The
frontend sends the user's undo or redo intent while its Intravenous editor has
focus; it does not intercept Ctrl+Z in ordinary VS Code source editors. The
server determines the next applicable record and applies its complete mutation
batch. All clients see the resulting authoritative session revision/notification,
not separate per-client project realities.

A new accepted ordinary user action clears the redo branch. Undo/redo moves the
history position without recursively recording itself as another user action. A
failed or stale reverse transaction must leave both canonical project state and
history position consistent; use explicit applicability/version checks rather
than silently overwriting newer accepted edits. A new session or project reload
starts without an implied persisted undo timeline.

Per-user selective undo is a possible later refinement, **not** the initial rule.
It would require detecting and resolving dependencies between users' edits; simply
keeping separate stacks could undo a state on which another user's later action
relies. This is not a reason to delay a useful global session history.

## Relationship to project JSONL, saves, and backups

`iv_project.jsonl` is a **normalized snapshot of current project intent**, not
an append-only action log. Its accepted command vocabulary is useful for forward
and reverse mutations, but history records are structured typed data during
normal operation; they need not be serialized as JSONL or reuse the persistence
reader verbatim. Ordinary save/autosave writes the current normalized project,
not each action or history entry. Project replay reconstructs the current state;
it does not create an undo entry for each replayed line.

Backup/checkpoint support is a promising **future use of the same project-state
capture and serialization facilities**. A backup is a restorable snapshot at a
point in time; an inverse-only undo entry is not by itself a complete backup.
A future history store could mix full snapshots/checkpoints with reversible
change records, or serialize a chosen undo point together with the base state
needed to reconstruct it. This is a candidate design, not a requirement to
persist session undo history now or to change the canonical project file format.
Backups must preserve the full durable project model and stable identities and
must not confuse runtime/executor state with reconstructable project intent.

## First implementation and verification boundary

Implement the first undo slice after the Vue graph editor can perform actual
instance and connection edits, alongside the required batched project-mutation
path. Test undo/redo of a multi-node deletion (including surviving dangling
matchers and changed replacements/controls), an existing-record upsert, and a
semantic node interaction with successive provisional replacements. Verify one
accepted project action and one normal realization/publication path for each
undo/redo; the application-event tree must contain no repeat module visits.

Then group manual-control gestures as one step, including cancel/disconnect,
component remounts, and multiple clients. Verify stale-action rejection and the
single global history order. Backup persistence, per-user selective history, and
non-project side-effect reversal are deferred.

## Superseded Stage 2 status

The stage document also said:

> The first server-owned undo/redo slice remains unimplemented. Stage 3 has not
> started.

The implementation checkpoint linked above replaces that sentence.
