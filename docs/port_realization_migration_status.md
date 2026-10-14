# Port Realization Migration Status

_Status: active implementation record. This document describes current source work;
the normative destination remains in `graph_jit_direction.md` and
`random_access_port_data_direction.md`._

## Active feature

The active feature is the complete graph-wide port-constraint and resolved-realization
layer, including pace-aware execution. It replaces static config-owned timing with
instance-authored constraints and makes the resulting facts authoritative for planning,
declaration, storage, scheduling, and lowering.

## Accepted pace algebra

Authored port pace is relative to one logical step of the owning node:

```text
absolute transported-value rate of port
    = effective execution rate of owning node * authored local port pace
```

Authored pace factors are powers of two. The implementation represents them as signed
base-2 exponents. Connections equate absolute transported-value pace exponents; they do
not directly equate local constants. Weighted exponent differences solve node execution
domains, and inconsistent sums around cycles reject the graph revision. The supported
absolute range is initially `[-64, +64]`, matching the Q64.64 dyadic execution
coordinate.

## Implemented internal milestones

- `ConstrainPortsContext` now has statically restricted proxies for continuous value
  type, dynamic value size, local port pace, Sequential/Tick history, Tick latency, and
  Random-Access/Tock ordering.
- Reflection and configured-graph serialization retain each property-specific equality
  and anchor without a type-erased property/value mismatch state.
- `ResolvedPortRealization` now contains selected continuous value types, dynamic sizes,
  local and absolute port paces, node execution rates, sample/event history and latency,
  and Coverage ordering requirements.
- The graph solver now resolves ordinary equivalence classes and weighted node-rate
  exponent differences, propagates continuous value type/size constraints through
  connections and bundle boundaries, and rejects ambiguous dimensions, inconsistent
  pace cycles, non-power-of-two authored factors, and absolute exponents outside the
  Q64.64 range.
- Static `SequentialInputConfig::history` and `TickOutputConfig::history/latency` have
  been removed. Production nodes with nonzero timing now author those facts through
  `constrain_ports()`.
- Connection analysis and sample/event storage planning consume resolved history and
  latency rather than rereading static port configs.
- `PortPace` and the weighted node-domain solver now use signed base-2 exponents;
  configured-graph serialization stores that exponent directly.
- `FractionalGlobalIndex` and `FractionalGlobalIndexRange` now represent the common
  circular Q64.64 coordinate domain for execution/storage addresses and transported
  coordinates. A finite range projects onto a resolved lattice, including zero-value
  slow-port projections and wraparound.
- Each registered continuous-value extension now owns its trivially copyable storage
  element, resolved-size validity, channel capability, and per-value audio sample
  support. The preliminary `FFTBlock` extension enforces at least explicit DC and
  Nyquist bins and derives `2 * (bin_count - 1)` samples of support.

## Incomplete milestones

- GraphJit scheduling still invokes every node with one common project block extent.
  It does not yet project the root's selected dyadic interval onto each port lattice or
  present the resulting zero-or-more values to each node.
- Tick bindings and authored port facades still address Sample storage with the old
  one-frame-per-project-index assumption. They do not yet expose per-port transported
  `block_size()` or the resolved `block_extended()` Sequential view.
- Audio sample-rate basis, transport cadence, and per-value sample support are not yet
  independently solved or applied to declaration, initialization, Tick, or Tock
  contexts.
- The remaining unit tests that directly construct timing-bearing legacy configs and
  directly invoke connection analysis have not yet been migrated to authored constraints
  and explicit resolved-realization input.
- Coverage ordering is solved and propagated where both endpoints expose an ordering
  variable; the Region/Coverage realization that satisfies a sorted requirement remains
  owned by Front 3.

## Resolved implementation friction: rational-rate callback tails

The initial scheduling plan assumed that resolved rational node rates could be applied
by deriving one legal root-domain quantum and passing a different integral extent to
each node. Source inspection exposed an additional ownership requirement that the plan
did not state.

`GraphExecutor::tick_block()` accepts any nonzero root extent up to the compiled
maximum. The generated root currently gives every member of an SCC one common
root-domain slice, and ordinary feed-forward values may live only in the generated
root's invocation-local stack arena. A rational-rate node can end such a root call with
an incomplete logical firing. For example:

```text
decimator local input pace  = 2
decimator local output pace = 1
root callback extent        = 5 project-rate samples

complete decimator firings  = 2
consumed input values       = 4
pending input values        = 1
```

That fifth input value cannot be discarded and cannot remain in invocation-local stack
storage. Restricting every host callback to a multiple of a graph-wide least-common
quantum would avoid the pending value, but would couple the audio device's callback
contract to every rational pace in the graph and could make the required quantum grow
without a useful bound. It is therefore not an implementation-neutral detail.

Multirate semantic SCCs expose the same issue inside one callback. A consistent cycle
may contain one node firing once while another fires twice. The present
one-call-per-member-per-SCC-slice schedule and one common slice index cannot express
that periodic firing schedule or the different connection-domain cursor advances.

The same analysis also invalidates the assumption that every heterogeneous-pace node
has one callback-facing sample rate. For an STFT with local input pace 512 and local
FFT-output pace 1 connected to a project-rate sample stream:

```text
sample-input absolute rate = 48000 values/second
node firing rate           = 93.75 firings/second
FFT-output absolute rate   = 93.75 values/second
```

The FFT implementation needs the 48 kHz sample-input rate to interpret its bins; a
node-level `ctx.sample_rate` derived only from the firing rate would report 93.75 Hz and
would be semantically wrong. Absolute value rate and integer index progression must
therefore be port/rate-domain facts. A node-level sample-rate convenience is valid only
for a statically or graph-proven homogeneous rate domain. This also requires an explicit
mapping between each integer port-domain index and the project/root timeline; passing
one project `GlobalIndex` unchanged to every node is insufficient for oversampled,
undersampled, and FFT-block streams.

That mapping cannot be inferred from the 64-bit circular `GlobalIndex` bits and a
rational rate alone. Multiplication or division by a non-unit rational is not an
invertible mapping of the modulo-2^64 ring, and a seek may begin between two logical
firings. A resolved rate domain therefore also needs an explicit timeline origin/phase
and a stated discontinuity/seek rule. This is a semantic design question, not merely a
wider-integer implementation detail.

The likely general solution is a static synchronous-dataflow realization:

- each connected rate component has exact rational domain cursors relative to the
  project/root timeline;
- each rate-domain cursor has an explicit origin/phase and a defined reset or
  reconstruction rule for discontinuous root indices;
- each resolved continuous port exposes its absolute value rate and domain-relative
  integer interval, while homogeneous nodes may expose a proven common rate as a
  convenience;
- graph compilation derives an integral repetition vector and a bounded periodic firing
  schedule for each execution region;
- generated execution consumes every complete firing available in the selected root
  interval;
- compiler-planned persistent connection carry retains values belonging to incomplete
  firings across root callbacks; and
- root-output availability, authored latency, feedback delay, event timestamps,
  persistence coordinates, and generation migration are validated against those domain
  cursors rather than assumed to share one project-frame index.

The accepted resolution does not implement that general SDF expansion. Port pace is
restricted to powers of two, and generated execution covers one shared half-open Q64.64
dyadic interval. Each port observes the zero or more lattice positions inside that
interval. A slow port may therefore expose an empty block during a root invocation,
while a fast port exposes multiple values, without leaving an incomplete generic actor
firing in invocation-local storage. Algorithms needing preceding values declare
history and use `block_extended()`; output revision uses latency. Non-dyadic conversion
is explicit resampling or coordinate-driven Random Access work.

The same resolution separates audio sample-rate basis from transported-value cadence
and per-value sample support. An FFT output may have a slow block cadence while
retaining the input signal's audio basis. A node-wide `ctx.sample_rate` convenience is
legal only for relevant audio ports proven to share one basis.

The superseded unrestricted-rational and provisional SDF proposal is preserved with
its provenance in
[Rational Port Pace And SDF Scheduling](./historical/rational_port_pace_and_sdf_scheduling.md).

## Expected repository state

The repository is **not expected to compile** at this milestone. Expected compiler
errors belong to this unfinished feature and are concentrated in tests that still use
removed config timing fields or the old connection-analysis signature. No compatibility
wrapper should be introduced to make those call sites compile; migrate them after the
pace-aware execution shape is complete enough to define their final assertions.

## Next bottom-up connection

Carry projected per-port extents into generated Tick bindings and storage planning.
Then migrate the remaining tests, add execution-path cycle/range/dimension regression
coverage, and refactor the solver around the stable responsibilities discovered by the
working execution path.
