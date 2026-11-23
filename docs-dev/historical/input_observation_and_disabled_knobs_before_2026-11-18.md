# Input observation and disabled knobs before 2026-11-18

_Historical, non-normative. Preserved after the owner approved observation of
resolved input values and temporary editing of disabled knobs._

## Observation scope and background queries

Source and replacement sections are [Scope and invariants](../vue_output_observation_direction.md#1-scope-and-invariants)
and [Background window queries](../vue_output_observation_direction.md#background-window-queries).

- **Outputs are the initial observable port endpoints.** All inputs remain visible,
  discoverable, and connectable through ordinary graph metadata, but *input value*
  observation is explicitly deferred. A later input view must define what the
  receiving node actually sees after fan-in and conversion; it must not silently
  substitute one connected source.

A range observation names one output, a finite global timeline window or exact
requested positions, an operation, and a bounded resolution/quality request.
For `tock/ephemeral`, its demand is intersected with exact `IndexCoverage` and
batched through the existing reverse-coverage/evaluation transaction. Any needed
projection of ephemeral data occurs while its transaction-local views remain
valid, yielding an independently owned **bounded** result; the client never
pins arbitrary Tock scratch or receives a dangling storage pointer.

For published `tock/persisted` and Tick/persisted outputs, query the relevant
subset of a pinned immutable `PersistedDataStore` version. A Tock/persisted
candidate that is not complete must not be displayed as if it were published;
completion and atomic publication retain their existing full-output-coverage
contract. No observation subsystem duplicates persisted records or invents a
partially committed persistent output.

An output's **inputs** may participate in reverse coverage as ordinary internal
dependencies. This is not input-port observation by the Vue client.

Defer field-level serializer overrides, Tock `State` observation, input value
observation, arbitrary authored observation callbacks, elaborate derived caches,
and broad custom spectrum/event operation catalogs until demonstrated needs.

## Connected input override and observation status

Source and replacement sections are [Temporary connected-input override](../node_presentation_and_manual_controls_direction.md#temporary-connected-input-override)
and [Live observation is outside Front 14](../node_presentation_and_manual_controls_direction.md#live-observation-is-outside-front-14).

_Settled by the repository owner on 2026-11-14._

During a live Sequential knob interaction, the manual value replaces the selected input's incoming signal,
including when its settled participation policy is disconnected-only. The knob
retains its independent manual value; it does not have to follow the connected
signal. Releasing the mouse closes the undo gesture. While that client's pointer
remains over the knob, retain the dynamic code and temporary override. Leaving
the knob requests settled code immediately; the authored connection resumes when
that replacement activates. Neither the authored wiring nor the durable
participation policy changes. Hover alone prepares code and never activates an
override. This hover-leave policy was accepted on 2026-11-14; the
[earlier settling policy](./historical/manual_control_debounce_before_hover_settling_2026-11-14.md)
is preserved for provenance.

Represent the override as an internal Tick/ephemeral producer at the selected
public input. Preserve module-internal conversion, fanout, and authored temporal
cuts. Input history records the values actually consumed, including manual
values that remain in the history window after the gesture ends. A recording
node can explicitly persist the gestures when desired.

Front 14 did not implement the observation API. The current normative direction is
[Vue Output Observation And Query Direction](./vue_output_observation_direction.md):
output-first type-registered operations, reflected supported Tick `State` fields,
selective live probes, sparse/persisted Tock output queries, and the planned
`Observations` application module for batched subscriptions/results. Input values
and Tock `State` diagnostics are explicitly outside the initial scope. Concrete
Vue and transport interfaces still follow real client requirements. Manual controls
do not depend on implementing observations first.
