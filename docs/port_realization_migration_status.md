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
  Coverage ordering. Region ports are intrinsically ordered and expose no ordering
  constraint.
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
- Leaf nodes now declare ports as heterogeneous tuples of concrete typed authored
  configs. `input<Types...>()` and `output<Types...>()` use scoped access/production
  tokens, and each registered continuous-value extension owns the option types for
  every legal access or production form. Homogeneous generic nodes may still return an
  array of one concrete authored config type.
- Reflection now erases authored continuous configs into configured continuous records
  whose Sequential/Region/Coverage or Tick/Region/Coverage alternatives are encoded by
  distinct variants. Access/production metadata therefore cannot form an invalid
  cross-product. Event configs remain separate and carry no continuous-only options.
- Configured Sequential continuous inputs retain scalar default/minimum/maximum
  presentation metadata only in rank-zero extensions and only where meaningful.
  Nonzero-dimensional values expose none of those fields. Every extension retains
  overridable `neutral_value` and `combine_values` reduction semantics. The old
  parallel Sample-only projection fields have been removed. Existing Sample planning
  reads the same type-owned metadata that future continuous-value realizations will consume.
- Configured-graph serialization delegates binary encoding to each type-owned metadata
  alternative and retains the declarative metadata for every
  continuous alternative. The metadata alternatives themselves define the configured
  value-type domain. No parallel bitset can disagree with them, and the
  archive rejects empty or repeated alternatives. Executable function pointers are not
  serialized.
- The erased metadata variants and `ContinuousValueTypeId` use one registry order.
  Compile-time checks prove that each alternative matches the extension at the same
  registry position. Graph and archive code ask one shared helper for the value type.
  They do not cast `std::variant::index()` independently.
- Audio-capable extensions accept channel layout in their typed authored options, but
  reflection promotes the common layout to the configured port-level field and removes
  it from erased type metadata. Configured records therefore have one channel-layout
  fact rather than synchronized copies in every possible value-type alternative.
- Each node compiler record now retains optional node-specialized operations for
  initializing the neutral storage of a Sequential continuous input and combining its
  storage values. Those operations dispatch only over that node's statically authored
  continuous-input/type pairs, so per-input `combine_values` overrides remain compiled
  package behavior without imposing runtime work on unrelated ports.
- A dynamically sized `FFTBlock` Sequential input represents `neutral_value` as an
  initializer operation over one resolved logical block rather than one bin copied
  across the block. The compiler-record initializer invokes that operation once per
  logical FFT value after realization has supplied its bin count. Neither this
  executable initializer nor `combine_values` is serialized into configured metadata.
- Configured continuous records and their properties are named for continuous values
  rather than `Sample`. The existing sample-named execution and storage planner remains
  the explicitly unfinished realization layer.
- Graph and module source introspection now report continuous input and output records
  separately from event records. A continuous record contains the full declared
  value-type domain and type-owned presentation metadata. Sample alternatives can
  report scalar default, minimum, and maximum values. `FractionalGlobalIndex` reports
  its scalar default with exact decimal bit strings. `FFTBlock` reports no scalar
  presentation fields. Finite Sample values use JSON numbers. Non-finite Sample values
  use the stable strings `nan`, `positiveInfinity`, and `negativeInfinity` because JSON
  has no non-finite number. The boundary also reports only real configured
  connectivity and virtual-port binding facts; it does not invent current values or
  overrides.
- The VS Code graph model consumes the continuous introspection schema and displays
  declared types and bindings. The former read-only knobs and state actions were
  removed because no runtime read or mutation path supplied their values.
- The unit tests that formerly constructed timing-bearing legacy configs or invoked the
  old connection-analysis signature have been migrated to authored constraints and
  explicit resolved-realization input.

## Incomplete milestones

- GraphJit scheduling still invokes every node with one common project block extent.
  It does not yet project the root's selected dyadic interval onto each port lattice or
  present the resulting zero-or-more values to each node.
- Tick bindings and authored port facades still address Sample storage with the old
  one-frame-per-project-index assumption. They do not yet expose per-port transported
  `block_size()` or the resolved `block_extended()` Sequential view.
- Source introspection cannot report the selected value type, resolved dynamic size,
  or live runtime value. Those facts become available only after GraphJIT realization
  and require the planned ProjectGraph/runtime read-model join.
- Audio sample-rate basis, transport cadence, and per-value sample support are not yet
  independently solved or applied to declaration, initialization, Tick, or Tock
  contexts.
- Coverage ordering is solved and propagated among Coverage endpoints. Region ports
  carry no ordering variable because a finite Region is intrinsically ordered. The
  Region/Coverage realization that satisfies a sorted Coverage requirement remains
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

The typed authored/configured port-config migration is intended to compile as one
coherent feature, but its current working-tree implementation has not been built or
tested by the implementation agent. The repository owner performs that validation.
No compatibility aliases or wrappers preserve the removed Sample-only authored-config
API. The prior expected-broken state is preserved in
[Port-realization status before typed authored port configs](./historical/port_realization_status_before_typed_port_configs.md).

## Next bottom-up connection

Complete type-owned storage and node-facing port access for the resolved continuous
type, starting with `FractionalGlobalIndex` and the canonical `FFTBlock` representation.
The existing Sample execution planner must not acquire central switches for those types:
each extension supplies its storage/access operations, while realization selects and
specializes one alternative before lowering. After that storage/access path works, carry
projected per-port extents into generated Tick bindings and storage planning.
