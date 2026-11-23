# Automation fallback accessor proposals before resolved reads

_Historical, non-normative. Preserved on 2026-11-15 before applying the owner's
resolved-read decision._

Source: the in-progress automation direction, section 4, and migration plan.
The owner rejected both injecting fallback into supplied Regions and making
`uncovered_value(index)` the primary node interface. Replacement:
[automation direction, section 4](../automation_and_suspendable_coverage_direction.md#4-uncovered-value-policy-belongs-to-the-destination-input).

## Superseded open question

The access-mode rule above does not yet settle how fallback is exposed beside
whole logical Region views. Returning sparse Regions with a separate fallback
lookup and materializing a filled Region across a gap have different Region
and storage semantics. That interface decision must be resolved before the
fallback implementation proceeds; it must not silently change the requested-only
input invariant in section 6.

### Remaining interface question: fallback and Region views

The selected access-mode rule reaches an existing representation boundary.
`BackgroundEvaluationCallFrame::seal()` binds whole, explicitly requested
covered logical Regions. `Region` is contiguous storage plus its range; a
Coverage input can retain disjoint/overlapping Regions and duplicates.
`ConfiguredManualControl` and its current lowering support Tick Sequential
Sample inputs, not background Region/Coverage inputs.

For a Coverage input whose source supplies [0,10) and [20,30), reverse demand
for [5,25) currently selects the two whole source Regions and keeps [10,20)
uncovered. Manual fallback could be exposed through an explicit uncovered-value
lookup while keeping those Regions, or through newly materialized filled Regions
that bridge gaps. The latter changes Region enumeration, identity, contiguous
storage costs, and the requested-only contract. These are different interfaces,
not interchangeable storage optimizations. Implementation pauses for the owner
before selecting one. The accepted access-mode and point rules are recorded;
the fallback mechanism and revised automation payload have not been implemented.
