# Iv-Module Instances And Graph-Input Direction

This note captures the current intended direction for iv-module definitions,
iv-module instances, and graph-input lane ownership.

If context gets compacted later, this is one of the first notes to re-read.

## High-level ownership

- `IvModuleDefinitions` owns one canonical `GraphBuilder` per iv-module on disk.
- A definition rebuild produces one new canonical builder for each changed
  iv-module definition.
- Consumers see those builders as `const` pointers/references during event
  dispatch and rebuild queries.
- `IvModuleInstances` performs the single per-instance copy it needs before
  mutation or completion.

`IvModuleDefinitions` should also compute the definition-level public interface
diff for each changed definition.

## Definition change events

`IvModuleDefinitions` should publish batched definition change events.

Created/updated definition entries should contain:

- definition identity
- module root
- canonical `GraphBuilder const*` or equivalent non-owning access
- definition-level public interface diff

Deleted definition entries should contain only the identity needed for cleanup.

The diff is per-definition, not per-instance.

## Instance module role

`IvModuleInstances` consumes definition changes.

Its job is to:

- copy canonical builders for the affected instances
- emit its own batched builder-change event
- let downstream modules complete those per-instance builders
- publish the completed per-instance builders to execution-side modules
- remember one realized builder per instance for later rebuilds and execution
- request a fresh canonical builder again when an instance needs structural
  repatching

## Instance events

`IvModuleInstances` should publish batched instance events.

Created/updated entries should contain:

- instance identity
- associated definition identity
- `GraphBuilder&` for augmentation of created/updated entries
- per-instance execution parameters that affect graph materialization

Deleted entries should contain only the identity needed for cleanup.

The point of this event is to let augmentation modules complete per-instance
builders without making `IvModuleInstances` own their policy.

## Graph-input-lanes role

`GraphInputLanes` is the right owner for DSP graph-input lane policy.

It should own:

- logical sample knob state
- concrete sample input state
- concrete event input state
- graph-input-lane-related lane queries and mutations
- deferred timeline lane structural batches for graph inputs
- deferred rebuild requests for affected iv-module instances

It should consume the instance builder-change event, inspect the logical graph
inputs exposed by the builder, and complete the builder according to its owned
desired state.

That includes:

- direct logical-follow sample control through shared live values
- timeline-lane completion for sample and event inputs when explicitly requested
- default behavior derived from the fresh canonical builder shape
- cross-domain prerequisite publication for timeline-involved inputs

## Timeline role

`Timeline` should not own logical-node-id-based graph-input reconciliation
policy.

`Timeline` should instead remain the generic lane substrate:

- generic lane storage
- lane CRUD application
- lane connections
- lane change events

Graph-input identity policy should live outside timeline.

## JSON-RPC note

JSON-RPC currently needs to fetch and manipulate logical node input knob state.

That state belongs with `GraphInputLanes`, not with
`ProjectService`.
