# Previous precedence for overlapping manual input controls

This superseded design came from
[Persistent manual controls](../node_presentation_and_manual_controls_direction.md#persistent-manual-controls)
at revision `d478132f`. See the accepted
[input override rule](../node_presentation_and_manual_controls_direction.md#input-override-rule).

> A symbolic knob stores one value and applies it to the ports in its mapping.
> An explicitly enabled or edited concrete member input takes precedence at
> its port. An enabled symbolic input takes precedence over an implicit
> member default. A temporary input drag takes precedence over settled input
> controls. If two input controls of the same specificity still overlap, the
> first saved control takes precedence.

The discussion briefly considered resolving equally close symbolic controls
in authored assignment order. That rule was superseded before implementation
was completed.

## Concrete knob followed by the closest symbolic knob

The working draft of the input override rule on 2026-11-24 proposed that
an unwired input or a known cable gap use the enabled concrete knob, then
the closest enabled symbolic control, with authored assignment order
breaking ties. The port's implicit default came last. It also proposed
revisiting whether a package could choose the policy through an annotation
or macro. The accepted [direct cable first rule](../node_presentation_and_manual_controls_direction.md#input-override-rule)
replaces the single symbolic winner with ordinary input combination.

## Same-day additive proposal

The subsequent discussion considered letting enabled knobs contribute to
the input's usual combination with cables. In that proposal, two enabled
symbolic knobs mapped to one additive Sample input would add their values.
An oscillator with a 220 Hz cable and a 440 Hz knob would receive 660 Hz.
The owner rejected this interpretation and chose knob override at the
input. The proposal reached the design note and never reached execution.

## Persistent override interpretation

The first version of the replacement design note said an enabled knob
replaced a cable in the settled graph. Its example gave an oscillator
440 Hz whenever its local knob was enabled at 440 Hz, including with a
220 Hz cable attached. The owner clarified that the wired signal takes
precedence in settled execution. A knob gesture can temporarily replace
the wired signal so the user can audition the knob.

## Group-wide audition proposal

The discussion also considered replacing the signal at every concrete
input mapped by a symbolic knob. One option included unwired members
with enabled concrete knobs. Another left those members at their local
values. The owner then scoped an audition to the input whose knob is
being edited. Each downstream input resolves its own incoming signals.
That decision supersedes both group-wide alternatives.

## C++ wires during an input gesture

The first direct cable first draft said that a drag replaced the incoming
signal at the selected input. Its example had only project cables and never
distinguished the two kinds of wiring. The owner clarified that a C++ cable
is fixed. See the [input override rule](../node_presentation_and_manual_controls_direction.md#input-override-rule)
for how it combines with an auditioned value.
