# Lane Query Completion Plan

Query completion is editor tooling, not query evaluation.  The strict parser
remains authoritative for committed queries; completion uses tolerant,
cursor-aware analysis of the same token and grammar model.

1. Publish the project-global, revisioned `LaneQuerySchema` independently of
   lane-view results through a snapshot RPC and a change notification.
2. Extend query tokens with source ranges and add a pure C++ completion API
   taking source, cursor offset, and schema.  It returns the replacement
   range, context, candidates, and optional ranged diagnostics.
3. Use schema types to constrain candidates: property/selector keys, `unit`
   only for unit properties, and numeric literals/ranges only for numeric
   properties.
4. Add a completion RPC backed by that API.  The extension caches revisioned
   schema snapshots, refetches after a missed revision, and discards stale
   completion responses.
5. The webview renders keyboard-accessible suggestions and replaces exactly
   the returned range.  Suggestion navigation never submits a lane query.

All protocol offsets are UTF-8 byte offsets.  The webview must convert between
browser UTF-16 selection indices and UTF-8 offsets, even though the current
language syntax is ASCII-only.
