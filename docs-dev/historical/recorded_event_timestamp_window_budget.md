# Recorded event timestamp windows before exact viewport reads

The previous event timestamp observation contract in
[`project_presentation_plugins.md`](../project_presentation_plugins.md#frame-and-input-contract)
used a point budget. The exact viewport contract replaces it.

> `points` limits the number of events returned.
> When the result is truncated, it contains the earliest events up to that
> limit. Panning or zooming can narrow the query.
