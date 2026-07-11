# Lane Filters Runtime Flow

- `Timeline` owns only base lane facts:
  - lane ids
  - base lane metadata
  - base schema
- filter concerns stay out of `Timeline`
- `LaneFilters` owns:
  - named filters
  - filter dependency ordering
  - derived filter membership such as `filter.<name>`
  - current per-filter result state
- `filter.*` is query-language-level symbolic state, not timeline metadata written back into lanes

## Control Flow

1. `Timeline` emits base lane metadata/schema changes
2. `LaneFilters` refreshes filters in dependency order
3. each filter result becomes either:
   - snapshot
   - error
4. `LaneFilters` emits completed filter results
5. `LaneViews` consumes those results and updates its own view state
6. JSON-RPC forwards lane-view updates/errors to UI

## Important Rules

- no `LaneViews -> LaneFilters` callback control flow
- no filter metadata writeback into `Timeline`
- filter names are conventional dotted names, for example:
  - `graph_input.default`
  - `lane_view.<id>`
- cyclic filter references are errors
- filter evaluation errors are first-class runtime state and must propagate downstream
