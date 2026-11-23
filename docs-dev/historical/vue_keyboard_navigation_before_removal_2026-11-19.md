# Earlier keyboard navigation and focus indicators

_Historical record, 2026-11-19. The owner asked to remove keyboard navigation
and focus outlines from the Intravenous webviews. See the
[current policy](../node_presentation_and_manual_controls_direction.md#webview-focus-and-keyboard-shortcuts)._

[Node presentation and manual controls](../node_presentation_and_manual_controls_direction.md#knob-gestures-and-discovery)
specified the following for knobs.

> Keyboard focus remains visible in the same color family.

[Vue presentation refinement](../vue_presentation_refinement_direction.md)
specified these rules for clipboard interaction, control points, and tooltip
replacement, respectively.

> Do not intercept plain `c`/`v` when a text field or other editor owns text
> input; preserve native OS shortcuts and accessible keyboard focus behavior.

> Preserve clear keyboard focus and unambiguous Shift+click/drag/context-menu targets.

> This is a visual-information policy, **not** a reduction of accessibility:
> retain semantic names, keyboard focus feedback, screen-reader labels and
> accessible descriptions independently of visual tooltip display.

[Vue client implementation stages](../vue_client_implementation_stages.md)
records the former lane and meter menu keyboard navigation.
