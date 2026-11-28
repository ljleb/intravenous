# Lane zoom before shared mode transitions

This note preserves the earlier header tuning entry from
`docs-dev/vue_ui_tuning.md` on 2026-11-18. The owner reported that the header
switched early and meter scale labels disappeared before automation became
read-only. See the [current tuning guide](../vue_ui_tuning.md).

> Header changes from stacked to inline. `NodeLane.vue`, `@container node-lane
> (max-height: 56px)`. Independent of the presentation mode.

The latest working-copy cutoff before this change was 73 px. Meter labels
used a separate cutoff of 23 px per channel.
