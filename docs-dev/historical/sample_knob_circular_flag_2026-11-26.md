# Sample knob circular flag

This section from `node_presentation_and_manual_controls_direction.md` recorded
the original circular-knob declaration. The declaration now uses `KnobGeometry`.
[Current declaration and behavior](../node_presentation_and_manual_controls_direction.md#circular-sample-input-knobs)

#### Circular Sample input knobs

_Accepted 2026-11-26._ A Sequential Sample input can declare `.circular = true`
with finite `.min` and `.max`, where `min < max`. Set the flag on the module
input to expose a circular knob on the module lane. The knob draws one full
turn with a shared endpoint. Scrubbing, keyboard steps, numeric entry, and
paste produce knob values in `[min, max)`. The project stores that value and
discards the turn count.

The manual-control ramp follows the shortest arc and emits values in the same
half-open interval. An exact half-turn moves toward increasing values. The
range describes the knob and its manual contribution. Cable values and the
input's combination function keep their ordinary semantics.
