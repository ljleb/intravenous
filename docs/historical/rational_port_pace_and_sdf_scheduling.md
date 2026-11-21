# Superseded Rational Port Pace And SDF Scheduling Direction

_Status: non-normative historical material._

_Provenance: preserved on 2026-10-14 from the pace sections of
[`graph_jit_direction.md`](../graph_jit_direction.md),
[`sequential_port_storage_planning.md`](../sequential_port_storage_planning.md),
[`dsp_execution_storage_glossary.md`](../dsp_execution_storage_glossary.md), and
[`port_realization_migration_status.md`](../port_realization_migration_status.md)._

_Reason for supersession: implementation analysis showed that unrestricted rational
actor firing rates require generic cross-root partial-firing ownership and periodic
multirate SCC schedules. The accepted replacement restricts dense port pace to exact
powers of two and executes every node over one shared dyadic global-time interval,
allowing each port to contain zero or more values. See the current pace sections in the
source documents above._

## Preserved normative direction

Port pace was previously specified as an unrestricted positive rational quantity of
transported values per local logical node step. Pace arithmetic was required to remain
exact rather than use floating point. The graph-wide solve used:

```text
absolute transported-value rate of port
    = effective execution rate of owning node * authored local port pace
```

A connection equated absolute transported-value rates rather than local pace constants.
An oversampler with local input pace one and local output pace two could therefore feed
an ordinary filter whose local input and output paces were both one; the filter's node
execution rate became twice the oversampler's input-domain rate. Products of rational
ratios around a cycle had to be consistent, and each otherwise-unanchored component
received a deterministic canonical scale of one.

GraphJit was expected to choose a legal logical invocation quantum, derive integral
per-port block extents from those rational rates, and preserve exact correspondence
across arbitrary root callback subdivision. A node defining pointwise `tick()` was
legal only when its relevant Sequential ports resolved to a compatible equal pace.

## Preserved implementation friction

The checked-in generated root accepts any nonempty block extent up to its compiled
maximum and may keep ordinary feed-forward values only in invocation-local stack
storage. Under the rational-firing interpretation, a decimator consuming two values per
firing and receiving a five-value root callback could complete only two firings. The
fifth value required persistent ownership until the next callback. A multirate SCC also
required a repetition vector and periodic firing schedule rather than the existing
one-call-per-member slice traversal.

The provisional general solution was static synchronous-dataflow realization:

- exact rational rate-domain cursors relative to the project timeline;
- an integral repetition vector and bounded periodic firing schedule per execution
  region;
- persistent connection carry for values belonging to incomplete firings; and
- rate-domain-aware persistence, feedback, event timestamps, and migration.

Further analysis separated audio sample-rate basis, transported-value cadence, and the
sample support of a block-valued element. It also showed that dyadic global-time
interval execution can present zero-or-more values per port without leaving generic
unconsumed connection values. That accepted simplification superseded both unrestricted
rational pace and the proposed general SDF runtime.
