# Concrete Member Output Gain Switch

This rule previously appeared in
[Node Presentation And Manual Controls](../node_presentation_and_manual_controls_direction.md#persistent-manual-controls).
On 2026-11-20, the owner removed the remaining output enable switches.

> The same knob components serve the ports of C++-constructed nodes. Eligible
> virtual inputs and concrete member knobs start disabled. Virtual output gains
> are always enabled. GraphJit omits an unused virtual output gain when the
> output has no consumer, no live observation, and no active gesture.
>
> A virtual knob stores one value and applies it to the ports in its mapping.
> An enabled concrete member knob takes precedence at its port.

The [current rules](../node_presentation_and_manual_controls_direction.md#persistent-manual-controls)
use unity gain to let the virtual output gain apply at a concrete member.
