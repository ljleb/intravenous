# Tock Binding Before Explicit Provided-Region Semantics (2026-11-15)

_Status: historical, non-normative. This records the earlier patch's
"requested-only" Tock wording, revised after inspecting the callback-bound
Region and `IndexCoverage` records. Its replacement is
[Automation And Suspendable Coverage Direction, section 6](../automation_and_suspendable_coverage_direction.md#6-tock-input-and-output-views),
along with corresponding updates in
[Coverage And Background Evaluation](../coverage_and_background_evaluation.md),
[Random-Access Port Data Direction](../random_access_port_data_direction.md), and
[Batched Node Callbacks Direction](../batched_node_callbacks_direction.md)._

The prior design correctly excluded incidentally cached data, but sometimes
called the positional reverse requirement the node-visible input coverage
while simultaneously giving the node complete selected Region buffers.
The source's `BackgroundEvaluationCallFrame::seal()` can indeed bind a narrow
`coverage_value` to a port with complete larger `region_views()`. That
ambiguity motivated the revised evaluation-facing naming and binding rule.

## Former section 6 of automation_and_suspendable_coverage_direction.md

## 6. Requested-only random-access inputs of `tock_coverage()`

**Normative invariant:** each `RegionInputConfig` or `CoverageInputConfig`
visible to `tock_coverage()` (or to one lane of `tock_coverage_batch()`) exposes
only the exact covered input positions **requested for that evaluation** by
reverse propagation. Its node-facing `coverage()`, Region/range enumeration,
readability, and event iteration must not expand to include other positions
merely because they exist in a published persisted root, an upstream output's
complete coverage, or a larger cached/transaction-local materialization.

Formally, the node-visible input positions are

```text
visible_input = explicitly selected covered logical input Regions
(no other available Regions)
```

The *requested* selection is the finalized evaluation requirement for that
input, not every exploratory read used by a suspended propagation callback to
decide those requirements. Selection respects the existing **whole logical
Region** contract: a demand intersecting a producer Region may retain that
complete Region as an explicitly required unit; it does not create a clipped
pseudo-Region or reinterpret persisted Region identities. A `RegionInputConfig`
still receives one contiguous Region; a `CoverageInputConfig` may receive a
selected collection of Regions, preserving overlap and multiplicity. The
scheduler must make the complete Region(s) it selected part of the **explicit
resolved request** *before* callback binding. No unselected Region is exposed
merely because it is available in storage. An authored reverse callback can
intentionally select the full finite point Region, in which case seeing it is
correct. Event iteration is limited to those selected logical Regions,
including valid zero-event regions. Read access to any unselected Region is
illegal even if its contents are already cached.

The callback's own requested output regions are selected according to the
same complete logical output-Region requirement; the requested positional
domain is intersected with exact output coverage when establishing demand. Multiple downstream roots may be unioned into one
explicit request before invocation. The invariant is **per finalized logical
request**, not per original UI query, and each `_batch()` lane has its own
request. Two executions with identical selected versions, semantic inputs,
requested input/output regions, and configuration see equivalent node-facing
inputs regardless of unrelated precomputed or stored positions. This forbids
accidental dependence on ambient materialization history.

This requested-only view restriction applies to **Tock evaluation**; it does
not prohibit forward/reverse planning from consulting additional, explicitly
requested discovery inputs. Those exploratory reads must not leak extra
coverage into the subsequent `tock_coverage()` input binding.

## Former input invariant in coverage_and_background_evaluation.md, section 10

**Requested-only input invariant:** for every `RegionInputConfig` and
`CoverageInputConfig` bound to a `tock_coverage()` invocation (or an individual
`_batch()` lane), node-facing Region enumeration, positional `coverage()`,
accessible data and event iteration describe exactly the **selected covered
logical input Regions** explicitly requested by reverse planning. The request
is normalized to complete logical Regions before binding: no cropped Region
is substituted for a selected source Region, and a Region input still receives
one contiguous Region. The source's wider output coverage, a persisted
snapshot's additional Regions, a cached upstream materialization, or unused
discovery reads from a propagation coroutine do not widen the selection. A
node may explicitly reverse-require a full finite input Region, in which case
seeing it is intentional. Event Regions may be complete while containing
zero events. Reads of unselected Regions are invalid even if the addresses
happen to be backed by retained storage.

Thus, at fixed semantic/data versions and authored configuration, two Tock
calls with the same selected logical input/output Region requests see equivalent
inputs and produce the same observable result regardless of other positions
previously materialized. This also holds for each batch lane independently. Batching
several demand roots can union their **explicit** requested regions before a
single callback; there is no promise that two different requests present
identical views.

## Former random-access port binding paragraph

A consuming callback still receives only the
Regions selected by its own resolved input requirement; extra producer Regions
cannot become visible merely because other work materialized them. In
`tock_coverage()` and each `tock_coverage_batch()` lane this is a strict
requested-only input binding rule: the scheduler first normalizes the request
to complete logical Regions, then exposes only those selected Regions,
independent of wider retained/cached availability.
## Former batch direction paragraph

For Tock evaluation, every lane's Region/Coverage inputs expose only the
reverse-required **requested-and-covered** positions for that invocation,
regardless of additional available or cached storage. The scalar and batch
forms must see equivalent input views for the same requested positions.
