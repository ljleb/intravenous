# Lane Query Tooling Direction

Lane-query evaluation and lane-query editing are related but distinct concerns.
The existing tokenizer, parser, binder, and evaluator remain authoritative for
committed queries. Editor tooling must also work for incomplete text at an
arbitrary cursor position.

## Global schema service

The lane-query schema is project-global derived state. It is the union of
queryable metadata keys across all timeline lanes, with one of the value types
`unit`, `int`, or `float` for each key. It is not scoped to the lanes currently
matched by a particular lane view.

A lane that is filtered out of every open view can still introduce the first
instance of a metadata key, remove its last instance, or change its type. Those
events change the available query completions. Conversely, ordinary lane/value
changes that leave the schema revision unchanged do not require completion UI
updates.

The runtime already computes a revisioned `LaneQuerySchema` and
`LaneQuerySchemaChange` while forwarding timeline lane changes to
`LaneFilters`. The socket/UI boundary must publish this independently of lane
view result updates:

- a read-only RPC obtains the current schema snapshot and revision;
- a dedicated JSON-RPC notification announces a later schema revision and its
  added, removed, or retyped entries;
- the extension host owns the revisioned cache and forwards it to webviews.

The precise wire route may be chosen with the implementation, but it must not
depend on a lane view matching a changed lane and must recover from a missed
notification by requesting a fresh snapshot.

## Cursor-aware tooling API

Extend the query library with a non-mutating editor-tooling API alongside the
ordinary parser. Its input is source text, cursor offset, and a schema snapshot;
its output includes a replacement range, completion context, candidates, and
optional ranged diagnostics.

The tokenizer must retain source ranges for tokens. The normal parser can keep
its strict committed-query behavior, but completion must use tolerant prefix
analysis rather than attempting to recover from a failed complete parse. It
must handle partial input such as:

```text
dsp_gr|
dsp_graph.|
foo = |
(foo |
foo = 12..|
```

Initial contexts are property/key segments, values, projection selectors,
operators, and grouping syntax. Candidate values are constrained by the schema:

- property and selector candidates carry their key and value type;
- `unit` is offered only for unit properties;
- numeric literals and ranges are offered only for numeric properties;
- grammar tokens are offered only where their surrounding syntax permits them.

Named filter references can be added when they become an intentional
user-addressable part of the language.

## UI behavior

The webview presents a compact keyboard-accessible suggestion list, but does
not own query semantics. Selecting a suggestion replaces exactly the reported
source range. Navigating suggestions must not submit a query; ordinary query
submission remains the existing debounced/explicit-edit path.

Schema revisions invalidate completion and diagnostic caches. The UI should
make a committed-query failure visible without preventing further edits or a
later query update.
