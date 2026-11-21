# Front 3 Contract Before Recording Coverage And Per-Region Event Order

_Status: historical. This note preserves normative text superseded on 2026-10-26
by the recording Coverage rule and the per-Region event-order rule in
[Random-Access Port Data Direction](../random_access_port_data_direction.md#5-regioncoverage-compatibility-is-asymmetric)
and
[Coverage And Background Evaluation](../coverage_and_background_evaluation.md#22-direct-connection-compatibility-and-explicit-recording)._

The earlier compatibility table used one generic `Tick stream` row and said that it
was effectively Region-shaped for its available block. That statement did not
distinguish a replayed Tick invocation from a Tick/persisted recording. The earlier
direct-connection text also said that the unreproducible Tick/ephemeral restriction
was the only special prohibition introduced by retention.

The accepted replacement distinguishes the two representations. A replayed Tick
invocation has one invocation block. A Tick/persisted recording is a sequence of
positional overwrites whose `IndexCoverage` derives zero or more maximal contiguous
Regions. Overlapping and touching recorded ranges merge. Original Tick block
boundaries have no authored Region identity. A Tick/persisted recording can therefore
satisfy a Coverage input but cannot satisfy a Region input.

The earlier event-output contract also said that one output invocation emitted events
in nondecreasing global timestamp order. Event Coverage now gives every authored
Region an independent event sequence, so the replacement requires nondecreasing
timestamps within each event Region. A Sequential consumer still receives a
deterministically premerged timeline.
