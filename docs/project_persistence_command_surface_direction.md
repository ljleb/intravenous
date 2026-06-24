# Project Persistence And Command Surface Direction

This note consolidates the current direction for project persistence, shared
modification commands, config-file ownership, and authored-state boundaries.

It is intentionally design-only.

## Core direction

Project persistence should be shaped as a list of commands to apply in order to
create the current project state.

This implies:

- the project file is a reconstruction program, not a runtime snapshot
- project loading should not need to pretend to be a UI session
- JSON-RPC and project loading should both reuse the same transport-independent
  modification surface

The key architectural move is to introduce a stable bottleneck app module that
receives authored project modification commands regardless of their source.

Possible names:

- `ProjectCommands`
- `ProjectMutations`
- `AppCommandSurface`

The exact name can be chosen later. What matters is the role.

## Shared command surface

The shared command surface exists so that:

- JSON-RPC can remain only a transport adapter
- project loading can replay the same typed mutations without UI emulation
- tests can eventually invoke the same mutations directly
- cross-module orchestration can live in one stable place if no simpler single
  owner exists

This module is allowed to be a bottleneck on purpose.

It should expose only authored mutation commands, not queries.

The preferred implementation shape is intentionally minimal:

- a thin event-forwarding surface
- one stable mutation ingress for JSON-RPC and project loading
- no policy-heavy normalization layer inside the dispatcher by default

The dispatcher may still become a useful orchestration point in rare cases, but
that is a fallback, not the desired steady state.

Examples:

- create iv-module instance
- delete iv-module instance
- set iv-module instance settings
- set graph-input port policy
- set module-managed lane settings
- set project-overridable execution settings

Queries such as graph lookups or lane inspection do not belong here.

## New app modules

Two new app modules are implied by this direction.

### Shared modification module

A stable transport-independent modification module:

- receives typed authored mutation commands
- invokes owning/managing modules
- acts primarily as a thin event forwarder
- may optionally coordinate more than one module when needed, but should not
  become orchestration-heavy by default

### Project persistence module

A dedicated persistence module:

- loads the project file
- saves the project file
- owns normalized reconstruction output
- asks modules to contribute commands into a command-list builder
- replays loaded commands only through the shared modification module

This module must not become a second mutation API.

Save should not happen implicitly from ordinary project mutations.

Instead:

- JSON-RPC should expose an explicit save command
- that command should enter the project persistence module
- the project persistence module should then raise its save-side contribution
  event
- contributors should fill the builder
- the builder should lower into the final command list
- only then should the file be written

## Event direction

The persistence module likely needs at least two important event flows.

### Save-side contribution event

Subscribers contribute authored commands into a command-list builder.

Important property:

- subscribers describe current authored state
- they do not emit transient edit history

### Load-side replay path

Loading interacts only with the shared modification module.

Important property:

- project load does not directly mutate owning modules through custom private
  persistence hooks

Replay semantics should be:

- best-effort
- failure-logged

That means:

- a command that can be applied should be applied
- a command that fails should emit a clear diagnostic
- later commands are still attempted in file order
- dependency truth is discovered dynamically by real command execution rather
  than by a separate dependency graph or annotation layer

## Config-file split

There should be three distinct config/storage locations.

### Installation defaults file

The installation defaults file remains the right place for installation-scoped
defaults.

The existing `.intravenous_defaults` file is a good home for this.

Examples:

- default audio device selection
- default execution settings
- toolchain/build defaults if they remain installation-scoped

### Project file

Project-specific config and replay commands should move into a dedicated
project-owned file such as:

- `project.intravenous`

This file should own:

- the project command list
- project-owned settings
- project overrides of installation defaults

### Iv-module `.intravenous`

The `.intravenous` file should be reserved for iv-module-local settings only.

It should not contain:

- JUCE path
- compiler path
- C++ executable path
- general compilation settings

The project needs to own compilation compatibility so that all iv-modules built
for the project remain compatible together.

## Current config reality

The runtime now uses:

- `project.intravenous` as the project-owned command/settings file
- `.intravenous_defaults` as the installation defaults file

Module-local `.intravenous` files are still reserved for iv-module-local
settings, but current project persistence work does not rely on them.

## One route per purpose

We should never create two different command routes for the same logical
setting where one route exists for the project value and another for the
installation default value.

Instead:

- installation defaults are edited in installation config
- the shared modification command uses one value domain
- `default` is a first-class value in that domain

So a project-owned setting may hold either:

- an explicit concrete value
- `default`

and `default` means:

- resolve through installation defaults

Resolving `default` is not the job of the thin shared command dispatcher.

It belongs with the owning/managing module and the persistence/config model.

This should apply to things like:

- audio device selection
- compiled sample cache chunk size multiplier

The current compiled sample cache chunk size multiplier should be treated as:

- project-owned
- installation-defaulted

If later we determine that `1` is always optimal, the setting can be removed.

## Project file shape

The project file should use one command per line.

Reasons:

- easier to understand
- easier to diff
- easier to resolve conflicts by hand
- easier to keep dependent commands localized

Commands may depend on earlier commands.

Related commands should ideally remain close to one another in save output for:

- conflict locality
- human comprehension
- replay locality/cache locality

For example:

- create an iv-module instance
- immediately follow with authored settings for that instance

Each serialized command object should have exactly two top-level fields:

- `command`
- `args`

If a command needs an id, that id should appear as a named field inside
`args`.

No generic cross-command metadata fields should be added by default.

Project-wide settings should be represented by one command at the beginning of
the command list rather than by a separate non-command header section.

Commands should be object-oriented:

- one command per authored object
- a lane connection is one authored object
- a lane view is one authored object
- an iv-module instance is one authored object

This means the default shape of a command should be a full object definition
rather than a tiny UI-gesture-like patch.

Paths should be stored relative to the project directory.

The persisted format should be JSON, one object per line.

Reasons:

- simpler dependency story
- no extra parser work for now
- still easy enough to diff and hand-edit in line-oriented command form

Object references should be represented only as UUID strings inside command
args.

## Serialization policy for module-managed lanes

`Timeline` should not blindly serialize every lane visible in its structural
substrate.

For any lane family not truly authored and managed by timeline itself, the
managing app module fully chooses serialization policy.

This applies to:

- graph-input-derived lanes
- audio-device-managed lanes
- visualization lanes
- any other system-managed or module-managed lanes

The principle is:

- serialize authored policy owned by the managing module
- do not serialize purely derived lane records just because they exist inside
  the timeline substrate

One important consequence is that lane connectivity becomes first-class authored
state over partly derived lane state.

In current code and in the intended direction:

- which lanes exist is often owned by lane-authoring/managing modules
- which lanes connect is owned by the timeline substrate

Therefore:

- any lane that may participate in saved authored connectivity must have a
  stable identity
- that stable identity must be chosen and preserved by the module that owns the
  lane's existence/policy
- timeline connectivity should be serialized against those stable lane ids

So even when a lane body is derived, its stable lane identity may still be part
of authored persisted state if authored connectivity or authored views need to
refer to it reliably.

For graph-input-derived lanes specifically:

- store graph-input port state/policy
- do not store the derived synthetic timeline lanes directly

This same rule should apply generally to any non-timeline-managed lane family.

## Authored state inventory

Current intended authored state includes:

- iv-module instances
- lanes from managing modules
  - currently most concretely `AudioDeviceLanes`
- iv-module instance port/input/output policy states
  - such as disconnected, logical follow, overridden, timeline lane, and
    similar policy values
- lane views and the query associated with each
- any setting related to any of the above

Lane filters do not need to be stored separately if the query already is the
filter model.

## Concrete per-module persistence proposal

This section is the current best-shot proposal for what project persistence
should save by app module.

### `IvModuleInstances`

Persist:

- stable instance id
- module root / definition reference
- all per-instance authored settings
- any future per-instance setting that affects materialization

Do not persist:

- realized builder
- live loaded module id
- introspection snapshot
- realized module refs

### `GraphInputLanes`

Persist:

- authored input policy state per relevant port
- authored output policy state per relevant port
- authored sample override values
- any authored setting that controls disconnected / logical-follow /
  overridden / timeline-lane style behavior
- stable lane ids for any graph-input-managed lanes that must be referencable
  by saved connectivity or saved views

Do not persist:

- fully derived lane records that can be regenerated from authored policy
- runtime-only live values that are not authored overrides

### `AudioDeviceLanes`

Persist:

- authored project value for selected input/output device as `explicit |
  default`
- stable ids for audio-device-managed lanes if those lanes may participate in
  saved connectivity or saved views
- any authored settings for those lanes

Do not persist:

- current device availability snapshot
- runtime stream state
- synchronizer/resampler buffers and live device state

### `Timeline`

Persist:

- authored lane connectivity
- any truly timeline-owned settings that are not owned by another managing
  module

Do not persist:

- lane existence for module-managed lanes
- execution caches
- task graph state

Important property:

- timeline connectivity should be serialized by stable lane ids, not by
  transient allocation order

### `LaneViews`

Persist:

- stable view id
- query string
- viewport/window/order/layout state needed to restore the project UI
  meaningfully
- any additional authored view settings later considered part of reopening the
  project "as it was"

Do not persist:

- resolved query results
- transient dirty/runtime state

Important implementation note:

- the actual 2D editor/view layout is VS Code-facing UI state
- this should be explicitly represented as structure, not inferred from list
  ordering
- restoring that layout belongs with the app/UI module on project load
- this work can be deferred, but it should not be treated as solved by command
  ordering alone

### `LaneFilters`

Persist:

- nothing separately when the query already is the filter model

Do not persist:

- bound/compiled query state
- cached filter evaluation state

### `LanesVisualization`

Persist:

- nothing

Do not persist:

- visualization helper lanes
- visualization payloads
- refresh/publishing runtime state

### `IvModuleDefinitions`

Persist:

- nothing directly

Do not persist:

- live loaded definitions
- canonical builders
- status or message notifications

### `IvModuleReload`

Persist:

- nothing

Do not persist:

- watcher state
- reload attempts/results
- dependency tracking state

### `IvModuleSourceIntrospection`

Persist:

- nothing

Do not persist:

- introspection indexes
- query-facing cached snapshots

### `TimelineExecution`

Persist:

- the project value for `compiled_sample_cache_chunk_size_multiplier` as
  `explicit | default`, if this setting remains useful

Do not persist:

- compiled support ranges
- compiled sample caches
- compiled event caches
- runtime execution memory/state

### `TaskRunner`

Persist:

- nothing

Do not persist:

- declared task graph
- compiled execution plan

### Project/execution/build settings

Persist:

- project-owned build or execution settings
- project overrides of installation defaults

Do not persist:

- resolved installation values as though they were explicit project values

## Save builder model

App modules should not emit commands directly while saving.

Instead:

- the persistence module owns a structured save-state builder
- each contributor updates only its fixed subset of that structured builder
  state
- the builder lowers the structured save state into the final normalized
  command list when output is built

This keeps module save contributions structured and local while keeping the
serialized file flat and stable.

The structured builder state will likely need slices for:

- project settings
- iv-module instances
- graph-input policy objects
- module-managed lane objects
- timeline connectivity objects
- lane view objects

## `project.overrideSettings`

`project.overrideSettings` is allowed to parse known keys before forwarding the
override request.

Desired behavior:

- recognized keys should be parsed explicitly
- recognized valid keys should still be applied even if some sibling keys are
  unknown
- unknown keys should not prevent recognized settings from being applied
- unknown keys should not be silently ignored
- unknown keys should at least emit a diagnostic/log message
- after that parsing/logging step, the original structured args object should
  still be forwarded so owning modules can pull the fields they care about

This keeps the command surface practical while still making unexpected data
visible.

## Lane views

Lane views should be stored in the project file.

Reason:

- closing and reopening the same project should preserve the UI layout

This is not currently being treated as a collaboration-heavy application, so
project-owned UI layout is acceptable for now.

If later needed, this can evolve toward named layouts or swappable UI layouts
without requiring a separate UI-only config file immediately.

## Iv-module instances

`IvModuleInstances` should be audited before project serialization work begins.

The important question is whether its desired/authored side is already the right
home for all per-instance saved state, or whether that structure must first be
expanded.

Current code already has a promising desired/realized split:

- desired instance data
- realized loaded instance data
- realized builders and module refs

But persistence should only depend on the authored side.

So the design requirement is:

- confirm or refine `IvModuleInstances` as the canonical owner of persisted
  per-instance authored state before implementing persistence

## IDs

Persisted authored objects should use stable IDs that do not change once picked.

UUIDs are a good fit for this.

Recommended direction:

- assign a UUID when a persisted resource is created
- preserve it forever across save/load cycles
- reuse it on subsequent saves
- use raw UUID strings in persisted command args

Important property:

- save normalization must never renumber or compact IDs

Recommended direction:

- use UUIDs for persisted authored object creation
- preserve them forever
- reuse them on subsequent saves

This is especially important for:

- iv-module instances
- lane views
- any module-managed lanes that may participate in saved connectivity or saved
  views

## What should not be stored

The project file should not store:

- live loaded iv-module definitions
- canonical builders
- realized per-instance builders
- task graphs and task dependencies
- compiled sample caches
- visualization lane content or snapshots
- lane visualization helper lanes
- device enumeration snapshots
- reload/watcher state
- any other fully derived runtime state

## Replay diagnostics

Project replay should stay simple:

- run commands in file order
- log failures when they happen
- keep going with later commands

We do not currently need a structured `applied` / `failed` outcome model, an
explicit dependency graph, or a cause-annotation layer.

The useful diagnostic source is the real exception text produced by command
execution in file order.

## Current and future settings

A setting belongs in the shared modification/persistence model if:

- it mutates authored state
- it meaningfully reproduces project behavior or layout
- it is not fully derived

Examples already discussed:

- iv-module instance settings
- graph-input policy values
- lane-view state and query
- module-managed lane settings
- project-overridable execution settings

Future DSP compiled-resource bindings are acknowledged as a real area, but are
explicitly deferred for now.

## Recommended design sequence

1. Define config-file responsibilities clearly:
   installation defaults, project file, iv-module-local file.
2. Inventory all manipulable authored data sources in the repo.
3. Define the stable shared modification command surface.
4. Audit authored-state owners, especially `IvModuleInstances`.
5. Define serialization ownership policy per managing module.
6. Define persisted ID policy.
7. Define replay diagnostics and best-effort failure semantics.
8. Define normalized one-command-per-line project file semantics.
9. Only after that, settle the concrete JSON command schema and
   implementation.
