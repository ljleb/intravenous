# Presentation ABI 5 edit records

This records the superseded ABI 5 edit layout from
`docs/api/reference/presentation_c_abi.md` and the Automation Curve
migration notes from `docs/implementation_fronts_direction.md`.
See the [ABI 6 C reference](../api/reference/presentation_c_abi.md) and
the [current migration notes](../implementation_fronts_direction.md#automation-curve-boundary-audit-2026-11-22).

The ABI 5 context was 168 bytes. Offsets 156 and 160 held the head and
tail of numeric edit batches. `iv_submit_numeric_edits(context, span)`
submitted typed numeric node-event edits and gesture finishes in order.
The descriptor prefix used version 5.

An `IvNumericNodeEdit` is 36 bytes. Nominal event ID, schema fingerprint,
and gesture ID are strings at 0, 8, and 16. A span of named fields at 24 is
followed by flags at 32. Each field is 20 bytes and comprises a name followed
by an `IvNumericValue` at byte 8. Use the node-event protocol's exact event
identity and field names. The host validates the receiver and payload against
that protocol before editing. A commit has no gesture ID. A preview may
use `IV_EDIT_REPLACE_PENDING` and a gesture ID if a newer value supersedes
the entire pending preview for that gesture. Submit `IV_EDIT_FINISH` with
only the gesture ID to finish it. The host accepts at most 32 commands in
a frame and 64 fields per command.

The ABI 5 numeric edit record cannot describe `PlacePoint` as one edit.
Its point contains a converted exact sample index and an interpolation
enum. The same edit carries a sequence of segment records and a sequence
of point IDs for coincident order. Splitting these into numeric operations
would expose intermediate placements and change the undo and acknowledgement
contract. `NodeConstructionArguments` also publishes the accepted argument
values as JSON without the `expression_schema` that describes their layout.
The server already publishes the reflected payload schema for node events.

**Proposed dependency.** Generate a versioned, fixed binary layout for
each bound constructor or event schema. A guest would use generated
records and spans. The host would bind and validate the layouts once per
artifact, use the same node-event identities at delivery, and still send
ordered request acknowledgements. This extends the configuration metadata
and package tooling. The other option is to make Automation Curve's point
and placement structs part of the general presentation ABI. That is a
smaller port and requires another ABI change for each new payload shape.

