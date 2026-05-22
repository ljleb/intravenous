# Application Modules And Batched Event Architecture

This note captures a preliminary architectural direction for Intravenous.

It is intentionally high level. The goal is to establish boundaries and event
semantics before code is split further.

## Goal

The application should be organized around a small number of long-lived
application modules.

An application module is:

- fixed in quantity at runtime
- responsible for one clear kind of data or one clear conversion concern
- privately stateful
- connected to other application modules only through static events and bridges

This is distinct from an `intravenous-module`, which refers to DSP code and its
runtime build/load/reload lifecycle.

## Core Principles

### One module per kind of maintained data

We should split around real domains of owned state, not around vague
"responsibilities".

Examples:

- intravenous-module definitions
- intravenous-module instances
- lane graph state
- lane views
- JSON-RPC transport/conversion

Some application modules may own collections. Others may own no list-like data
at all and instead exist only to translate between representations or protocols.
`SocketRpcServer` is an example of the latter.

### Fixed module count, dynamic owned data

Application modules are fixed in count for the lifetime of the process.

Dynamic objects such as:

- intravenous-module definitions
- intravenous-module instances
- lane views
- watched files
- introspection snapshots

should be owned inside those fixed modules, not promoted to separate global
singletons.

### Communicate only through events

Application modules should not directly reach into one another's internal state.

They should instead:

- publish domain events
- subscribe through bridges or direct handlers
- gate their behavior on whether the receiving module has been initialized

This keeps modules independently testable and reduces construction-order
coupling.

### Batch events, do not emit repeated partial updates

If one control-flow source would naturally produce several updates of the same
kind in a row, those updates should be bundled into a single event object.

Prefer:

- `FilesChanged`
- `ModuleInstancesDiff`
- `LaneGraphChanged`
- `LaneViewsUpdated`

over:

- `FileChanged`
- repeated `InstanceAdded` / `InstanceRemoved` / `InstanceChanged`
- repeated per-lane notifications during one reconciliation pass

This avoids:

- repeated expensive downstream work
- temporary inconsistent states
- UI partial updates
- fragile user interactions in the middle of an update sequence

Not every event must contain a list, but repeated same-kind events from the same
source should be treated as a design smell.

## Important Distinctions

### File watching is not reloading

The file watcher should be generic infrastructure.

It should:

- own watched roots/files
- report batched file-change events

It should not:

- know what an intravenous-module is
- decide how a file change should be handled
- trigger reloads directly

Reload decisions belong to the application module that understands the affected
domain.

### Intravenous-module definitions are not instances

An intravenous-module definition is analogous to a class.

An intravenous-module instance is analogous to an instance of that class.

These should be owned separately.

The definitions module should own:

- loading
- reloading
- loaded definition state
- dependency knowledge

The instances module should own:

- created instances of those definitions
- reconstruction after definition reload
- diffing instance-level changes

### Loading/reloading is not introspection

The system that knows how to build/load/reload intravenous-modules should not
also be the place that owns the query/introspection model.

A better split is:

- one application module owns intravenous-module definition lifecycle
- another owns introspection/query state derived from those definitions

This separation is important for tests and for future support of multiple
definitions and multiple instances.

## Preliminary Application Modules

This is not yet a complete final list, but it reflects the current direction.

### Application environment

Owns stable shared process/application context such as:

- workspace root
- discovery roots
- toolchain settings/defaults

This is global because it is stable shared context, not because it owns dynamic
project state.

### File watcher

Owns:

- watched files/roots
- any debounce/coalescing policy

Publishes:

- `FilesChanged`

Consumes:

- watch/unwatch/update-watch requests

### Intravenous-module definitions

Owns:

- loaded intravenous-module definitions
- build/load/reload mechanics
- dependencies per definition

Publishes:

- definition load/reload success
- definition load/reload failure
- batched definition change events where appropriate

Consumes:

- file-change events
- definition lifecycle requests

### Intravenous-module instances

Owns:

- instances of loaded intravenous-module definitions
- mapping from instance to definition
- reconstruction/diffing when a definition changes

Publishes:

- batched instance diffs

Consumes:

- definition lifecycle events
- instance create/delete requests

### Intravenous-module introspection

Owns:

- introspection snapshots
- logical-node/source-span/type/port indexes
- query-facing model derived from current definitions and/or instances

Publishes:

- batched introspection/model change events

Consumes:

- definition events
- instance events where necessary

### Timeline lanes

Owns:

- lane graph
- lane bindings
- connections
- lane-affecting timeline state

Publishes:

- batched lane graph diffs

Consumes:

- instance/introspection change events

### Lane views

Owns:

- all lane views
- each view's visible window and related per-view state

Each lane view needs to remember at least:

- first visible element
- visible count

Potentially also:

- filter state
- sort state
- cached visible slice

Publishes:

- batched lane view updates

Consumes:

- lane graph diff events

### JSON-RPC server

Owns:

- JSON-RPC transport
- request parsing
- response formatting
- notification formatting

Publishes:

- inbound request events

Consumes:

- lane view updates
- status/message/model events that should be forwarded to clients

This module is primarily a conversion boundary and may not own list-like domain
state.

## Event Chaining

The intended shape is an event chain where each step only understands its own
domain.

For example:

1. file watcher publishes `FilesChanged`
2. intravenous-module definitions react and publish definition reload events
3. intravenous-module instances react and publish instance diffs
4. timeline lanes react and publish lane graph diffs
5. lane views react and publish view updates
6. JSON-RPC reacts and notifies the client

Each stage should consume domain events and publish domain events. No stage
should need to reach into the internal state of the previous one.

## Design Guidance For Future Splits

When introducing or revising an application module, define:

1. what state it owns
2. what events it publishes
3. what events it consumes

If those three lists are not clear, the boundary is probably wrong.

When designing events, prefer:

- one event that reports one coherent domain transition
- one payload that can be processed atomically

Avoid:

- repeated same-kind events for one source transition
- partial updates that require subscribers to guess when the update stream is
  complete
- event names that hide whether the payload is authoritative state, a diff, or
  a request

## Immediate Implication

`RuntimeProjectService` is currently too broad.

Future refactors should move its embedded domains into standalone application
modules, especially:

- intravenous-module definition lifecycle
- intravenous-module instance lifecycle
- introspection/query model
- lane graph/view state

The first extraction should prefer a true domain boundary over a convenience
helper boundary.
