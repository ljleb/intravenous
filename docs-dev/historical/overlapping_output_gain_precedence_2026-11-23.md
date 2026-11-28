# Overlapping output gain precedence before route-based composition

_Historical, non-normative. Preserved on 2026-11-23 after the owner approved
gain composition along output routes._

The following text formerly appeared under
[Persistent manual controls](../node_presentation_and_manual_controls_direction.md#persistent-manual-controls).
The [output gain specialization contract](../graph_jit_direction.md#output-gain-specialization-owner-requirement-2026-11-16)
now specifies the intended output behavior. The input rules remain in the
manual-controls document.

> A symbolic knob stores one value and applies it to the ports in its mapping.
> An explicitly enabled or edited concrete member input takes precedence at
> its port. An enabled symbolic input takes precedence over an implicit
> member default. A concrete member output gain takes precedence when its
> value differs from unity.
> A temporary drag takes precedence over settled controls. If two controls of
> the same kind and specificity still overlap, the first saved control takes
> precedence.
