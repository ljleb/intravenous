# Automation knobs before independent bounds

_Historical. Preserved on 2026-11-19 after the owner clarified that the
manual bounds can cross and that the sampled output uses their midpoint._

The previous [Vue presentation contract](../vue_presentation_refinement_direction.md#explicit-value-bounds-and-lane-zoom-categories)
specified this editing behavior.

> An enabled minimum limits the maximum knob from below, and an
> enabled maximum limits the minimum knob from above. If enabling a knob would
> cross the opposite side, move the knob being enabled to the opposite value.

The previous [builtin automation guide](../../src/intravenous/builtin_packages/builtin/AUTOMATION.md)
described the same enable behavior.

> If enabling one knob would cross the other, it moves to the opposite value.

Both current documents describe independent knobs and midpoint output.
