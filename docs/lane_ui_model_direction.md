# Lane UI Model Direction

This note defines the boundary between an executable lane model and one or
more user-interface presentations of that model.

## Core rule

The backend does **not** know about lane presentations.

It owns a lane's executable semantics, authored state, validation,
serialization, persistence, and optional stable model type identifier. A
frontend decides whether it recognizes that type identifier and how to present
it. A new frontend or a replacement presentation must be able to support an
existing lane type without changing backend code.

Lanes without a model type identifier continue to work normally and use a
generic presentation.

## Optional UI model state

A lane may opt into UI-editable authored state. Its generic backend-facing
snapshot is conceptually:

```cpp
struct LaneUiStateSnapshot {
    std::optional<std::string> type_id;
    std::uint64_t revision;
    std::string serialized_state;
};
```

`type_id` identifies a lane model such as `iv.timeline.beat-trigger`; it never
identifies a UI implementation. The serialized state is opaque to generic
runtime and RPC code. A concrete lane provides the conversions between its
typed state and the canonical serialized form.

The same canonical form is used for UI exchange and project persistence. It
may initially be JSON text, but generic code must not depend on that fact.

## Publication

Each lane decides when its UI state has changed and marks a cheap dirty flag.
The flag should normally be an atomic boolean. UI-state publication runs on a
UI-oriented application pass, never an audio/execution path:

1. the pass checks and clears a lane's dirty flag;
2. only a changed lane snapshots and serializes its state;
3. the result is folded into the ordinary lane-view update notification.

This avoids serializing unchanged settings every visualization frame.

Snapshotting and applying state still use the lane owner's normal
synchronization, so a published snapshot is coherent.

## Generic asynchronous editing

The client uses one generic model-state request, conceptually:

```text
timeline.setLaneUiState
```

with lane id, optional expected revision, and opaque serialized state. The
owning lane validates and deserializes into a candidate typed state, atomically
commits it if valid, increments its revision, marks UI state dirty, and
requests whatever executable invalidation its semantics require.

The authoritative state is delivered through the existing lane-view update
notification. UI implementations keep local drafts for immediate interaction;
continuous edits are coalesced and sent asynchronously. Revisions prevent
out-of-order requests from overwriting newer state.

The initial contract deliberately transmits full state rather than a generic
property dictionary or control-specific RPC methods. Future semantic actions
can be added if a lane needs them, without changing the presentation boundary.

## Frontend plugins

Plugins are frontend-only. A plugin matches an optional `type_id`, consumes
the lane's opaque serialized state, and emits generic state writes. It may
provide a compact control contribution and a track-layer contribution.

The lanes-view host owns visual cohesion: layout, sizing, typography, color
tokens, shared controls, hover/selection, pan/zoom, ruler, cursor, and common
accessibility behavior. Plugins contribute semantic content through host
primitives rather than unrestricted independent styling.

## Initial beat-trigger lane

The first UI-model lane is a compiled event source with defaults:

- type id: `iv.timeline.beat-trigger`
- 140 BPM
- 4/4
- `eventsPerBeat = 1`
- sample index 0 is beat 0

There is intentionally no project-global tempo. A beat-trigger lane is the
modular project tempo source and may be connected wherever timing is needed.
Its UI can render bar, beat, and subdivision grid lines from the exact same
typed settings used to generate its trigger events.

## Delivery order

1. Add the generic optional lane UI-state model, dirty publication plumbing,
   and serialized-state mutation request.
2. Add persistence support and focused light tests for the generic contract.
3. Add the beat-trigger lane's typed settings, compiled-event execution, and
   persistence tests.
4. Add the frontend plugin host primitives and beat-trigger presentation.
5. Add end-to-end RPC and lane-view tests, including asynchronous revision and
   reload behavior.
