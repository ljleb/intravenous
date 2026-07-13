# Authored Lane Creation Direction

This note records the remaining work for user-created timeline lanes. It
builds on `lane_ui_model_direction.md`.

## Ownership boundary

`Timeline` remains a generic structural graph owner. It must not contain
lane-type-specific creation, persistence, execution, or presentation logic.

A separate authored-lanes app module owns user-created lane instances. It:

- owns stable authored lane ids and their canonical serialized state;
- knows which lane types are supported for creation;
- turns generic creation/reload requests into `TimelineLaneBatchUpdate`s;
- persists generic authored-lane records;
- forwards normal changed-lane invalidation after state edits.

`RuntimeProject` remains the transport-independent mutation ingress. JSON-RPC
and project-file replay both use that ingress; it forwards requests to the
authored-lanes owner.

## Compile-time creatable lane contract

The authored-lanes module has a compile-time list of supported lane C++ types.
It contains only the types, not duplicated descriptors. A type in this list
must satisfy a creatable-lane concept or compilation fails.

Required static members are conceptually:

```cpp
static constexpr std::string_view lane_model_type_id();
static constexpr std::string_view lane_creation_category();
static constexpr std::string_view lane_creation_label();
static constexpr std::string_view lane_creation_description();
static std::string default_lane_ui_state();
static TypeErasedLaneNode from_lane_ui_state(
    std::string_view serialized_state,
    LaneCreationContext const&);
```

The existing instance UI-state hooks remain responsible for state snapshots and
state application after creation. The type contract supplies the initial state
and reconstruction factory.

The initial supported type is `BeatTriggerLaneNode`.

## Generic server surface

The UI creation command requires:

```text
timeline.laneTypes
timeline.createLane { typeId }
```

`timeline.laneTypes` returns type id plus category/label/description derived
from the registered C++ type traits. The client must not maintain a duplicate
catalog.

`timeline.createLane` accepts only a type id. The authored-lanes module uses
the type's default serialized state, allocates a stable lane/public id, stores
the authored record, and produces a generic timeline upsert.

Later generic operations may include delete, duplicate, and move/reorder;
they must remain type-independent at the transport boundary.

## Persistence

Project persistence stores generic authored-lane records:

- stable authored/public lane id;
- lane model type id;
- canonical serialized state;
- eventual layout/order data belongs to lane-view layout persistence, not the
  lane model itself.

On load, the authored-lanes module resolves the type id through its registry
and reconstructs the node with `from_lane_ui_state`. Unknown types should be
reported clearly and left unapplied rather than silently reinterpreted.

## Frontend creation UI

The VS Code command is `Intravenous: Create Lane`.

It presents a single Quick Pick with category separators, never a deeper menu
hierarchy. It must obtain entries from `timeline.laneTypes`, then call
`timeline.createLane`. The temporary frontend catalog used while developing
the command is to be removed once the server query exists.

## Beat-trigger vertical slice

The beat trigger is a compiled trigger-event lane. Its lane plugin:

- consumes its existing compiled event-window visualization output;
- draws markers/grid only from those compiled event timestamps, never from a
  separate frontend tempo calculation;
- overlays tempo, time-signature, and events-per-beat controls;
- emits serialized state writes through `timeline.setLaneUiState`;
- uses the common playback seek route when its ruler/grid is clicked or
  dragged.

Every beat lane shares the global sample playhead and the common horizontal
viewport. No beat lane is a special global owner: any visible beat lane may
draw its own ruler/grid and scrub the same transport.

Once beat-lane rulers are available, the generic top sample ruler should be
removed; it remains only as a fallback while no beat lane is visible.

## Required tests

Light tests should cover:

- creatable-lane compile-time contract and registry listing;
- generic creation creates an addressable timeline/public lane;
- default state, type id, and compiled output are correct;
- state edits invalidate compiled output and publish revised state;
- persistence/reload reconstructs the same authored lane and state;
- unknown type handling is explicit;
- RPC list/create/state-write routing;
- compiled event windows drive beat marker positions exactly.
