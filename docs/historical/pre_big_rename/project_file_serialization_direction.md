# Project File Serialization Direction

_Status: current persistence direction for the generalized `ProjectGraph`._

Canonical project intent is split by responsibility: `NodeInstances` owns the
desired node-instance set, `GraphConnections` owns the desired connection set,
and `ProjectGraph` coordinates reconstruction transactions without duplicating
either. `ProjectPersistence` is the serializer/replayer. See
[project_graph_application_architecture.md](./project_graph_application_architecture.md)
and [event_flows/startup_and_project_replay.md](./event_flows/startup_and_project_replay.md).

## Persistent state versus derived state

Project persistence stores configured user intent, not runtime object graphs and
not execution-derived state.

For the project graph, persist at least:

- stable project node-instance ids;
- node definition ids;
- the exact C++ configuration argument-list source used to request each node;
- project-wide connection declarations;
- structured `ProjectNodePortMatcher`s for every connection side;
- explicit source/output and target/input channel type information required by
  sample connections;
- user-owned graph metadata as it becomes part of the canonical model.

Other app modules may contribute their own independently owned persistent state
through the persistence collection protocol.

Do **not** persist derived graph/execution state such as:

- `NodeInstances` configured cache entries;
- compiled configuration-expression thunks/globals;
- definition snapshots or package provider callbacks;
- `GraphBuilder`/`BuilderSession` objects;
- local-to-parent embedding maps;
- concrete builder-local node/port/channel handles;
- resolved connection channel ids;
- `GraphJit` compiled kernels/ORC resources or `GraphExecutor` pending/active runtime state;
- volatile physical audio-device objects/bindings.

## Node configuration expressions

The initial generic node-configuration persistence format stores one C++
argument-list source string verbatim, for example:

```text
OscillatorConfig{.frequency = 440.0f}, 0.25f
```

Do not parse or normalize it by splitting on commas. It is C++ source and may
contain nested comma expressions/arguments. `NodeInstances` later asks the
configuration compiler to parse/type-check it in the definition provider's
translation-unit context.

A future structured UI may generate this string from forms and controls, but
the stored semantic request can remain the same.

## Structured project connection matchers

Persistent connections should store structured matcher fields rather than make
a dotted debug string the canonical representation.

A `ProjectNodePortMatcher` contains conceptually:

- top-level stable project node-instance id;
- recursive node-path matcher;
- port matcher/name;
- optional concrete port-channel selector.

The node path must preserve selectors for nested virtual nodes, stable ordered
direct members, tiled node children, and recursive subgraph scopes.

Tiled child selection is a node-path operation. Port-channel selection is a
port operation. They must remain distinct in persistence.

A connection stores lists of output-side and input-side matchers because each
matcher is set-valued. Output matches are summed/contribute to the source;
input matches receive/broadcast the resulting source according to normal graph
connection semantics.

If a matcher currently resolves to zero nodes/ports, retain it as dangling
project state. Reload/startup must never silently delete or retarget it.

## Command model

The important ingress surface is the typed application command/event API, not
JSON-RPC itself. JSON-RPC is a transport adapter that parses/validates wire
messages and invokes typed requests.

Project file replay should reconstruct normalized app-module state through the
same semantic mutation surfaces used by live edits, but it does not need to
serialize the JSON-RPC wire protocol literally.

The currently chosen persisted format remains suitable:

- JSON;
- one command object per line;
- each command object has `command` and `args`.

As the project-graph surface is added, normalized reconstruction commands should
represent current desired state, for example:

```text
create/update this project node instance id with definition id + C++ arguments
remove this project node instance id
create/update this project connection matcher set
remove this project connection
```

The normalized file is not an append-only edit history.

## Replay batching

Even if the file is command-oriented, replay should collect project graph state
and apply it as a coherent batch where practical.

`ProjectPersistence` should not emit one downstream root rebuild for every line
of a loaded project. It should reconstruct the normalized project graph snapshot
and then invoke `ProjectGraph` once for that replay cause.

This is required both for startup efficiency and for the strict event
propagation tree/batching rule.

Other persistent domains should use their own batched replay procedures when
combining them into the same propagation would create a diamond that later
converges on one app module.

## Dedicated persistence responsibility

`ProjectPersistence` owns:

- loading/parsing the project file;
- replay orchestration into owning app modules;
- normalized save collection;
- atomic file writing.

It does not own canonical node/connection state. `ProjectGraph` does.

`ProjectAutosave` may coalesce mutations and request a save after project load.
Project replay itself must not immediately schedule a save merely because it is
reconstructing state.

## Save policy

Ordinary configured mutations should be coalesced with a short debounce and
saved atomically rather than writing after every UI gesture.

A save collection procedure should ask each owning app module for its normalized
persistent contribution. `ProjectGraph` contributes its current project-owned
node-instance declarations and connection declarations; it must not expose
`NodeInstances` caches, `GraphJit` compiled generations, or `GraphExecutor` runtime state as persistence.
