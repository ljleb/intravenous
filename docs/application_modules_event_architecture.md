# Application Modules And Batched Event Architecture

This note captures the current architectural direction for Intravenous as it is
actually being implemented.

It is intentionally high level. The goal is to keep ownership boundaries and
control-flow seams clear as the runtime grows.

## Goal

The application should be organized around a small number of long-lived
application modules.

An application module is:

- fixed in quantity at runtime
- responsible for one clear kind of owned state or one clear translation domain
- privately stateful
- connected to other application modules only through static events and bridges

This is distinct from an `intravenous-module`, which refers to DSP code and its
build/load/reload lifecycle.

## Core Principles

### One module per maintained domain

We should split around real owned data, not vague “responsibilities”.

Examples:

- iv-module definitions
- iv-module instances
- iv-module reload state
- graph-input lane policy
- lane graph state
- lane views
- JSON-RPC transport

Some modules own dynamic collections. Others own no durable domain data and
exist only to translate between modules or protocols. `SocketRpcServer` is an
example of the latter.

### Fixed module count, dynamic owned data

Application modules are fixed in count for the lifetime of the process.

Dynamic objects such as:

- iv-module definitions
- iv-module instances
- watched dependencies
- introspection snapshots
- lane views

should be owned inside those fixed modules, not promoted to new global
singletons.

### Communicate through events and bridges

Application modules should not directly reach into one another’s internal
state.

They should instead:

- publish domain events
- consume those events through bridges or explicit handlers
- treat initialization and binding as external wiring concerns

This keeps modules independently testable and reduces construction-order
coupling.

### Batch updates

If one control-flow source naturally produces several updates of the same kind
in a row, those updates should be bundled into a single event object.

Prefer:

- `IvModuleDefinitionsChanged`
- `IvModuleInstancesChanged`
- `TimelineLanesChanged`
- `LaneViewsUpdated`

over repeated single-item notifications during one reconciliation pass.

This avoids:

- repeated downstream work
- temporary inconsistent states
- partial UI updates
- user interactions during an unstable intermediate state

## Important Distinctions

### Watching is not reloading

File watching should remain generic infrastructure at the utility-class level.

The app-level owner should be domain specific.

The current intended split is:

- a generic watcher utility class handles low-level path watching
- `IvModuleReload` owns:
  - watched definition declarations
  - dependency tracking for reload purposes
  - change detection
  - reload attempts

`IvModuleReload` should not own the live loaded definition map. It should only
produce reload results.

### Definitions are not reload

`IvModuleDefinitions` is the owner of declared and live loaded definitions.

It should own:

- declared definition ids and module roots
- current live loaded definition state
- canonical builder ownership
- public created/updated/deleted definition diffs
- definition-domain notifications

It should not own:

- file watching policy
- dependency watching state
- reload scheduling

Those belong to `IvModuleReload`.

### Definitions are not instances

An iv-module definition is analogous to a class.

An iv-module instance is analogous to an instance of that class.

These should be owned separately.

`IvModuleInstances` should own:

- desired instance declarations
- instance identity
- mapping from instance to definition id / module root
- realized instance state derived from live definitions
- user-facing instance CRUD

`IvModuleDefinitions` should not be a user-facing control surface in the real
app flow. In normal operation, active definitions are derived from the instance
set.

### Loading/reloading is not introspection

The system that knows how to build/load/reload iv-modules should not also own
the query model.

A better split is:

- `IvModuleDefinitions` owns live definition state
- `IvModuleSourceIntrospection` owns source-span / logical-node / port query state

This separation matters for tests, for multi-definition support, and for future
instance-oriented project state.

### Graph-input policy is not timeline policy

`GraphInputLanes` should own DSP graph-input lane policy.

It should own:

- graph-input port set derived from iv-module instances
- logical knob state
- concrete override state
- graph-input lane queries and mutations

`Timeline` should remain the generic lane substrate:

- lane storage
- lane connections
- lane change events
- generic lane execution substrate

Timeline should not become the owner of logical-node-id-driven graph-input
policy.

## Current Module Set

This reflects the current code direction.

### StartupConfig

Owns stable startup context such as:

- workspace root
- discovery roots
- toolchain/search-path settings

This is process/application context, not dynamic project state.

### IvModuleInstances

Owns:

- desired iv-module instances
- realized instance list
- required-definition derivation from the instance set
- instance list notifications and instance CRUD

Publishes:

- required-definition diffs
- instance diffs
- instance-list updates

Consumes:

- definition change events
- JSON-RPC instance create/delete requests

### IvModuleDefinitions

Owns:

- declared definitions
- live loaded definitions
- canonical builders
- definition-domain notifications
- public definition diffs

Publishes:

- declaration changes toward reload
- live definition changes toward downstream consumers
- definition status/message notifications

Consumes:

- required-definition diffs from instances
- reload results from `IvModuleReload`

### IvModuleReload

Owns:

- reload-oriented dependency state
- watcher integration
- reload attempts
- success/failure reload results

Publishes:

- reload results

Consumes:

- definition declaration changes
- watcher-detected change signals

### IvModuleSourceIntrospection

Owns:

- introspection snapshots
- logical-node/source-span/type/port indexes
- query-facing projection from current live definitions

Publishes:

- query results only when requested

Consumes:

- definition changes
- live input snapshot answers from `GraphInputLanes`

### GraphInputLanes

Owns:

- the DSP graph-input lane subset
- logical knob state
- concrete override state
- graph-input lane queries/mutations

Publishes:

- graph-input lane requests toward timeline
- live input snapshot responses toward introspection

Consumes:

- instance changes
- JSON-RPC-driven control mutations routed through owners

### Timeline

Owns:

- lane graph
- lane bindings
- lane connections
- lane change notifications
- generic timeline/lane substrate behavior

Publishes:

- timeline lane change events

Consumes:

- graph-input lane requests
- lane-view queries through bridges

### LaneViews

Owns:

- lane views
- view-local filtering/window state

Publishes:

- lane-view updates

Consumes:

- timeline lane change events
- lane query requests through bridges

### SocketRpcServer

Owns:

- JSON-RPC protocol mechanics
- request/response envelopes
- client connection lifecycle
- outgoing notifications

It should remain a transport adapter, not a domain-state owner.

## Current Control-Flow Spine

The intended app flow is now:

1. `StartupConfig` resolves startup context
2. `IvModuleInstances` owns desired instances
3. `IvModuleInstances` emits required-definition diffs
4. `IvModuleDefinitions` owns the active live definitions for that required set
5. `IvModuleDefinitions` emits declaration diffs to `IvModuleReload`
6. `IvModuleReload` emits `loaded[]` / `failed[]` results
7. `IvModuleDefinitions` applies those results and emits public definition diffs
8. downstream modules (`IvModuleSourceIntrospection`, `GraphInputLanes`, later
   execution owners) react

The important asymmetry is:

- instances control which definitions should exist
- reload does not own live definitions
- definitions compute public diffs

## Intentional Non-Goals

This architecture does not imply:

- a generic project-state god object
- a generic filesystem app module
- direct cross-module mutation of private state
- per-item same-kind event spam

Those are exactly the shapes we are trying to avoid.
