# Output gain before connection-only observation semantics

This records the earlier output gain contract from the output gain implementation
record and the GraphJit direction. The owner superseded its observation semantics
on 2026-11-19. See the [current output gain contract](../output_gain_implementation.md#accepted-contract)
and the [GraphJit direction](../graph_jit_direction.md#output-gain-specialization-owner-requirement-2026-11-16).

The gain was a non-destructive `output * scale` transform after the public
output, including buffered outputs and previously stored contents. It changed
the value consumers read, including background and random-access consumers.
Output shape, logical Region identity, sparse coverage and source materialization
validity remained unchanged. Consumer reads used a coherent graph/control
snapshot. The output observations selected the private gained view, and both
background replay reads and observations used coherent gain-version selection.
