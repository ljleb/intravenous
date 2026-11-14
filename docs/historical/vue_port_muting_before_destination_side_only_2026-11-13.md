# Vue Port Muting Before Destination-Side Feedback

_Historical, non-normative. Preserved on 2026-11-13 from the uncommitted
port-feedback work. The owner clarified that same-direction ports should keep
their normal colour. Replaced by the port-feedback rule in
[Node Presentation And Manual Controls](../node_presentation_and_manual_controls_direction.md)
and the implementation status in
[Vue Client Implementation Stages](../vue_client_implementation_stages.md)._

The preceding design text said:

> Ports rejected by the current drop checks are muted during the drag, using a
> blend of their normal colour and the theme's disabled grey. Candidate ports
> retain their normal colour. A grouped icon is muted when none of its ports
> is a candidate. Filled and dashed connection states remain visible through
> the colour change. This feedback uses the existing direction/kind and
> reattachment checks; it does not add converter discovery.

The corresponding implementation status said that `NodePorts` marked all
unavailable handles, rows, and groups with `.unavailable`. The manual check
required output groups to look muted while dragging a new connection from
another output. The revised rule limits that feedback to the destination side.

The preceding first-press status also said:

> `GraphEditor.startConnection` now captures source geometry, focuses the editor,
> and takes pointer capture before assigning `connectionDrag`. A synchronous
> focus-side blur therefore cannot cancel a newly assigned drag. Blur handling
> also ignores an event if the document currently has focus, and lost-capture
> handling ignores an event if the editor still owns that pointer. The owner's
> reported first-click failure after activating the webview still needs a manual
> check; no host-level input delivery behaviour has been reproduced here.

The owner subsequently confirmed that the failure remained and that it occurred
when returning from another application or native window. The revised status
records this failed fix and the opt-in input recorder, without claiming a fix.
