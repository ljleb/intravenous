# Iv-Module Instance Management Direction

This note captures the next intended direction for iv-module instance
management. It is a forward-looking checkpoint, not an implementation claim.

If context gets compacted later, this is a useful companion to
[iv_module_instances_graph_input_direction.md](./iv_module_instances_graph_input_direction.md).

## Goal

`IvModuleInstances` should eventually become the owner of user-managed iv-module
instances, not just a passive consumer of definition reloads.

That means the module should later support explicit requests to:

- create an instance from a definition
- delete an existing instance
- rebuild affected instances when a definition changes

## UI Direction

The VS Code UI should eventually gain a webview for managing iv-module
instances.

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
- building augmented instance graphs after instance events finish firing

Multiple instances may share the same definition. Downstream consumers should
not need duplicated definition-level diff computation for each one.

## Relationship To Lanes

Lanes should eventually be filterable by iv-module instance so the UI can show
only the lanes related to one chosen instance.

This means the lane-related modules will later need enough metadata to answer
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

Tests written now should be easy to migrate toward this future.

That means:

- instance tests should focus on owned instance lifecycle and emitted diffs
- bridge tests should focus on definition -> instance and instance -> downstream
  relationships
- integration tests should verify that instance changes propagate across the
  application

The current test rewrite should preserve behavioral intent while avoiding
hard-coding the temporary one-instance-per-definition shape more deeply than
necessary.
