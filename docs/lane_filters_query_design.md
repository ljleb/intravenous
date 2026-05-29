# Lane Filters Query Design

- lane metadata properties are schema-typed: `unit | int | float`
- `Timeline` owns lane metadata and the current metadata schema
- `LaneFilters` does not own or mirror lane metadata; it evaluates queries through a read-only dataset view supplied by a bridge
- queries are parsed into a raw AST, then rebound against the latest schema when needed
- schema changes do not destroy filter definitions:
  - filter source / raw AST stays the same
  - bound queries are rebuilt from the raw AST against the new schema
- control flow:
  1. `Timeline` changes lanes / lane metadata
  2. `timeline -> lane_filters` bridge forwards the change, current dataset view, and schema change summary
  3. `LaneFilters` invalidates all compiled filters and rebinds on schema changes
  4. `LaneFilters` emits filter-changed
  5. `lane_filters -> lane_views` bridge forwards that
  6. `LaneViews` refreshes ordering/window state and emits view updates
  7. JSON-RPC forwards the view updates to the UI

## First implementation

- support the full prototype language, including projections
- no persistent cached bitmasks yet
- any lane metadata change invalidates all filter evaluations
- leave later selective invalidation / cached-mask work as a follow-up optimization
