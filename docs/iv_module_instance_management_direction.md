# Iv-Module Instance Management Direction

This note captures the remaining intended direction for iv-module instance
management beyond the lifecycle and rebuild work already present in code.

If context gets compacted later, this is a useful companion to
[iv_module_instances_graph_input_direction.md](./iv_module_instances_graph_input_direction.md).

## Current state

`IvModuleInstances` already owns user-managed iv-module instances in the core
sense:

- create an instance from a definition/module root
- delete an existing instance
- rebuild affected instances when definitions or graph-input repatching require it
- remember one realized builder per instance

The remaining work is around persistence, richer per-instance authored state,
and broader UI affordances.

## UI Direction

The VS Code UI will likely grow a richer instance-management surface later, but
the ownership model should not depend on that UI shape.

The intended control flow is:

1. UI webview raises instance-management requests through JSON-RPC
2. JSON-RPC forwards those requests to `IvModuleInstances`
3. `IvModuleInstances` updates owned instances and emits batched instance
   changes
4. downstream modules react through their existing bridges

JSON-RPC should stay a transport adapter. It should not own instance state.

## Relationship To Definitions

`IvModuleDefinitions` owns canonical per-definition builders and
definition-level diffs.

`IvModuleInstances` should own:

- which instances currently exist
- which definition each instance is associated with
- copying canonical builders for each affected instance
- publishing per-instance builders for augmentation
- remembering per-instance rebuilt builders for execution

Multiple instances may share the same definition. Downstream consumers should
not need duplicated definition-level diff computation for each one.

## Relationship To Lanes

Lanes will likely need filtering by iv-module instance so the UI can show only
the lanes related to one chosen instance.

This means the lane-related modules will need enough metadata to answer
questions like:

- which lanes belong to this instance
- which logical node controls belong to this instance
- which connections cross instance boundaries

The lane view should remain a filtered projection over timeline-owned lanes.
The filter semantics can grow later without changing the basic ownership model:

- `Timeline` owns lane state
- `LaneViews` owns filtered views
- `GraphInputLanes` owns DSP graph-input lane policy
- `IvModuleInstances` owns instance identity and instance lifecycle

## Testing Direction

Tests should stay easy to migrate as the richer UI and persistence layers land.

That means:

- instance tests should focus on owned instance lifecycle and emitted diffs
- bridge tests should focus on definition -> instance and instance -> downstream
  relationships
- integration tests should verify that instance changes propagate across the
  application

The current test suite should preserve behavioral intent while avoiding
hard-coding the current UI shape or a temporary one-instance-per-definition
mental model more deeply than necessary.

## Remaining next steps

The most useful remaining work in this area is:

- persisting iv-module instances and their authored per-instance settings
- exposing richer instance-management UI controls
- connecting instance lifecycle to the future persistent-state app module

## Project Authoring UI Notes

Instance management should live in a separate command-opened panel rather than
cluttering the live graph inspector. The panel should list project instances,
their module roots, realization/build status, and authored per-instance
settings. It should support create, select, duplicate, delete, source reveal,
and retry/rebuild for failed instances.

Module discovery remains server-owned. The server should discover definitions
from the project-local `modules/` directory by default and from configured
shared source roots. The creation flow should make a project-local module easy
to create from the standard source template, then offer to instantiate it.

Duplication needs two explicit policies:

- ordinary duplicate copies the module reference and per-instance settings but
  resets port policy, output routing, and lane connections;
- duplicate with setup copies authored input values and port policies, creates
  fresh stable ids for owned lanes, and reproduces eligible lane connections.

Output routing should remain excluded by default. A later advanced flow may
offer inputs only, inputs plus routing, or the full patch explicitly.

When an editor focuses a module definition source file, the live graph sidebar
should switch to that definition's instances while preserving the selected
instance when it belongs to the definition. A definition with no instances
should offer the creation action rather than showing unrelated instance state.

The panel must only use the typed instance mutation surface. Project save
normalizes those mutations into `project.intravenous`; users should not need to
author that file directly outside normal version-control conflict resolution.
