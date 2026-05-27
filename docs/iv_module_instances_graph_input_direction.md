# Iv-Module Instances And Graph-Input Direction

This note captures the current intended direction for iv-module definitions,
iv-module instances, and graph-input lane ownership.

If context gets compacted later, this is one of the first notes to re-read.

## High-level ownership

- `IvModuleDefinitions` owns one canonical `GraphBuilder` per iv-module on disk.
- A definition rebuild produces one new canonical builder for each changed
  iv-module definition.
- Consumers receive those builders as `const` references during event dispatch.
- Consumers must copy a builder before mutating it.

`IvModuleDefinitions` should also compute the definition-level public interface
diff for each changed definition.

## Definition change events

`IvModuleDefinitions` should publish batched definition change events.

Created/updated definition entries should contain:

- definition identity
- module root
- canonical `GraphBuilder const&`
- definition-level public interface diff

Deleted definition entries should contain only the identity needed for cleanup.

The diff is per-definition, not per-instance.

## Instance module role

`IvModuleInstances` consumes definition changes.

Its job is to:

- copy canonical builders for the affected instances
- produce instance-ready refs for augmentation
- emit its own batched instance event
- after the instance event finishes firing, build the augmented builders
- store the built results in memory for execution

## Instance events

`IvModuleInstances` should publish batched instance events.

These events should still contain:

- created / updated / deleted entries
- associated definition identity
- pre-fetched node refs
- pre-fetched input/output refs
- stable input identifiers
- `GraphBuilder&` for augmentation of created/updated entries

The point of this event is to make it easy for subscribers to connect new things
to disconnected exposed inputs/outputs.

Private physical nodes that have no associated logical id should remain private.

## Graph-input-lanes role

`GraphInputLanes` is the right owner for DSP graph-input lane policy.

It should own:

- the DSP-produced lane subset we care about on iv-module instance reload
- logical knob state
- concrete override state
- parent/child knob inheritance behavior
- graph-input-lane-related lane queries and mutations

It should consume the instance event and use the provided refs plus
`GraphBuilder&` to augment graphs as needed.

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
