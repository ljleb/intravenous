# Timeline Transport And Realtime Execution Constraints

This note is a guardrail for future timeline and lane work. It is not a full
implementation design.

The current lane design notes describe the structural lane model: what a lane
is, what domains exist, how lanes connect, and how lane views project the graph.
This note is about execution details and control-flow ownership.

Future lane/runtime work should preserve the expectation that `Timeline` becomes
the authority for realtime transport state: play/pause state, current playback
index, seek/resume position, and the advancement of realtime execution.

Realtime DSP graph instances should eventually execute as timeline-managed or
timeline-scheduled participants whose inputs and outputs are represented through
realtime lanes. Device callbacks may request work, but they should not become
the source of truth for timeline position.

The realtime lane APIs should avoid assuming that all realtime lanes share one
global sample rate or block size. Future graph inputs, graph outputs, and lane
connections may run at power-of-two over- or under-sampled rates relative to one
another. Current implementations may run everything at one rate, but the API
shape should leave room for timeline-owned scheduling, rate conversion, and
block-size adaptation.
