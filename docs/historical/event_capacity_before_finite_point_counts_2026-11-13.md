# Event capacity before finite point counts

Non-normative historical text preserved from section 15 of
`docs/coverage_and_background_evaluation.md` on 2026-11-13. The owner accepted
finite counts declared through port constraints while discussing sparse automation
points. The replacement is in [the active design](../coverage_and_background_evaluation.md).

`EventOutputProperties::max_events_per_index` remains density/capacity metadata, not
a literal per-timestamp quota. For one selected range:

```text
required_event_entries = ceil(max_events_per_index * range.extent)
```

For a generated live event representation spanning `W` positions, the analogous
capacity uses the appropriate compiler-known effective fan-in bound over `W`.
When several event connections are combined into one buffer, GraphJit must account
for all incoming effective capacity bounds so every legal incoming sequence fits
without audio-thread allocation.

