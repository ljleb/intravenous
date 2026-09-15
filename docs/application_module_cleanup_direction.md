# Application Module Cleanup Direction

This note records the next application-module maintenance boundary before the
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

## Explicitly deferred decisions

This cleanup does not yet decide:

- the new project-wide graph execution app-module decomposition
- ownership of realtime execution scheduling
- ownership of compiled sample/event query planning and caching
- the future source model for lane filters/views/query schema
- the future data-source API consumed by `LanesVisualization`
- how `SystemAudioDevices` attaches to graph execution
- which new isolated app modules should mediate persistence and execution state

Those decisions should be made after the deletion/refocus pass, against the
smaller surviving module graph.

## Resulting retained module inventory

After the cleanup, the intended retained/refocused app-module set is:

- `SystemAudioDevices`
- `LanesVisualization`
- `LaneFilters` (temporarily disconnected)
- `LaneQuerySchemaService` (temporarily disconnected)
- `LaneViews` (temporarily disconnected)
- `IvModuleInstances`
- `IvModuleDefinitions`
- `IvPackageReload`
- `IvModuleSourceIntrospection`
- `IvPackageDefinitions`
- `ProjectPersistence`
- `ProjectAutosave`
- `SocketRpcServer`

This is a maintenance checkpoint, not the final module set. New execution-side
modules should be added only after their ownership can be stated independently
and their event boundaries are clear.
