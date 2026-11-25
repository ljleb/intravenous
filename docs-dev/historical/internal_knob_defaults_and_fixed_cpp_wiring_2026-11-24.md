# Former internal knob defaults and fixed C++ wiring

_Superseded on 2026-11-24. Preserved from [Node Presentation and Manual Controls](../node_presentation_and_manual_controls_direction.md) when the owner limited default enablement to project-authored node interfaces and made C++ wiring yield to project input choices._

## Default enable state

Sequential knobs start enabled at the authored default. Coverage knobs start
disabled at zero, preserving sparse behavior for projects without an explicit
saved control. Toggling or connecting a port preserves its stored value. The
toggle and value belong to the same durable project record. An existing
explicit participate-with-connections policy still permits additive Sequential
composition.

The same knob components serve the ports of C++-constructed nodes. Symbolic
input knobs start disabled. Concrete member input knobs start at the value
and enable state declared by the port. See the former
[member input defaults](historical/member_input_knob_defaults_2026-11-20.md).
Output gains are editable at all times and start at unity. GraphJit omits an unused symbolic
output gain when the output has no consumer, no live observation, and no
active gesture. See the former
[symbolic output switch](historical/virtual_output_gain_switch_2026-11-20.md) and
[member switch](historical/member_output_gain_switch_2026-11-20.md) rules.

## C++ wiring

Cables authored in C++ describe fixed wiring. An input gesture keeps them
connected. At a concrete input with fixed wiring, combine the auditioned
knob value with the C++ contributions using the input's normal combination.

## Temporary override

Hover prepares code for a temporary override without activating it. A drag uses
the manual value in place of project wiring at that input, even when its stored
knob is disabled. C++ wiring continues to contribute. Release saves the value
and closes the undo gesture. The
override stays active while that client hovers over the knob. On hover leave, the
settled code resumes when its generation activates. The authored wiring and the
stored enable setting survive the entire interaction.

## Public defaults

A Sequential input's authored default is an implicit manual value until a user
changes it. A Coverage input has no implicit manual contribution. Hover and an unchanged gesture do not create persisted overrides or
undo entries. Undoing the first changed gesture can restore absence of an
override, allowing the authored default to apply again. Runtime control bindings
can exist for such implicit defaults without requiring saved project records.

## Former implementation status

From [Implementation Fronts](../implementation_fronts_direction.md#direct-cable-first-input-resolution).

The owner clarified that C++ wiring is fixed. The configured graph records it
in `ConfiguredSampleConnection::source_owned`. Effective connection analysis
keeps these wires during a gesture. A Sequential module input now receives its
wires individually at its resolution stage, preserving their origin for this
decision. A live knob participates alongside any retained sources. This change
awaits native runtime tests.

**Saw `gain.a` follow-up (2026-11-24).** The previous live priority let a
symbolic knob displace an explicitly enabled concrete knob. `compile_impl()`
now gives the concrete knob priority in both live and settled code. An enabled
symbolic knob can still be suppressed after hover ends. Realtime lowering uses
the normalized connection's presence for `ManualControlPlan::fallback_only`.
Background planning uses that same connection presence to choose between
`sample_fallback_value` and `sample_manual_contribution`. In both cases a
fixed C++ connection currently supplies coverage to that decision. The
remaining receiver plan needs to select the project contribution using project
coverage and combine that result with the fixed C++ contribution.

## Former Vue knob defaults

From [Vue Presentation Refinement](../vue_presentation_refinement_direction.md).

Sample Sequential and Coverage inputs can declare `enabled_by_default`.
Sequential knobs start enabled by default. Coverage knobs start disabled by
default. An instance's saved setting takes precedence. `default_value` sets
the initial number and the double-click reset value. Double-click changes the
number and preserves the enabled state.
