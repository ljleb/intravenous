# Lane Layout Before Fixed Vertical Zoom And Source Selection

Preserved on 2026-11-12 from
`docs/node_presentation_and_manual_controls_direction.md` and
`docs/vue_client_implementation_stages.md`. Replaced by the owner's clarification
that viewport resizing preserves vertical zoom, that a default lane does not
nest the old per-port display, and that source-selection filtering remains needed.
The replacement is in
[Node Presentation And Manual Controls Direction](../node_presentation_and_manual_controls_direction.md#lanes-and-reusable-timeline-display).

The previous normative text incorrectly prohibited filtering:

> Groups replace filtering as the way to organize the visible graph: the editor
> shows the scope tree unfiltered.

The initial layout helper divided the available height among children:

```ts
export function defaultLaneHeight(availableHeight: number, childCount: number): number {
    return Math.min(320, Math.max(48, Math.floor(Math.max(0, availableHeight) / Math.max(1, childCount))));
}
```

This caused viewport resizing and filtering to change lane heights. The current
initial vertical zoom is independent of viewport size and child count. Overflow
uses a vertical scrollbar.

The previous implementation status said:

> The generic lane shows each port separately; it does not create one synthetic
> timeline or meter for the node's combined outputs.

The underlying data remains separate per port, but the copied `PortRow` lists
looked like lanes inside each node lane. The default frame now uses grouped port
metadata in its left area. The source-inspection panel retains its old rendering
until it is replaced with a filtered host of the shared lane UI.
