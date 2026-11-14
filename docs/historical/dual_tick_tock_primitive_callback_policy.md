# Superseded dual Tick/Tock primitive callback policy

_Historical record captured 2026-10-21. This preserves the callback policy that was
superseded by the single-domain primitive rule now stated in
[Batched Node Callback Direction](../batched_node_callbacks_direction.md) and
[Coverage, Random Access, And Background Evaluation](../coverage_and_background_evaluation.md)._

Before the Front 7 callback-boundary decision, the design treated Tick and Tock
authoring as separate choices on one concrete primitive. The affected normative text
said that an ordinary Tick node authored exactly one of `tick()`, `tick_block()`, or
`tick_block_batch()`, while a node with authored Tock production separately authored
exactly one of `tock_coverage()` or `tock_coverage_batch()`. Static validation likewise
described those as two independent callback-family checks. This allowed one primitive
to contain both a Tick callback and a Tock callback and to mix Tick and Tock outputs.

The rationale was that output production is a per-port axis and that one node might
share work among several outputs. The runtime implementation consequently retained
Tick/skip and Tock/propagation compiler anchors on the same concrete node record, and
several planning tests used mixed-production primitives.

The collaborator rejected that composition model on 2026-10-21. A concrete executable
primitive now has exactly one authored primary execution family: one Tick form or one
Tock form. Its outputs belong to that family. A module node groups separate Tick and
Tock primitives when one authored abstraction should expose both. Replay remains an
asymmetric compiler operation: an eligible Tick implementation can be invoked by a
generated background replay program, but an authored Tock implementation never backs
or synthesizes a Tick callback.

