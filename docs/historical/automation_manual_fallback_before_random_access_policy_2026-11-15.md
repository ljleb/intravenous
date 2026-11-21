# Automation fallback and point choices before the access-mode correction

_Historical, non-normative. Preserved on 2026-11-15 before applying the owner's
reply to the automation implementation questions._

The fallback wording below came from commit `d2e1d0ec` in
`docs/automation_and_suspendable_coverage_direction.md`, section 4. The other
excerpts came from the same normative document and the in-progress migration
plan. The owner subsequently selected coincident-point jumps, confirmed
Bézier tension 1 by default, and changed the access-mode rule: Region/Coverage
destinations default to manual fallback and may override it; Sequential
uncovered-value behavior stays neutral and cannot opt into that policy.

Replacement: [Cyclic automation](../automation_and_suspendable_coverage_direction.md),
sections 1, 2 and 4, and the accepted-decision checkpoint in the
[migration plan](../automation_and_background_migration_plan.md).

## Previous fallback contract

## 4. Uncovered automation uses the destination's manual value

`empty` outgoing segments are **uncovered**, not covered with a zero sample
and not an interpolation toward a fallback value. An eligible destination
Sequential input selects the automation result at positions where the
connected automation covers the position, and otherwise selects the input's
existing persisted/live manual knob value. This is a selection policy, not
additive fan-in and not the existing temporary manual-gesture override.
The manual value remains independently editable while automation is connected.
The ordinary Sequential `neutral_value` rule remains applicable to other
inputs; it is not silently redefined for every connection. Fallback occurs
only on **known uncovered** positions, never merely because a background
coverage query is still pending.

## Previously open payload choices

Duplicate timestamps and a specific tension serialization convention require
an explicit implementation decision; they must not silently change the
half-open segment/coverage rules.

A single segment tension controls
the interpolation family, including a value giving **exact linear**
interpolation. The precise tangent estimator, its behavior at adjacent
`hold`/`empty` segments, and the tension-to-shape mapping remain to be settled
as part of implementation; an illustrative smoothstep or Hermite formula is
not itself the normative formula.

## Earlier implementation recommendations, not accepted decisions

### Step 3 decisions awaiting the owner

The tension convention above is settled. Two other user-visible choices remain
open, and implementation pauses before freezing them:

- At a shared sample index, preserve an instantaneous jump (the first point
  supplies the incoming endpoint and the last supplies the outgoing segment),
  or reject coincident points. Preserving the jump is the current recommendation;
  it has not been selected by the owner.
- Enable manual fallback through the destination input declaration, with the
  existing neutral-value behavior as the default, or through a project setting
  on each connection. The destination declaration is the current recommendation;
  it has not been selected by the owner. The normative requirement remains that
  eligibility is explicit and unrelated inputs retain their neutral-value rule.
