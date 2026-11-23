# Tick replay and playback coverage before the pointwise correction

Status: historical, preserved November 17, 2026. These excerpts describe an
invalid replay capability and the coverage preparation that depended on it.
See [Tick input-union coverage](../tick_input_union_and_feedback_direction.md),
[coverage and background evaluation](../coverage_and_background_evaluation.md),
and [playback implementation](../project_playback_implementation.md) for the
replacement. The earlier coverage and glossary wording is also preserved in
[the Tick coverage archive](./tick_coverage_before_intent_recovery_2026-11-17/).

## GraphJit direction

From `graph_jit_direction.md`, replay eligibility:

> The target semantic rule removes the Random Access prohibition. A Random
> Access input is a declared replay dependency that background planning must satisfy
> before invoking the Tick wrapper; it is not hidden state and does not by itself make
> the computation unreplayable.

> A replayable Tick node with non-pointwise Random Access addressing participates in
> the same background forward/reverse dependency framework as authored Tock planning;
> its callbacks may conservatively request/invalidate complete potentially relevant
> coverage when a tighter value-blind bound is unavailable.

## Tick input-union coverage

From `tick_input_union_and_feedback_direction.md`, values and coverage:

> Existing authored coverage callbacks keep their separately declared contract;
> this correction does not discard authored propagation.

From the implementation dependency order:

> Keep coverage work off the audio thread.

## Project playback implementation

From `project_playback_implementation.md`, open friction and follow-up:

> One option is to prepare a live coverage table for each exact `(P, I, extent)`
> before Tick. That preserves an authored callback's ability to inspect coverage
> and contents, but introduces a new background dependency on each advancing
> paused execution window. Another option is to compile the generated
> `input_union` selections into bounded lookups that distinguish `P` and `I`,
> leaving authored coverage queries on the background worker. The latter still
> needs a defined prepared result for authored forward coverage and a check of
> how that result feeds the compiled lookups. Do not move general coverage
> propagation onto the audio thread to avoid this choice.

> `BackgroundPropagationWorkspace::prepare_live_tick()` projects held
> Sequential coverage into bounded, exact execution windows. The
> `TickMaterializationSnapshot` keeps those results separate from the
> position-addressed result. Generated `input_union` sees a covered held
> Sample or a covered, empty held Event range as covered. A fallback value
> does not contribute coverage. Random Access and replay keep their
> position-addressed selection.

> The exact-window table does not solve content-dependent authored live
> coverage. `request_information()` gives a Sequential input's projected
> coverage to a metadata request. For a contents request it calls
> `prepare_discovery()`, which only has the position-addressed snapshot and
> the requested positions. For example, a forward callback can request the
> contents of a Sequential input fed by an ordinary live Tick node and
> publish an output range only if the first value is positive. If that node
> reads the current audio input, no recorded value or replay at the same
> index can give the value of the not-yet-executed callback. The playback
> readiness check needs the coverage before allowing that callback to run.
> Position-addressed discovery does not satisfy this dependency. This change
> does not add a replayability restriction or use replay as a substitute for
> that live value.

> Also verify sparse live Tick-to-Tick inputs with nonzero read latency:
> their coverage can depend on positions before the exact execution window,
> particularly across a seek. The held Tock selection and the current
> paused coverage table are not a record of that earlier live coverage.
