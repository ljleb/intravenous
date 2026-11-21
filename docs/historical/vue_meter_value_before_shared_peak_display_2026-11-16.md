# Meter Value Mode Before the Shared Peak/RMS Display, 2026-11-16

_Status: historical, not normative. Preserved before applying the owner's
follow-up request that both meter modes show the same readings, with only the
scale changing and a held peak marker that accelerates downward._

Replacement: [Vue meter direction](../vue_presentation_refinement_direction.md#1-channel-correct-meters-and-two-display-modes)
and [output observation](../vue_output_observation_direction.md#multichannel-sample-observation-and-meter-display-owner-refinement-2026-11-16).

## Previous value-mode requirement

_Source: `docs/vue_presentation_refinement_direction.md`, section 1,
2026-11-16 before the follow-up._

Add a Vue meter **`value` display mode** using a linear, untransformed sample
value scale: for a unit 0-to-1 scale, `0.5` is exactly halfway up. The display
must not apply dB, logarithmic, or nonlinear magnitude compression. The
underlying signed sample value must not be silently replaced by its magnitude;
the eventual signed/out-of-range visual convention can be established alongside
the control, without changing what `value` means. If displaying a latest
sample requires a different registered observation operation from block
`level`, request that operation explicitly rather than relabeling a peak/RMS
reduction as a sample value.


## Previous operation-selection guidance

_Source: `docs/vue_output_observation_direction.md`, multichannel meter section,
the same snapshot._

requires a green dBFS-graded **level** mode and a separate untransformed,
linear **value** mode; visual scaling belongs in Vue, not the DSP values.
If `value` needs a latest-sample operation rather than a block reduction,
request the appropriate registered operation explicitly. Reuse ruler-grade
visual presentation with square corners rather than inventing another
unrelated meter tick system.
