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

The accepted authored syntax distinguishes the two meanings without introducing a
second top-level port property:

```cpp
port.pace() = local_power_of_two;
port.pace().absolute() = global_power_of_two;
```

The direct assignment constrains transported values per logical node step. The chained
assignment anchors the resolved graph-wide pace of that port. After local ratios and
connections are solved, an unanchored connected component defaults only its one uniform
absolute scale to one. The solver does not use that default to invent an unresolved
internal ratio.

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
  circular Q64.64 coordinate domain for node-visible execution positions and
  transported coordinates. Physical Sequential storage uses separate compiler-owned
  linear cursors. A finite range projects onto a resolved lattice, including
  zero-value slow-port projections and wraparound.
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
  parallel Sample-only projection fields have been removed. Continuous storage
  planning reads the same type-owned metadata for every selected extension.
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
  rather than `Sample`. Several execution/storage planner identifiers and filenames
  remain sample-named, but their selected geometry and value-count arithmetic now serve
  all registered continuous types. Renaming that terminology is maintenance work, not
  a missing parallel realization.
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
- Graph and subgraph boundary inputs now use the same registered continuous-value
  domain as leaf Sequential inputs. A boundary can declare one or several concrete
  value alternatives with type-owned boundary options, and the configured boundary
  retains that complete value-type domain and presentation/default metadata. Boundary
  options deliberately exclude `neutral_value` and `combine_values`: a boundary is a
  structural external source, not the leaf input that reduces connected producers.
  Each extension translates its boundary options into configured Sequential metadata
  using its default reduction algebra. Omitting the value type selects the first
  extension in the sole continuous-value registry; a compile-time invariant currently
  requires that extension to be `Sample`. The overload that accepts Sample
  default/minimum/maximum arguments remains Sample-specific authoring sugar; it is no
  longer the implementation path that defines what boundary inputs can represent.
- The VS Code graph model consumes the continuous introspection schema and displays
  declared types and bindings. The former read-only knobs and state actions were
  removed because no runtime read or mutation path supplied their values.
- The unit tests that formerly constructed timing-bearing legacy configs or invoked the
  old connection-analysis signature have been migrated to authored constraints and
  explicit resolved-realization input.
- GraphJit continuous storage planning now uses each selected extension's resolved
  element size, alignment, elements per transported value, channel capability, and
  maximum projected value count. `Sample`, `FractionalGlobalIndex`, and `FFTBlock`
  therefore use the same representation planner without a central per-type storage
  switch.
- Generated Tick bindings retain each port's selected geometry and absolute pace. A
  callback's root `GlobalIndexRange` is projected independently onto every continuous
  port lattice, so fast ports receive several transported values per project sample,
  slow ports may legally receive zero values, and `value_index()` names the projected
  port-domain ordinal.
- Node-facing single-type ports expose their concrete type-owned access directly.
  Multi-type ports expose only their declared `is<T>()` / `as<T>()` alternatives. The
  generated binding is initialized from immutable realization bytes, selected package
  callbacks are imported into the project LLVM module, and the whole module is
  optimized after package O0/no-inline barriers are removed. The selected value type
  is therefore available as a realization constant for inlining and dead-code
  elimination rather than requiring per-value variant dispatch.
- Sequential continuous inputs expose a projected `block()` and a contiguous
  history-plus-current `block_extended()` view. The storage planner aliases an already
  contiguous representation where possible and otherwise reserves bounded
  invocation-local scratch for the selected projected value extent. Dynamic
  `FFTBlock` values preserve their resolved bin count within that same access path.
- Pointwise `tick()` lowering accepts only one graph-proven continuous pace for the
  relevant ports and project pace when integer-timed events participate. Native
  `tick_block()` callbacks may observe heterogeneous per-port projections directly.
  Contiguous input-window preparation is compiled only into authored block callbacks;
  pointwise `tick()` nodes and generated default skip behavior do not pay that work.
- Runtime GraphJit coverage now exercises an `FFTBlock` producer/consumer with slow
  zero-value callbacks, history-inclusive contiguous access, and a multi-type input;
  it also exercises a fast `FractionalGlobalIndex` producer/consumer. These tests were
  added after the complete storage/access path took shape.

## Incomplete or downstream-owned milestones

- Source introspection cannot report the selected value type, resolved dynamic size,
  or live runtime value. Those facts become available only after GraphJit realization
  and require the planned ProjectGraph/runtime read-model join. This is a runtime
  read-model feature, not missing constraint or Tick-storage realization.
- Tick/persisted capture still publishes the scalar `Sample` recording representation.
  Graph lowering now rejects a non-Sample or non-project-rate Tick/persisted output at
  that exact boundary instead of rejecting ordinary ephemeral non-Sample execution or
  letting the current capture callback silently skip an unsupported binding.
  Generalized retained-data capture and publication belong to Front 5.
- Contextual background Tick replay still realizes its temporary continuous bindings
  as project-rate `Sample` rings. Connection analysis rejects a non-Sample or
  non-project-rate continuous replay binding immediately after contextual replay proof,
  before publishing a background runtime plan. Existing Sample replay allocates
  contiguous input scratch only for an authored `tick_block()` callback; pointwise
  replay continues to read the bounded ring directly. Generalized background replay
  invocation and temporary continuous storage belong to Front 7, consuming Front 1's
  resolved port realization and Front 3's eventual Region/Coverage materialization;
  they do not belong to the port-realization front itself.
- Playback-paused substitution of background-produced continuous inputs with their
  neutral values, background-produced event inputs with empty blocks, and
  Region/Coverage inputs with empty extents belongs to the background-to-realtime
  publication boundary. Front 1 supplies the continuous neutral operation and the
  invocation-contiguous Tick storage it consumes; it does not yet implement transport
  state or background snapshot selection.
- Coverage ordering is solved and propagated among Coverage endpoints. Region ports
  carry no ordering variable because a finite Region is intrinsically ordered. The
  Region/Coverage realization that satisfies a sorted Coverage requirement remains
  owned by Front 3.
- Distinct physical sample-rate domains and fractional resampling remain deliberately
  deferred. Port pace alone controls transported-value cadence in the current front;
  the project sample rate remains one unchanged callback context parameter.

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

The discontinuity rule is now settled. Tick and Tock keep the same node-visible
`FractionalGlobalIndex`. Tick runs continuously while playback is paused; resume/seek
may jump its next global coordinate. Sequential storage and history follow hidden
invocation-order cursors per used dyadic pace, so they remain adjacent across that
global seam. A slow port's post-seam values still use the newly selected global lattice
phase. This removes the earlier unresolved need for a second public time type.

Generated continuous execution now implements that separation across primitive
bindings, materialization, carry, feedback, output revision, capture reads, and
generation migration. The root loads one cumulative cursor for each used dyadic pace,
derives slice-local physical beginnings from the projected root prefix, and advances
each cursor once per complete callback. It also loads one saturating authored extent
for each producer whose revision horizon feeds detached feedback. Every branch from
that producer reuses the same loaded extent; SCC slices add only their root-prefix
projection. This preserves initialized detach prehistory without per-detach warm-up
state or repeated cursor/extent loads. Pointwise `tick()` applies its current value
offset to each binding's own physical beginning rather than borrowing the first port's
address; an overlaid persisted/materialized input ring and a live output ring may
therefore have different physical origins. Detached-revision availability continues to
use the semantic root-prefix coordinate even after copy/composition code has selected a
physical storage beginning.

Runtime coverage includes both project-rate history/latency across a discontinuous
global jump and a slow `FFTBlock` lattice whose node-visible phase jumps while its
Sequential history remains the previously transported FFT value. A focused helper test
also gives pointwise input and output bindings different physical beginnings and proves
that the common value offset is applied independently to both. Sequential events still
need their explicit timestamp/seam mapping; that named dependency does not block
continuous Front 1 completion. The superseded direct-global-storage interpretation is
preserved in
[Global Timeline As Sequential Storage Address](./historical/global_timeline_as_sequential_storage_address.md).

The accepted initial resolution keeps the project sample rate unchanged and constrains
only transported-value pace. An FFT output may have a slow block cadence while
`ctx.sample_rate` remains the project rate, and the FFT extension supplies the number
of project samples represented by each value. Distinct physical sample-rate domains
and fractional resampling are deferred. The superseded per-port sample-rate-basis
proposal is preserved in
[Per-Port Audio Sample-Rate Basis Proposal](./historical/per_port_audio_sample_rate_basis_proposal.md).

The superseded unrestricted-rational and provisional SDF proposal is preserved with
its provenance in
[Rational Port Pace And SDF Scheduling](./historical/rational_port_pace_and_sdf_scheduling.md).

## Expected repository state

The complete Front 1 working-tree implementation, including type-owned Tick/Sequential
storage, per-port dyadic projection, and physical Sequential continuity across global
transport seams, is intended to compile as one coherent feature, but its latest runtime
tests and persisted-boundary diagnostic have not been built or run by the implementation
agent. The repository owner performs that validation. No
compatibility aliases or wrappers preserve the removed Sample-only authored-config API.
Earlier incomplete checkpoints are preserved in
[Port-realization status before typed authored port configs](./historical/port_realization_status_before_typed_port_configs.md)
and
[Port-realization status before type-owned Tick execution](./historical/port_realization_status_before_type_owned_tick_execution.md).

## Front 1 completion boundary

Front 1's graph-wide constraint solve, resolved continuous geometry, per-port dyadic
Tick projection, physical Sequential cursor realization, Sequential history
materialization, detached-revision startup validity, and typed node-facing access are
implemented as one path for `Sample`, `FractionalGlobalIndex`, and `FFTBlock`. Subject
to owner build/test validation, no dependency-ready continuous Front 1 execution layer
remains to be connected.

The remaining items above deliberately stay with their consuming fronts. Front 3 owns
Region/Coverage data realization. Front 5 owns generalized retained-data capture and
publication. The ProjectGraph/runtime read-model join owns realized live
introspection. Optional FFT packages own transform/window/overlap algorithms; core
Front 1 owns only the canonical `FFTBlock` and the execution/storage contract that
transports it.

## FFT implementation package boundary

Core Intravenous owns the canonical `FFTBlock` representation and the generic storage,
constraint, scheduling and port-access machinery needed to transport it. Core does not
own an FFT algorithm, an FFT plan abstraction, an FFTW include, or an FFTW link
dependency. Forward and inverse transforms are explicit nodes rather than implicit
port conversions.

The optional `src/intravenous/builtin_packages/fftw` IV package owns the first
executable forward and inverse real-transform nodes. Only that package includes
`fftw3.h`. Its package-local CMake downloads the official FFTW consumer release into
the package build directory and provides the resulting native shared library to that
package's ORC JITDylib. The application, `iv_builder`, GraphJit and the default
`builtin` IV package remain FFTW-free. Unlike `builtin`, the `fftw` sibling is not
staged into the application's default package search root. Merely building or starting
the application therefore does not configure, download, discover, or load FFTW; the
user must select the FFTW package through the ordinary package search/build mechanism.

Within that package, FFTW plan creation and destruction occur off the realtime thread.
Realtime callbacks execute only already-created plans and preallocated native buffers.
The package converts between FFTW's native buffers and the canonical `FFTBlock` bins,
and its inverse node applies FFTW's required normalization. A future project-owned or
permissively licensed FFT package can provide different explicit transform node IDs
without changing `FFTBlock` or core execution.

The package uses one native adapter library to own FFTW plans, buffers, and the shared
planner lock. Hot-loaded revisions of the package's LLVM node code resolve through that
same adapter, so overlapping graph generations cannot call FFTW's non-thread-safe
planner under independent locks. The current loader retains native package libraries
for the process lifetime. Consequently, edits to `module.cpp` hot-reload normally, but
an edit to `backend.cpp` requires a server restart before the replacement native
adapter can take effect. This is an explicit current native-sidecar limitation, not a
requirement of `FFTBlock` or of the transform-node semantics.

The rejected core-owned backend wording and implementation boundary are preserved in
[Superseded Core-Owned FFTW Backend Proposal](./historical/core_owned_fftw_backend_proposal.md).

## Planned paired STFT module

The accepted final low-level forward and inverse FFT nodes take explicit power-of-two
transform size `N` and hop size `H` construction arguments. Hop size is an algorithm
parameter, not a free graph-pace variable: their local pace constraints describe `H`
after construction, and ordinary connection solving scales the complete pair into any
surrounding absolute pace domain.

The current preliminary FFTW nodes take only `N` and therefore realize `H == N`.
Supporting `H < N` is package-level unfinished work, not missing port realization. The
inverse implementation must first settle analysis/synthesis window ownership,
overlap-add accumulation, and reset/reconstruction across a discontinuous project
timeline. Adding an `H` constructor argument while retaining the current non-overlap
algorithm would be false API completion and is deliberately avoided.

After `SubgraphClosure` exists, the FFTW package may add a paired STFT module that takes
`N`, `H`, inverse/window configuration, and one spectral-processing closure. It creates
the explicit forward node, invokes the closure on its `FFTBlock` boundary, creates the
explicit inverse node with the same `N` and `H`, and wires the result. This removes
duplicated user configuration without embedding FFT execution in ports or teaching the
generic constraint solver to infer hop size. Until then, users construct the two
low-level nodes directly; the preliminary implementation supports the non-overlap
`H == N` case.
