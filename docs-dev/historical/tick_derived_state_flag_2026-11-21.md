# Tick State reconstruction flag

This design was recorded on 2026-11-21 in the node interaction direction and the observation cutover status. The [Tick lifecycle contract](../node_interaction_and_presentation_communication_direction.md#tick-state-across-graph-generations) replaces the flag with the rule for Tick nodes that have no move operation.

## Previous lifecycle contract

A Tick node can declare `static constexpr bool derived_state = true` if its
construction arguments and resources contain all the information needed to
initialize its `State`. When the configuration stays the same, a successor
initialized this way must preserve the node's observable behavior. For example,
a node can resolve a device binding again during preparation. A node must carry
signal history forward if future callbacks depend on that history.

The declaration requires `State` and `initialize()`. The executor can rebuild
derived State for an observation cutover when it can initialize the successor
outside the audio thread while the predecessor still runs. The existing
`move()` rules still apply when a compatible move is available. Tock State
already follows the reconstruction rule as part of its callback contract.

## Previous observation cutover status

`SystemAudioInput` and `SystemAudioOutput` now declare `derived_state`.
When a new probe needs a storage cutover, `NodeStorage` can initialize
their bindings from the unchanged configuration during preparation.
The storage cutover regression checks live probe readings, the feedback
signal, and system audio across activation. It also checks that device
resolution completes before activation.

## Lifecycle contract before package validation

After the flag was removed, the node interaction direction inferred the reconstruction promise from the absence of a move operation. On 2026-11-21, the [Tick lifecycle contract](../node_interaction_and_presentation_communication_direction.md#tick-state-across-graph-generations) made that choice explicit through the State move constructor deletion.

A Tick node with no authored or implicit move operation promises that fresh
State preserves observable behavior when the configuration stays the same.
The successor reconstructs any required State from its construction arguments
and resources. The executor calls `initialize()` during preparation when its
dependencies are ready and calls `release()` on the predecessor at cutover.
Initialization that needs imports or moved State runs at the cutover boundary
and follows the existing realtime constraints.
