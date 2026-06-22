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
- optionally coordinates more than one module when needed

### Project persistence module

A dedicated persistence module:

- loads the project file
- saves the project file
- owns normalized reconstruction output
- asks modules to contribute commands into a command-list builder
- replays loaded commands only through the shared modification module

This module must not become a second mutation API.

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

## Current `.intravenous` reality

Current code does not appear to use module-local `.intravenous` files in any
active way.

Instead, the runtime currently treats workspace-root `.intravenous` as the
project config file and required marker.

The active runtime parser currently recognizes these keys:

Toolchain/build keys:

- `c_compiler`
- `cxx_compiler`
- `cmake_program`
- `cmake_generator`
- `make_program`
- `juce_dir`

Execution keys:

- `sample_rate`
- `block_size`
- `compiled_sample_cache_chunk_size_multiplier`

This means that today:

- workspace `.intravenous` is really a project config file
- installation `.intravenous_defaults` is really an installation defaults file
- module-local `.intravenous` is not currently carrying meaningful active state

There is also at least one stale tooling path worth remembering:

- `scripts/generate_intravenous_defaults.sh` currently writes camelCase keys
  while the runtime parser expects snake_case keys

That should be cleaned up separately later.

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

Optional readability improvement:

- include a human-readable type prefix around the UUID if useful

Examples:

- `instance:<uuid>`
- `view:<uuid>`

Important property:

- save normalization must never renumber or compact IDs

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
7. Define normalized one-command-per-line project file semantics.
8. Only after that, settle the concrete YAML schema and implementation.

