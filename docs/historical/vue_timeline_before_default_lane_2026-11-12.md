# Primary Timeline Before The Default Lane

Preserved from the primary timeline implementation and
`docs/vue_client_implementation_stages.md` on 2026-11-12. The replacement is the
reusable default-lane implementation described in
[Vue Client Implementation Stages](../vue_client_implementation_stages.md#stage-2-implementation-status).

The initial primary view rendered its label and `Not realized` text directly
inside `TimelineScopePresentation`. It also drew a shared grid behind every
child presentation. The owner clarified that a timeline is an optional display
feature of a presentation, and that each lane must handle arbitrary port counts.
The label and status frame therefore moved to `NodeLane`, and the unconditional
per-child time grid was removed. Failure display now uses a diagnostic-backed
red name; the frame no longer adds a status line.

The previous verification instruction was:

> Check a node list long enough to scroll vertically. Ruler ticks and row grid
> lines must remain aligned.

The shared ruler remains, but individual lane contents need not use a time grid.
