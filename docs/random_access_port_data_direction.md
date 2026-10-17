# Random-Access Port Data, Audio Value Types, And Input Contract Direction

_Status: planned port-contract and storage direction. This document supersedes the
page-based random-access sample representation and the requirement that data-bearing
sample coverage regions be disjoint in the older coverage/storage notes. The
canonical persisted store has moved to explicit sample regions/event chunks; the
node-facing `SampleRegion`/`SampleCoverage` API remains to be implemented._

Related documents:

- [Circular Global Timeline Indexing](./global_indexing.md)
- [Coverage, Random Access, And Background Evaluation](./coverage_and_background_evaluation.md)
- [Sequential Port Storage And Connection Planning](./sequential_port_storage_planning.md)
- [Graph JIT Direction](./graph_jit_direction.md)
- [DSP Execution And Storage Glossary](./dsp_execution_storage_glossary.md)
- [Node Interaction And Presentation Communication Direction](./node_interaction_and_presentation_communication_direction.md)

This direction has four goals:

1. make random-access audio data directly usable by algorithms that require
   contiguous buffers, without a page iterator or per-value accessor in the authored
   node API;
2. make the authored port configuration types express only valid contracts, while
   preserving the useful separation between input access, output production,
   retention, channel layout, and data kind;
3. extend continuous audio ports beyond scalar `iv::Sample` through the same style of
   closed registered type system already used for event and channel types, initially
   adding an FFT-block value type; and
4. make constructor-dependent value size and port pacing graph-resolved realization
   facts so GraphJit knows exact storage requirements before lowering.

## 1. Random-access sample data is Region or Coverage

Random-access sample data should no longer be exposed as a page store, a page
iterator, or a single-sample random accessor.

The authored/runtime-facing sample representation is instead one of two forms:

- **Region**: one logical range backed by one contiguous sample buffer;
- **Coverage**: a collection of zero or more Regions, where every Region independently
  owns/references one contiguous sample buffer.

A Region conceptually carries at least:

```text
logical begin/range
pointer to contiguous sample storage
frame/sample count
channel/layout information needed to interpret that storage
```

The exact C++ view type remains an implementation/API detail, but the contiguity
contract does not. A Region must be directly consumable by libraries such as FFT
implementations that require one contiguous input buffer.

Current mono/stereo and planar/interleaved layouts remain meaningful. Random-access
materialization should preserve a producer's useful/native channel layout where
possible rather than requiring an avoidable planar/interleaved conversion merely to
cross the port API. Future channel counts must fit the same model.

### Node callbacks receive contiguous views

Within both `tick*()` and `tock*()` contexts:

- a Region random-access sample input exposes one contiguous Region view;
- a Coverage random-access sample input exposes the Regions in its Coverage, with a
  pointer and size/range for each contiguous Region;
- node code should not need to walk storage pages;
- node code should not be forced through a single-sample random-access accessor when
  the underlying data is already contiguous.

The runtime/compiler may still use whatever ownership/indexing structures are useful
internally, but those structures are below the authored random-access contract.

## 2. Coverage regions may overlap

`Coverage` must stop requiring its Regions to be an exclusive/disjoint partition.
Overlap is valid and semantically meaningful.

For example, a Coverage may contain:

```text
Region A: [0, 100)
Region B: [50, 150)
```

without normalizing those Regions into disjoint pieces. A Coverage consumer receives
those Regions as separate contributions and is free to process them in that form.

Accordingly, `Coverage` is no longer adequately described as merely a canonical set
of covered positions. Region decomposition and multiplicity can matter because a
Sequential consumer combines overlapping contributions.

### Coverage ordering is an optional contract

A Coverage may additionally promise that its Regions are already sorted. Sorting is
not inherent to every Coverage because producers and consumers that do not need it
should not pay for it. **Whether a configured Coverage port is required/guaranteed to
be sorted is a realization property contributed through `constrain_ports()`, not a
fixed field of the static `constexpr` port schema.** The ordering constraint values are conceptually:

```cpp
enum class CoverageOrdering { unspecified, sorted };
```

The canonical sorted order is:

```text
primary:   lowest logical start index first
secondary: largest Region size first for equal starts
```

Equivalently, equal-start Regions are ordered by descending end position. Exact
duplicates need no stable relative order. Sortedness does **not** imply disjointness,
normalization, merging, or uniqueness: overlapping and duplicate Regions remain valid
contributions.

A single Region trivially satisfies a sorted-Coverage requirement. A consumer that
accepts unordered Coverage accepts either form. A consumer that requires sorted
Coverage may receive a naturally sorted producer directly; several individually
sorted fan-in producers can be combined with a k-way merge of Region descriptors
without copying their sample/audio backing. An unordered producer feeding a sorted
consumer requires descriptor sorting, but still does not require sample-value
materialization merely to establish order.

Ordering constraints must propagate through configured nodes as well as connections.
For example, a node that preserves descriptor order may constrain its output ordering
to equal the realized ordering of an input:

```cpp
void PreserveCoverageOrder::constrain_ports(
    ConstrainPortsContext<PreserveCoverageOrder>& ctx) const
{
    ctx.output<"out">().ordering() = ctx.input<"in">().ordering();
}
```

The input ordering can itself depend on the guarantees of the nodes connected to it,
and a downstream sorted requirement can therefore propagate transitively through an
arbitrary chain of order-preserving nodes. A node that intrinsically emits sorted
Coverage can anchor its output to `CoverageOrdering::sorted`; a node whose algorithm
requires sorted input can anchor the corresponding input requirement. The graph-wide
solver/planner joins these relations and inserts an ordering realization only where
the existing guarantees are insufficient. The exact constraint-lattice/proxy API is
left to the port-realization implementation, but sortedness must participate in the
same `constrain_ports()` solve as size, pace, history, and latency.

## 3. Sequential sample inputs define reduction semantics

A Sequential sample input is the place where multiple sample contributions are
collapsed into one ordered sample stream. Its config therefore owns both:

- the UI/default scalar metadata for the input; and
- the algebra used to combine fan-in and overlapping Coverage contributions.

The planned config shape is approximately:

```cpp
using SampleCombineValuesFn = Sample (*)(Sample, Sample);

struct SequentialSampleInputConfig {
    Sample default_value = 0.0;
    Sample min = -std::numeric_limits<Sample::storage>::infinity();
    Sample max = std::numeric_limits<Sample::storage>::infinity();

    Sample neutral_value = 0.0;
    SampleCombineValuesFn combine_values;
};
```

The field grouping is intentional:

```text
default_value
min
max

neutral_value
combine_values
```

Sequential input history is no longer part of this static `constexpr` access config.
History is a realization-dependent timing requirement contributed by
`constrain_ports()` after the configured node instance exists.

`default_value`, `min`, and `max` belong together and exist only on Sequential sample
inputs. They are presentation-relevant scalar metadata: the framework/default
presentation can use them to generate an appropriate parameter control without a
node-specific presentation definition.

`neutral_value` and `combine_values` also belong together. They define how the
Sequential input reduces multiple contributions.

The authored contract assumes:

```text
combine_values is associative
combine_values is commutative
neutral_value is its identity
```

For the usual additive audio input this is:

```text
neutral_value = 0
combine_values = addition
```

Other Sequential inputs may use multiplication or another operation satisfying the
same contract. Floating-point reassociation is allowed by the semantic contract;
bit-identical results across every legal reduction order are not implied.

### Fan-in and overlapping Coverage use the same combiner

The runtime does not need distinct semantics for:

- two connected producers contributing to one Sequential input; and
- two Regions in one producer Coverage overlapping the same requested position.

Both are sample contributions to the same Sequential input and are reduced with that
input's `combine_values` starting from `neutral_value`.

## 4. Coverage -> Sequential is bounded range rendering

A Coverage producer can directly feed a Sequential input. This conversion must not
materialize the Coverage's entire sparse logical extent.

For each bounded Sequential block/window requested by the consumer:

1. create/use only that bounded destination window;
2. initialize it to the input's `neutral_value`;
3. find every Region contribution intersecting that requested window;
4. combine the intersecting samples into the destination using `combine_values`;
5. leave positions with no contribution at `neutral_value`.

Thus overlapping Regions are combined rather than concatenated, and large gaps cost
no proportional sample storage.

For example, clips at `[0s,2s)` and `[4h,4h+1s)` do not imply a four-hour neutral-valued
buffer. A Sequential consumer may materialize only its current block or a bounded
prefetch window.

Background evaluation may choose to prepare perhaps dozens or hundreds of Sequential
blocks in advance so `tock*()` work is invoked less frequently when profitable. That
is a scheduling/cache choice. It does not change the semantic rule that storage is
bounded by the requested Sequential window rather than by the sparse Coverage hull.

## 5. Region/Coverage compatibility is asymmetric

Ignoring independent lifetime/recording requirements for the moment, the sample
shape/access compatibility is:

| Producer representation | Region input | Coverage input | Sequential input |
| --- | --- | --- | --- |
| Region | yes | yes, as one Region | yes |
| Coverage | **no** | yes | yes, by bounded reduction |
| Tick stream | effectively Region-shaped for its available block | yes when lifetime/access requirements are satisfied | yes |

A Region producer provides a stronger contiguity guarantee than a Coverage producer.
A Coverage producer cannot directly satisfy an input that requires one contiguous
Region. Doing so requires an explicit materialization/copy policy rather than an
implicit hidden copy.

This compatibility is only one axis of connection validity. Existing temporal and
lifetime constraints remain independent. In particular, a Sequential/Tick value may
still require an explicit recording/persistence policy before it can satisfy arbitrary
random-access lifetime requirements. The Region/Coverage distinction adds a
contiguity requirement; it does not erase the existing access/lifetime requirement.

## 6. Continuous port values use a closed registered type system

Continuous ports should not be permanently hard-wired to `iv::Sample`. The
application should use the same closed-registry/plugin pattern already used for event
types and channel types: adding a supported transported value type adds one deliberate
registry case together with its storage semantics and any deliberately supported
conversion semantics.

The initial extension remains deliberately narrow:

```text
Sample
FractionalGlobalIndex
FFTBlock
```

The registry has one canonical declaration list. Each declaration contains the C++
value type, stable runtime identity, diagnostic name, fixed rank and channel
capability. That one declaration generates the runtime ID, compile-time trait,
runtime descriptor and value-type-domain membership. Adding a continuous value type
must not require editing an enum, a descriptor array, a trait specialization and
consumer switches separately. Consumers select behavior through the generated
descriptor or through a type-owned storage/port policy; they do not switch on known
value-type enumerators.

Storage and port behavior that differs by value type belongs to the same extension as
new declarations or specializations owned by that type. A type extension may therefore
add its own storage and authored-access declarations, but it must not modify an existing
storage dispatcher merely to teach it another enumerator. Missing required behavior is
a compile-time or realization-time error at the registry boundary, not a fallback to
`Sample` behavior.

Each extension names one trivially copyable storage-element type, its alignment, valid
resolved dimensions, channel/audio capability, and—where audio-capable—the sample
support represented by one transported value. Generic realization code consumes those
extension-owned facts through the generated descriptor. It does not inspect a known
value-type enumerator. For the preliminary types, `Sample` stores `Sample`,
`FractionalGlobalIndex` stores `FractionalGlobalIndex`, and `FFTBlock` stores a
resolved-length array of `FFTBin`.

`FractionalGlobalIndex` is the scalar Q64.64 coordinate of the circular global
sample-index domain. It addresses multirate execution and continuous storage, and may
also be transported as ordinary graph data naming a coordinate. Its representation
retains the full integer global-index width and adds 64 fractional bits, so fractional
positions do not sacrifice the existing global-index range. `GlobalIndex` remains the
compact integer-coordinate subtype used where fractional positions are impossible.

Image/video values are explicitly deferred. They should not force premature answers
about image pacing, pixel layouts, channel semantics, or GraphJit video-kernel
optimization while the audio system is still being completed. `FractionalGlobalIndex` is not an
audio amplitude type, but it belongs in the same transported-value registry because it
uses the same port/storage/lowering machinery.

The initial direction does **not** add implicit conversions between registered value
types. A direct connection requires the source and target to resolve to the same value
type. When a representation change is useful, an explicit conversion/DSP node owns its
rounding, overflow, precision and other semantic choices. This is intentionally
conservative: a one-way implicit conversion such as `Sample -> FractionalGlobalIndex` can be
added later without changing either type's representation if experience shows that it
is universally useful. A semantic transform such as waveform <-> FFT remains a DSP
node rather than an implicit type conversion.

Sparse isolated values do not require a fourth continuous-data representation. If a
node needs sparse sample/value occurrences, define an event type carrying the value
and use the existing event-time/index semantics.

Procedural/randomly addressable computation likewise does not require another access
form. The existing constrained replayable-Tick mechanism is the current way to
recompute eligible Tick producers for arbitrary requested coverage. A possible
future `tack()`/`tack_block()` split may make replayability explicit, but it is not the
current direction.

### Type-dependent `size()` is a realization fact

Each registered continuous value type has a fixed rank known from its type definition,
but its concrete dimensions may be realization-dependent. Call this property simply
**size** rather than introducing a universal `format` object.

For the initial types:

```text
Sample
    scalar; no dynamic dimensions

FractionalGlobalIndex
    scalar; no dynamic dimensions

FFTBlock
    rank 1
    size = number of frequency-domain values in one block
```

### `FFTBlock` has a canonical library-neutral bin representation

One frequency-domain value in an `FFTBlock` is a complex bin. Its canonical C++
element type is:

```cpp
using FFTBin = std::complex<Sample::storage>;
```

Consequently, `FFTBlock::size()` counts complex bins. It does not count underlying
real scalars, a transform library's work-buffer elements, or a configured time-domain
transform length. The preliminary canonical layout represents a real-valued
time-domain transform as the complete nonredundant half-spectrum: exactly `N / 2 + 1`
explicit `FFTBin` values, including DC and Nyquist, for even transform length `N`.
Accordingly:

```text
time-domain transform length = 2 * (FFTBlock::size() - 1)
```

A 1024-sample transform therefore has `FFTBlock::size() == 513` and occupies 1026
`Sample::storage` scalars. The mathematically-zero imaginary components of DC and
Nyquist remain present. They are ordinary homogeneous `FFTBin` elements, not endpoint
fields with a special packed interpretation.

`std::complex<Sample::storage>` is deliberate rather than incidental ABI leakage from
one FFT implementation. The C++ complex-number contract guarantees a trivially
copyable value and array-oriented access to consecutive real/imaginary scalar pairs.
That gives graph storage one stable interleaved-complex representation which can be
passed directly to libraries whose public contract accepts the same representation,
and can be adapted without inspecting private library objects when a C API names an
equivalent complex type.

The canonical representation is not required to match every FFT library's native
working layout. Split-complex planes, packed real-transform arrays, library-internal
unordered spectra, padding, and scratch space are realization details at the FFT node,
not alternate meanings of `FFTBlock`. Those representations must not cross an
ordinary port connection, enter persisted Region/Coverage data, or become a runtime
layout tag carried by each value.

An FFT implementation selected for a compiled node realization must describe the
requirements relevant to direct use of graph storage:

- the native input and output layout of the transform;
- the required address alignment and any required padding;
- whether the operation is in-place or out-of-place and whether it destroys input;
- the amount and lifetime of auxiliary scratch storage; and
- the transform's scaling/normalization convention.

GraphJit may let that implementation consume or produce an `FFTBlock` in place only
when the canonical bin layout, resolved bin count, alignment, capacity, lifetime, and
mutation rules all satisfy the implementation's documented contract. Exact
`std::complex<Sample::storage>` APIs are the straightforward direct case. A
scalar-interleaved API may also be direct when its contract matches the C++
array-oriented representation. A split-complex API may be direct when it accepts the
corresponding strided view; otherwise it requires a split-plane materialization.
Packed or unordered real-transform formats—including the common 1024-scalar packed
representation of a 1024-sample real transform—require canonicalization before the
value is published to the graph.

When direct use is not legal, the compiled realization owns pre-sized transient
scratch and any required conversion operation. This is not an audio-thread fallback:
the transform plan, alignment, storage extent, and conversion path are chosen before
activation, and execution performs no dynamic allocation. The planner may co-locate
or reuse scratch with destination storage when ordinary alias, lifetime, and mutation
analysis proves that safe, but the node-facing `FFTBlock` view exposes only its
logical complex bins.

Backend-specific normalization is handled by the explicit transform node and its
owning IV package so that changing the chosen library does not change the graph value's
numerical meaning. A continuous-value extension does not own or select an FFT backend.
Likewise, direct compatibility must come from a C++ or library guarantee; equal
`sizeof` and `alignof` values alone do not authorize reinterpretation. This keeps
library interoperability and zero-copy execution available without making JUCE,
FFTW, vDSP, PFFFT, or any other backend's scratch representation part of the graph
type system.

Forward and inverse FFTs are explicit nodes. Core does not insert a transform because
a connection crosses between `Sample` and `FFTBlock`, and the `FFTBlock` extension
does not link an implementation library. An FFT node's IV package alone owns its
backend include files, native library dependency, plans, scratch and conversions.

Do not generate a distinct C++/registry type for every FFT size. `FFTBlock` remains
one registered value type; a particular 1024-sample real-transform realization
resolves `size() == 513`. The type implementation/GraphJit may still specialize kernels for a
finite set of useful FFT sizes internally without making those specializations part
of port type identity.

The resolved size must be known **before** GraphJit chooses transient stack backing,
aliases representations, or promotes large values to fixed persistent realtime
storage. A graph revision with an unresolved or contradictory required size fails
before lowering rather than falling back to audio-thread dynamic allocation.

### Static port shape and instance-dependent constraints stay separate

`inputs()` and `outputs()` remain `static constexpr`. They define the stable authored
interface shape that lets the framework instantiate concrete specialized context
classes for `tick*()`, `tock*()`, declaration, and port-constraint callbacks. A port
schema may declare either one registered value type or a **closed finite set** of
supported value types. The set is static even when the realized member is not.

Node callbacks therefore remain concrete functions, not `auto&` function templates.
The planned scalar/batch callback forms remain concrete as well: batch callbacks use
typed range contexts such as `TickBlockBatchContext<Node>` and
`TockCoverageBatchContext<Node>`, with trait-generated scalar/batch adapters defined in
[Batched Node Callback Direction](./batched_node_callbacks_direction.md).

A configured node instance may additionally define:

```cpp
void constrain_ports(ConstrainPortsContext<MyNode>& ctx) const;
```

This phase contributes constraints over the statically known ports without changing
port count, names, each port's declared finite value-type set, or callback context
shape. In addition to size/pace/history/latency/order constraints, a multi-type port
has a value-type constraint variable whose domain is exactly its declared set.

Direct connectivity equates source and target value type. Two ports whose declared
sets overlap can therefore propagate a selected type through graph connectivity. A
node can relate several of its own ports in `constrain_ports()` in the same way:

```cpp
ctx.equal(
    ctx.input<"in">().value_type(),
    ctx.output<"out">().value_type());
```

An instance may also anchor one alternative from configuration. The graph-wide solve
intersects all such finite domains and equalities. An empty intersection is a graph
constraint error; a value-type variable that is still ambiguous when lowering needs a
constructor/UI/module choice rather than an arbitrary compiler-selected default.

For example, an FFT node may anchor the size selected by its constructor:

```cpp
void FFT::constrain_ports(ConstrainPortsContext<FFT>& ctx) const
{
    ctx.output<"spectrum">().size() = fft_size_;
}
```

A size-preserving spectral processor does not need the FFT size repeated in its
constructor:

```cpp
void SpectralGain::constrain_ports(
    ConstrainPortsContext<SpectralGain>& ctx) const
{
    ctx.output<"out">().size() = ctx.input<"in">().size();
}
```

For the common two-sided case, the constraint proxies returned by `size()`, `pace()`,
`history()`, and `latency()` support assignment as equality shorthand. Assignment adds
a constraint; it does not mutate an already-resolved port value. `ctx.equal(...)` remains
the clearer primitive for N-way equality and accepts constraint variables and constants.
A node with two inputs and two outputs can express one equivalence class directly:

```cpp
ctx.equal(
    ctx.input<"a">().size(),
    ctx.input<"b">().size(),
    ctx.output<"x">().size(),
    ctx.output<"y">().size(),
    fft_size_);
```

If one call contains different constants, the constraint context can reject it
immediately. Contradictions that only become visible after graph connections and
other nodes' constraints are joined are graph-compilation errors. The initial solver
may be mostly equality classes plus constants; the mechanism deliberately leaves
room for registered type-specific constraints later when a real use case requires
them.

After the graph-wide solve, `declare()` observes concrete resolved sizes through its
specialized context, for example:

```cpp
auto const fft_size = ctx.input<"in">().size();
ctx.local_array<&State::scratch>(fft_size);
```

`declare()` consumes the solved result; it should not both create an unresolved size
relationship and depend on that same relationship having already been solved.

### Multi-type buffer access is explicit and specializes away

A port that declares exactly one value type keeps the ordinary concrete typed buffer
API. It should not acquire a variant-like interface merely because some other ports
support alternatives.

A port whose static schema declares more than one value type instead exposes explicit
type discrimination and typed access for only those declared alternatives, for
example:

```cpp
auto out = ctx.output<"out">();

if (out.is<iv::Sample>()) {
    auto samples = out.as<iv::Sample>();
    // ...
} else {
    auto positions = out.as<iv::FractionalGlobalIndex>();
    // ...
}
```

`is<T>()` / `as<T>()` are available only on a multi-type port, and only for `T` in
that port's declared finite set. By storage planning and LLVM lowering, however, every
realized port has exactly one selected concrete type. GraphJit makes that selection a
static constant in the generated realization so ordinary constant propagation and
dead-code elimination remove the unused authored branches. There is no per-value or
per-callback runtime variant dispatch requirement, and storage planning never allocates
an abstract union buffer: it sees the selected type's concrete element size, alignment,
rank and other storage semantics.

The same `constrain_ports()` phase also contributes exact per-port `pace()`,
Sequential-input `history()`, Tick-output `latency()`, and Coverage `ordering()`
constraints. These are realization facts rather than static port-schema fields.
Ordering may be anchored locally or related to another port so that sortedness can
propagate transitively through graph connections and order-preserving nodes. For an
overlap FFT configured
with transform size `N` and hop `H`, a complete-window implementation may constrain
`input.history() = N - H`; if it does not emit a zero-padded startup frame, it may also
constrain a positive output latency so the first authored FFT block can be finalized by
a later invocation. Dyadic pace resolution, audio sample-rate basis, `tick()` legality,
and per-port `tick_block()` sizes are specified in
[Graph JIT Direction](./graph_jit_direction.md#planned-value-type-port-size-pace-history-latency-and-coverage-ordering-constraint-analysis)
and [Sequential Port Storage And Connection Planning](./sequential_port_storage_planning.md#planned-pace-aware-tick_block-contract).

### `FractionalGlobalIndex` enables explicit coordinate sampling without changing pace

`FractionalGlobalIndex` values make fractional/random source addressing ordinary graph data.
A canonical resampler shape is:

```text
positions : Sequential FractionalGlobalIndex
source    : Random Access Sample
output    : Tick/ephemeral Sample
```

For each output position, the node samples `source` at the corresponding fractional
`FractionalGlobalIndex`, using the node's explicitly defined interpolation and out-of-coverage
policy. The position stream may increase, decrease, repeat, jump or otherwise warp
time. A rate/BPM signal can therefore feed an integral-like node that produces
`FractionalGlobalIndex`; negative rate naturally produces decreasing coordinates. Stutters,
scrubbing, variable-speed playback and granular/time-warping structures can be built
from the same coordinate interface.

This does not redefine port `pace()`. Pace still states an exact transport-rate
relationship and performs no resampling. `FractionalGlobalIndex` is instead a value whose
meaning is a coordinate used by nodes that explicitly choose to sample another input.

Curve authoring should likewise keep curve definition separate from evaluated value
representation. One curve clip/event representation may be sampled/evaluated into
either `Sample` or `FractionalGlobalIndex`. That can initially be exposed through separate
concrete evaluator nodes hidden behind one module/presentation choice, or through one
evaluator whose output declares `{Sample, FractionalGlobalIndex}` and resolves its selected type
from graph constraints. The curve editing/clip machinery itself need not be duplicated
solely because the eventual dense value type differs.

### FFT blocks keep the existing audio channel model

`ChannelTypeId` remains meaningful for both scalar sample streams and FFT-block
streams. The project does not need to make channel layout a universal property of all
future registered value types merely to support FFT data.

A stereo FFT is naturally represented as one planar FFT block per channel. GraphBuilder
may continue using its existing channel-type tiling machinery, so ordinary tiled
spectral nodes operate as one concrete channel member per tile. No alternate
frequency-interleaved FFT representation is required for the initial design.

Existing channel conversions remain aliasing or linear operations applied over the
FFT values/bins. In particular, averaging two FFT blocks is equivalent to FFT of the
averaged time-domain samples, apart from floating-point reassociation details. This
preserves room for GraphJit to move/fuse legal linear channel operations around FFT
boundaries when profitable without changing authored semantics.

Tiling is itself an explicit value-type constraint. Every continuous port duplicated
by a tiled node or tiled subgraph must resolve to a registered audio-capable value
type. For example, tiling intersects `{Sample, FractionalGlobalIndex}` with the
audio-capable domain and therefore selects `Sample`; tiling
`{Sample, FFTBlock}` leaves both alternatives for other graph constraints to resolve.
An empty intersection is diagnosed at realization. Event ports remain outside this
continuous-value constraint.

A scalar control shared by every tile is not a tiled port. It belongs outside the
tiled leaf and must be connected through an explicit shared/broadcast structure. This
keeps `FractionalGlobalIndex` scalar and prevents channel duplication or channel
conversion from becoming an accidental property of scalar coordinate values.

## 7. Authored continuous port configs are typed and toolable

`Node::inputs()` and `Node::outputs()` return heterogeneous tuples of concrete port
configuration types. Node authors declare registered continuous value types as
template arguments to concise `input<Types...>()` and `output<Types...>()` helpers.
Type erasure begins during reflection, after the authored declarations have been
statically validated; it is not present in a leaf node's declaration.

Access, production, and retention choices must be scoped so ordinary C++ completion
shows the legal values at each argument position. The API must not add unqualified
global objects named `sequential`, `region`, `coverage`, `tick`, `ephemeral`, or
`persisted`.

The intended spelling is:

```cpp
static constexpr auto inputs()
{
    return std::tuple{
        input<Sample>(
            "gain",
            InputAccess::sequential,
            {
                .default_value = 1.0f,
                .min = 0.0f,
                .max = 2.0f,
                .neutral_value = 0.0f,
            }),

        input<FractionalGlobalIndex>(
            "position",
            InputAccess::sequential,
            {.default_value = {}}),

        input<FFTBlock>(
            "spectrum",
            InputAccess::region),
    };
}

static constexpr auto outputs()
{
    return std::tuple{
        output<Sample>(
            "audio",
            OutputProduction::tick),

        output<FFTBlock>(
            "spectrum",
            OutputProduction::region,
            OutputRetention::persisted),
    };
}
```

`OutputRetention` is an ordinary scoped enum because retention is an independent data
axis and does not choose the C++ type of the output declaration:

```cpp
enum class OutputRetention {
    ephemeral,
    persisted,
};
```

Input access and output production do choose different concrete declaration types. A
normal enum value passed as a function argument cannot select a function's return
type. They are therefore scoped, enum-like typed tokens rather than unscoped globals:

```cpp
struct InputAccess {
    struct Sequential {};
    struct Region {};
    struct Coverage {};

    inline static constexpr Sequential sequential {};
    inline static constexpr Region region {};
    inline static constexpr Coverage coverage {};
};

struct OutputProduction {
    struct Tick {};
    struct Region {};
    struct Coverage {};

    inline static constexpr Tick tick {};
    inline static constexpr Region region {};
    inline static constexpr Coverage coverage {};
};
```

The distinct token types select distinct authored config types:

```text
SequentialInputConfig<Types...>
RegionInputConfig<Types...>
CoverageInputConfig<Types...>

TickOutputConfig<Types...>
RegionOutputConfig<Types...>
CoverageOutputConfig<Types...>
```

Consequently, Sequential-only options cannot appear on Region or Coverage inputs, and
Tick-only semantics cannot appear on Region or Coverage outputs. There is no authored
runtime `kind x access x production` variant. Event declarations remain separate
typed configurations and do not acquire continuous-value-only fields for symmetry.

The helper's template arguments determine the option types, so callers use designated
initializers without spelling framework-internal `*_options` type names. Each
registered continuous value extension defines the exact options meaningful for that
type. `default_value`, `min`, and `max` are scalar presentation metadata and are legal
only for rank-zero element types. A rank-zero type exposes only the meaningful subset:
`Sample` supports all three, while circular `FractionalGlobalIndex` supports a default
but no context-free minimum or maximum. Any type with a nonzero-dimensional `size()`
exposes none of these fields. Attempting to initialize an unsupported field is a
compile-time error.

A multi-type port lists one option initializer per configured type in the same order
as the adjacent template argument list. Omitting all option initializers selects every
extension's defaults:

```cpp
input<Sample, FractionalGlobalIndex>(
    "value",
    InputAccess::sequential,
    {.default_value = 0.0f, .min = -1.0f, .max = 1.0f},
    {.default_value = {}})

input<Sample, FractionalGlobalIndex>(
    "value",
    InputAccess::sequential)
```

Reflection visits these concrete tuple elements and produces the homogeneous,
type-erased configured-graph representation used by dynamic graph construction,
archives, RPC, and GraphJit. The configured graph records stable registered type
identities and the selected access/production semantics; it does not force the
authored declaration back into the invalid cross-product representation.

A configured Tick node contributes its required output `history()` and `latency()` in
`constrain_ports()`. A configured Sequential consumer similarly contributes its
history requirement there. This allows constructor values and graph-resolved port
sizes and paces to determine temporal windows without changing the static callback
shape.

`CoverageOrdering` remains a configured-realization constraint rather than a static
field of the authored config. A Coverage input may constrain the ordering it requires,
a Coverage producer may constrain the ordering it guarantees, and an
ordering-preserving node may relate the two in `constrain_ports()`.

## 8. Sequential reduction is configurable per input

Every registered continuous value extension, regardless of rank, supplies a default
Sequential neutral value or neutral-initialization operation and a default
`combine_values` operation.
Those defaults make ordinary additive inputs concise. They do not fix every input to
addition. Each authored Sequential input can override both parts of its reduction
algebra.

For a scalar `Sample` input, the intended syntax is:

```cpp
input<Sample>(
    "gain_product",
    InputAccess::sequential,
    {
        .neutral_value = 1.0f,
        .combine_values = &multiply_samples,
    })
```

The authored contract remains:

```text
combine_values is associative
combine_values is commutative
neutral_value is its identity
```

The operation belongs to the individual Sequential input because fan-in meaning is
operation-dependent. The continuous value extension owns the typed option and
executable representations. It must not force every logical value type into the
scalar `Sample(Sample, Sample)` shape.

For a dynamically sized logical value such as `FFTBlock`, the static declaration
cannot contain a scalar-style default or a complete resolved-size neutral block. Its
extension instead types `neutral_value` as a non-owning initializer operation over one
resolved logical block and `combine_values` as an operation over two resolved logical
blocks. Realization supplies the resolved bin count before neutral initialization, and
the initializer is invoked once for each logical value in the destination storage.
This permits a neutral block to depend on its resolved size and bin position. It is not
restricted to one repeated bin value. The authored field names retain their semantic
meaning while the extension supplies their valid types and lowering behavior. The
superseded uniform-bin interpretation is preserved in
[historical documentation](./historical/fft_uniform_neutral_fill.md).

The materialized executable configuration contains only the trivially copyable values
and ordinary function/operation references needed by generated runtime/JIT code. It
does not store `std::function` or another owning, runtime-erased callable. A configured
combiner target is compiled into the node/package LLVM module and relocated normally.
Whole-graph LLVM can specialize, devirtualize, or inline a statically known operation.

Frontend/reflection metadata does not need to serialize or expose executable function
pointers. It records the presentation and value metadata that is semantically useful
outside the generated DSP module. Both the dynamically sized neutral initializer and
the block combiner remain executable behavior in the compiled node implementation.
Their configured metadata alternative is therefore stateless.

## 9. Persisted and ephemeral random-access sample data stop using pages

The target architecture should remove page-based sample storage for Tock-generated
and other random-access sample data.

Persisted versus ephemeral continues to specify lifetime/retention, but neither
contract implies fixed-width pages. The retained/materialized values are Region or
Coverage data with contiguous storage per Region.

Consequences include:

- node-facing APIs no longer expose or depend on page width/page indices;
- persisted storage preserves Region/Coverage data and its contiguous backing rather
  than canonical fixed-width sample pages;
- ephemeral Tock results use the same Region/Coverage shape contract with shorter
  lifetime/ownership;
- sparse logical gaps consume no neutral-filled backing merely because distant
  Regions share one output coordinate space;
- FFT/convolution and similar consumers can require Region input and pass its
  contiguous storage directly to libraries that require it.

The executor-owned `PersistedDataStore` provides the accepted retained
owner/versioning/publication boundary. It publishes one coherent immutable root whose
sample outputs are indexed contiguous `PersistedSampleRegion` records and whose event
outputs are indexed explicit-range `PersistedEventChunk` records. That internal
retained representation does not preempt the node-facing `SampleRegion` and
`SampleCoverage` API defined here: the store converts/binds retained sample regions to
that port shape at the runtime boundary.

## 10. Region count is evaluation data, not realtime layout data

The number of Regions in a Coverage is not assumed to be available during
`declare()` and should not become a `NodeLayout` sizing input merely to support
background processing.

Connectivity/fan-in arity may be a realization fact and can legitimately affect
packed realtime `State`. Region decomposition depends on evaluated/random-access
contents and belongs to background work.

If a node needs one background object per Region, its separately owned `TockState`
can dynamically resize during the appropriate `tock*()` callback:

```text
coverage/content changes
    -> tock*() observes the new Region set
    -> TockState resizes/rebuilds acceleration data
```

This does not change `NodeLayout` or force `NodeStorage` replacement.

## 11. Migration boundary from the current implementation

The current repository's canonical persisted store now uses contiguous
`PersistedSampleRegion` records and explicit-range `PersistedEventChunk` records.
Node-facing random-access sample APIs still use positional `IndexCoverage` plus
per-value access rather than the `SampleRegion`/`SampleCoverage` contract described
here. That remaining boundary is the next representation migration; the
store-internal record type is not a substitute for the authored port API.

Migration should preserve the broader architectural invariants already established:

- the audio thread does not block on background evaluation;
- random-access materialization/publication occurs off the audio thread;
- `TockState` remains separately owned background state and may allocate dynamically;
- `NodeStorage` remains the packed fixed-layout audio-thread realization;
- output retention remains independent of Tick/Tock production;
- connection validation rejects requirements the producer cannot satisfy directly
  unless an explicit adaptation/recording/materialization policy exists.

Persisted-data ownership/version publication remains an executor concern. Binding it
to the future node-facing views must not reintroduce storage record boundaries into
the authored port contract.
