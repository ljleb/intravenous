# Application Module Cleanup Direction

> **Status:** completed cleanup checkpoint. The lane/timeline/task-runner deletion
> described here has landed. The authoritative replacement project-graph module
> design is now
> [project_graph_application_architecture.md](./project_graph_application_architecture.md).
> Generalized future names drop the redundant `Iv` prefix: `NodeDefinitions`,
> `NodeInstances`, `PackageReload`, and `PackageDefinitions`.

This note records the application-module maintenance boundary that preceded the
project-wide graph execution redesign.

It is intentionally a cleanup plan, not a design for the replacement executor.
The purpose of this step is to remove obsolete lane-node execution ownership,
retain independent product/runtime modules, and leave clean seams for the next
set of isolated app modules.

Where this note conflicts with older lane/timeline/task-runner architecture
notes, this note takes precedence for application-module ownership and module
lifetime.

## Application-module rule

A long-lived app module is constructed once by application startup and owns one
coherent domain. App modules communicate with other app modules only through
linker-set events and explicit bridge bindings.

In particular:

- app-module constructors should not receive references or pointers to other app
  modules
- app modules should not call other app modules directly
- `app.cpp` owns construction order and bridge binding lifetime
- bridges translate events between modules without merging their ownership
- ordinary utility classes and short-lived helpers are not app modules and may
  be used directly by their owning module

The cleanup should move the existing runtime toward this rule rather than add
new direct dependencies as temporary migration shortcuts.

## Modules to delete

The following app modules belong to the lane-node execution architecture and
should be removed entirely:

- `Timeline`
- `TimelineExecution`
- `GraphInputLanes`
- `ConfiguredLanes`
- `TasksRunner`
- `IvModuleInstancesExecution`

Deleting these modules also means deleting bridges, RPC handlers, persistence
integration, startup wiring, and app-specific support code whose only purpose is
to serve them.

This decision does **not** select their replacement execution architecture yet.
The replacement should be introduced as new isolated app modules after this
cleanup exposes the remaining ownership boundaries.

## Modules to keep and refocus

### `AudioDeviceLanes` -> `SystemAudioDevices`

Keep the app module, but rename it to `SystemAudioDevices` and remove its lane
semantics.

It should retain only audio-device responsibilities that make sense without the
lane model, such as:

- system audio-device enumeration
- selected input/output device configuration
- device open/close and lifetime management
- device-facing callbacks and buffering
- audio synchronization/resampling logic that is independent of lane identity

It should no longer own or expose:

- lane ids
- timeline lanes
- lane-node construction
- lane connections
- task-runner registrations
- `TimelineExecution` integration

How system audio is connected to the future project-wide graph executor is a
later design decision.

### `LanesVisualization`

Keep `LanesVisualization` as an isolated app module.

Its current dependencies on `Timeline`, `TimelineExecution`, and `TasksRunner`
are obsolete and will need to be replaced. Do not delete the module merely
because those producers disappear.

The module remains the intended owner of visualization-specific state and
publication behavior. Its future input/data-source boundary should be designed
separately after the old lane execution modules are removed.

### `LaneFilters`

Keep the implementation, but disconnect it from the application module graph
for now.

Do not bind it to a replacement data source during this cleanup. Its eventual
input model should be decided together with the replacement graph/query model.

### `LaneQuerySchemaService`

Keep the implementation, but disconnect it for now.

The service remains useful query tooling, but the current timeline-derived
schema source should not force a temporary compatibility layer.

### `LaneViews`

Keep the implementation, but disconnect it for now.

Its future source of objects/results should be designed after the old timeline
and lane-node ownership model has been removed.

## Modules to keep

The following app modules remain part of the application architecture:

- `IvModuleInstances`
- `IvModuleDefinitions`
- `IvModuleSourceIntrospection`
- `ProjectPersistence`
- `ProjectAutosave`
- `SocketRpcServer`

Their existing bridges to modules being deleted will naturally disappear.
Keeping a module does not imply preserving every current event or handler on
that module; lane-node-specific integration should be removed with the owner it
serves.

## Package ownership cleanup

### `IvModuleReload` -> `IvPackageReload`

Rename `IvModuleReload` to `IvPackageReload` and make its ownership explicitly
package-scoped.

Its responsibility is to:

- watch the relevant filesystem/package inputs
- detect changes affecting packages
- trigger/reconcile package reloads
- publish package reload success/failure/change results

It should not reason about individual iv-module instances or module-specific
runtime behavior. Module definitions may be produced or updated as a
consequence of package reload elsewhere in the application, through events.

The low-level filesystem watcher remains implementation infrastructure rather
than a separate app module unless a later design gives it independent domain
ownership.

### `IvPackages` -> `IvPackageDefinitions`

Rename `IvPackages` to `IvPackageDefinitions`.

It should own the package-definition/catalog side of the product surface, such
as the set of known/configured packages and package-level user operations.
Exact package-definition fields can evolve independently of this cleanup.

Most importantly, `IvPackageDefinitions` must not directly retain references to
other app modules. The current direct constructor/reference relationship to
`IvModuleDefinitions` and `IvModuleReload` should disappear.

Communication with `IvModuleDefinitions`, `IvPackageReload`, persistence, RPC,
or future modules must go through linker-set events and bridges like other app
modules.

## Support objects that are not app modules

This cleanup should not mechanically promote every long-lived helper in
`app.cpp` into an app module.

Current examples include:

- `StartupConfig`
- `ProjectAutosaveService`
- `IvPackageReloadService`
- shutdown/lifecycle scopes

These are lifecycle or orchestration helpers. They can be renamed or simplified
as their owning modules change, but they are not part of the app-module
keep/delete inventory unless their ownership model changes explicitly.

## Temporary disconnected state

During this maintenance phase it is acceptable for these retained modules to be
constructed less often or not constructed/bound by the main application at all:

- `LaneFilters`
- `LaneQuerySchemaService`
- `LaneViews`

`LanesVisualization` is also expected to lose its old execution-source bridges
before it gains its future source.

This temporary disconnection is preferable to introducing adapters whose sole
purpose is to emulate the modules being deleted.

## Decisions made after this checkpoint

The previously deferred execution-side decisions are now specified in
[project_graph_application_architecture.md](./project_graph_application_architecture.md).
In particular:

- `ProjectGraph` owns durable user graph intent **and** orchestrates complete root
  `GraphBuilder` transactions; there is no separate `RootGraph` app module.
- `NodeDefinitions` owns one immutable versioned definition snapshot containing
  both leaf and module node definitions.
- `NodeInstances` performs one-snapshot batched recursive configuration and owns
  reusable configured node-instance caches.
- `GraphConnections` resolves recursive `ProjectNodePortMatcher`s only after the
  complete node batch has been embedded.
- `GraphExecutor` owns compilation/optimization and active/pending whole-project
  executable generations, with activation only between complete audio passes.
- `SystemAudioDevices` supplies stable logical bindings for requested ids and no
  longer creates project graph structure as part of its core responsibility.
- tree-shaped event procedures are documented under
  [event_flows/](./event_flows/README.md).

Lane filter/view/query and visualization source redesign remains separate work.

## Resulting retained module inventory

Immediately after the cleanup the retained/refocused set still used several
`Iv*` names. The next implementation phase generalizes/renames those modules and
adds the project-graph pipeline. Its target core inventory is:

- `SystemAudioDevices`
- `LanesVisualization`
- `LaneFilters` (temporarily disconnected)
- `LaneQuerySchemaService` (temporarily disconnected)
- `LaneViews` (temporarily disconnected)
- `NodeInstances`
- `NodeDefinitions`
- `PackageReload`
- `NodeSourceIntrospection` (provisional generalized name)
- `PackageDefinitions`
- `ProjectGraph`
- `GraphConnections`
- `GraphExecutor`
- `ProjectPersistence`
- `ProjectAutosave`
- `SocketRpcServer`

See the current architecture document for exact responsibilities and event
procedures.
