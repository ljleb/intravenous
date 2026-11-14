# Knob reset before the double-click decision

Historical context, preserved 2026-11-14 from the working-tree versions of
`docs/node_presentation_and_manual_controls_direction.md` and
`docs/vue_client_implementation_stages.md`. The owner replaced Hover + R
with double-click reset and requested height-dependent inline ports.
The current contracts are in
[Node Presentation and Manual Controls](../node_presentation_and_manual_controls_direction.md)
and [Vue Client Implementation Stages](../vue_client_implementation_stages.md).

## Previous reset contract

Right-click opens a numeric-value popover. Typing remains local until Enter
accepts one exact-value gesture. Escape, clicking outside, or leaving the
editor cancels the draft. Numeric entry bypasses drag snapping. Hovering a knob
and pressing R resets it to the authored default, provided the graph has keyboard
focus and no text field is being edited. Arrow keys adjust a focused knob.


## Previous accepted UI plan

Right-click opens numeric entry (Enter accepts, Escape cancels). Hover + R resets
to the declared input default while the graph has keyboard focus and no text
field is being edited. A compact VS Code status item exposes the current context's
shortcuts and drag sensitivity, with fuller help in its tooltip.


## Earlier implementation status

- Hover + R requires keyboard focus in the graph and does not intercept text
  input or Ctrl/Meta/Alt shortcuts. The extension displays contextual shortcuts
  and drag sensitivity in a status item, clearing it on focus loss or disposal.

