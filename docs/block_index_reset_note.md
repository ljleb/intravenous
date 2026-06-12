# Block Index Reset Note

`IvModuleInstancesExecution` currently advancing its own realtime block index is
acceptable for now.

When timeline playback is resumed, the timeline-side owner of the playback
block index should later publish a reset event so that
`IvModuleInstancesExecution` can realign its per-instance block indices.

That reset wiring is intentionally deferred until after the current execution
and cache work is finished.
