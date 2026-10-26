# Per-Port Audio Sample-Rate Basis Proposal

_Status: historical, non-normative. Preserved on 2026-10-16 from the Front 1
continuous-port design before the project sample-rate decision was simplified._

_Provenance:_ the superseded text appeared in `graph_jit_direction.md`,
`sequential_port_storage_planning.md`, `dsp_execution_storage_glossary.md`,
`batched_node_callbacks_direction.md`, `implementation_fronts_direction.md`, and
`port_realization_migration_status.md`.

_Reason for supersession:_ the earlier proposal made an audio sample-rate basis a
separate graph-resolved, constrainable property of every audio-capable port. It also
proposed that a node-wide `ctx.sample_rate` be available only when all relevant ports
shared that resolved basis, and that an oversampler change the basis seen by its output.
The accepted initial design now keeps one project/execution sample rate and constrains
only port pace. Fractional sample-rate conversion and a future representation of
sample-rate domains are explicitly out of scope for the current front.

_Replacement:_ see the pace and continuous-value sections in
[`graph_jit_direction.md`](../graph_jit_direction.md),
[`sequential_port_storage_planning.md`](../sequential_port_storage_planning.md), and
[`dsp_execution_storage_glossary.md`](../dsp_execution_storage_glossary.md).

## Preserved proposal

The previous design distinguished three resolved facts:

- an audio port's physical sample-rate basis;
- its transported-value cadence; and
- the amount of time-domain sample support represented by one value.

It proposed graph constraints that preserved or transformed the first fact. Under that
proposal, an FFT output could have a slow block cadence while retaining the input
signal's audio basis, whereas a 2x oversampler would transform an input basis of
48 kHz into an output basis of 96 kHz. Connections would require compatible bases.
Declaration, initialization, Tick, Tock, and batching would all receive the resolved
basis. A node-wide `ctx.sample_rate` convenience would be legal only for a realization
whose relevant audio ports shared one basis; other nodes would read port-specific
basis data.

For an overlap FFT of length 1024 and hop 512, the proposal described the input as
48,000 scalar values per second and the output as 93.75 FFT values per second while
both represented a 48 kHz signal. One FFT value had 1024 samples of support even though
consecutive values advanced by 512 samples.

The proposal also included the resolved sample-rate basis in callback batch
compatibility. It treated implementation of basis solving and context propagation as
part of Front 1.

## Accepted replacement

The current initial design keeps these meanings separate without making all three
graph constraints:

- `ctx.sample_rate` is the unchanged project/execution sample rate.
- Port pace is the only constrained cadence property. It determines the lattice and
  the number of transported values that fall in an execution interval.
- Per-value sample support remains type-owned geometry. For example, an `FFTBlock`
  with 513 complex bins represents a 1024-sample real transform.

An FFT port can therefore transport fewer values than a Sample port while both node
callbacks still receive the same project sample rate. A 2x paced Sample port transports
twice as many values in the same project-time interval; that pace fact does not change
`ctx.sample_rate` and does not imply resampling. If future DSP needs distinct physical
sample-rate domains, that feature must introduce an explicit design and explicit
resampling semantics rather than reinterpreting pace.
