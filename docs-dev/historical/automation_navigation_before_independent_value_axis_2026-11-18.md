# Automation navigation before the independent value axis

Historical. Preserved on 2026-11-18 from the Vue presentation direction.
The owner changed the design so that min and max affect the output curve
and the editor has a separate value axis. See the
[current value display and panning rules](../vue_presentation_refinement_direction.md#value-display-and-panning-2026-11-18).

## Value display and panning

The Scale panel selects Curved or Cropped display. Curved keeps the original
mapping, including the arctangent where an end is unbounded. Cropped uses a
linear window that clips values outside it. Enabled bounds limit the window's
travel. A range narrower than the window fits in the lane. Display and panning
affect only the lane view.

In Curved display, zoom-out widens the arctangent scale when at least one
end is unbounded. A single finite bound stays at its lane edge. With two
finite bounds, zoom-out stops when the full range fits.

When signals supply bounds, observe the public `min` and `max` inputs over
the visible window. Draw their sampled values in the lane. Use the lowest
visible effective lower bound and highest visible effective upper bound to
limit panning. Show the authored curve and a preview of the clamped output.
The preview uses representative bound samples. Playback uses the exact bounds
at each output sample.

## Explicit value bounds and lane zoom categories

The Scale panel controls display and snapping. With two distinct finite
limits, map the value height linearly between them. With one or no limit, use
the curved mapping. If the limits are equal, leave room around the value so
the point remains editable.

Shift and middle-drag pans the value scale when the view has room to move.
The gesture also pans the timeline horizontally. The cursor wraps inside
the visible lane content to the right of the header. With neither bound,
move the center of the arctangent scale. With one bound, keep that edge
fixed. Shift and a double middle click restores the default mapping.

## Acceptance checks

Test all four bound configurations for the automation value axis, scaled
read-only curve paths in `nano` and `mini`, and editable points/handles in
`editable`.
