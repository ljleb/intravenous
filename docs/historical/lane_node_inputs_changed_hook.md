# Lane Node Inputs-Changed Hook

This note captures the current intended direction for optional lane-node hooks
that react to connectivity changes.

## Goal

Some lane nodes need to run code when their inputs are rebound or when the
compiled-support information of relevant inputs changes.

The runtime should support that without forcing every lane node to implement a
hook.

## Lane-Node Surface

Lane nodes may optionally define:

- `on_inputs_changed(ctx)`

If a node does not define it, nothing happens.

Traits should provide:

- `has_on_inputs_changed<Node>()`
- `do_on_inputs_changed(node, ctx)`

The `has_*` helper should allow callers to avoid building a context for node
types that do not implement the hook.

## Context Shape

The hook should receive the current connectivity description, not a framework
diff.

If a node wants to compute its own diff, it may store prior connectivity and
compare.

The connectivity description should include enough information for nodes to
react meaningfully, especially:

- input ordinal
- input port kind/domain
- stable source-lane identity
- compiled support ranges for inputs whose source output is compiled

## Timing

This hook should run during synchronization/rebinding in execution-side modules,
not during task execution.

The intended owner is `TimelineExecution`, which already derives runtime lane
state from structural lane changes.

This keeps allocation/reconfiguration work out of the execution pass itself.
