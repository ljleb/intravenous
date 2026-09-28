# Random-Access Port Data, Audio Value Types, And Input Contract Direction

_Status: planned port-contract and storage direction. This document supersedes the
page-based random-access sample representation and the requirement that `Coverage`
regions be disjoint in the older coverage/storage notes. The checked-in runtime still
contains page-backed implementations while this migration is in progress._

Related documents:

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
should not pay for it. The canonical sorted order is:

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

## 6. Continuous audio values use a closed registered type system

Continuous audio ports should not be permanently hard-wired to `iv::Sample`. The
application should use the same closed-registry/plugin pattern already used for event
types and channel types: adding a supported transported value type adds one deliberate
registry case together with its storage semantics and legal conversions.

The initial scope is intentionally audio-only:

```text
Sample
FFTBlock
```

Image/video values are explicitly deferred. They should not force premature answers
about image pacing, pixel layouts, channel semantics, or GraphJit video-kernel
optimization while the audio system is still being completed.

Connections require a defined conversion between their registered value types. The
conversion relation must remain coherent under composition: if values of `A` and `C`
can both convert to `B`, both may contribute to one `B` input; if more than one path
can convert one source type to one target type, those paths must not create
path-dependent semantics. A semantic transform such as waveform <-> FFT is a DSP
node, not an implicit type conversion.

Sparse isolated values do not require a fourth continuous-data representation. If a
node needs sparse sample/value occurrences, define an event type carrying the value
and use the existing event-time/index semantics.

Procedural/randomly addressable computation likewise does not require another access
form. The existing constrained replayable-Tick mechanism is the current way to
recompute eligible sequential producers for arbitrary requested coverage. A possible
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

FFTBlock
    rank 1
    size = number of frequency-domain values in one block
```

Do not generate a distinct C++/registry type for every FFT size. `FFTBlock` remains
one registered value type; a particular graph realization resolves, for example,
`size() == 2048`. The type implementation/GraphJit may still specialize kernels for a
finite set of useful FFT sizes internally without making those specializations part
of port type identity.

The resolved size must be known **before** GraphJit chooses transient stack backing,
aliases representations, or promotes large values to fixed persistent realtime
storage. A graph revision with an unresolved or contradictory required size fails
before lowering rather than falling back to audio-thread dynamic allocation.

### Static port shape and instance-dependent constraints stay separate

`inputs()` and `outputs()` remain `static constexpr`. They define the stable authored
interface shape that lets the framework instantiate concrete specialized context
classes for `tick*()`, `tock*()`, declaration, and port-constraint callbacks. Node
callbacks should therefore be concrete functions, not `auto&` function templates.
The planned scalar/batch callback forms remain concrete as well: batch callbacks use
typed range contexts such as `TickBlockBatchContext<Node>` and
`TockCoverageBatchContext<Node>`, with trait-generated scalar/batch adapters defined in
[Batched Node Callback Direction](./batched_node_callbacks_direction.md).

A configured node instance may additionally define:

```cpp
void constrain_ports(ConstrainPortsContext<MyNode>& ctx) const;
```

This phase contributes constraints over the statically known ports without changing
port count, names, registered value types, or callback context shape.

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

The same `constrain_ports()` phase also contributes exact per-port `pace()`,
Sequential-input `history()`, and Tick-output `latency()` constraints. These are
realization facts rather than static port-schema fields. For an overlap FFT configured
with transform size `N` and hop `H`, a complete-window implementation may constrain
`input.history() = N - H`; if it does not emit a zero-padded startup frame, it may also
constrain a positive output latency so the first authored FFT block can be finalized by
a later invocation. Pace resolution, effective local sample rate, `tick()` legality,
and per-port `tick_block()` sizes are specified in
[Graph JIT Direction](./graph_jit_direction.md#planned-port-size-pace-history-and-latency-constraint-analysis)
and [Sequential Port Storage And Connection Planning](./sequential_port_storage_planning.md#planned-pace-aware-tick_block-contract).

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

## 7. Sample port config types should make invalid states unrepresentable

The current `InputConfig` representation combines a sample/event `kind` variant with
an independent `InputAccessConfig`. That cross-product becomes undesirable once only
Sequential sample inputs own `default_value`, `min`, `max`, `neutral_value`, and
`combine_values`.

The planned sample-input representation is instead approximately:

```cpp
struct SequentialSampleInputConfig {
    Sample default_value = 0.0;
    Sample min = -std::numeric_limits<Sample::storage>::infinity();
    Sample max = std::numeric_limits<Sample::storage>::infinity();

    Sample neutral_value = 0.0;
    SampleCombineValuesFn combine_values;
};

enum class CoverageOrdering { unspecified, sorted };

struct RegionSampleInputConfig {};
struct CoverageSampleInputConfig {
    CoverageOrdering ordering = CoverageOrdering::unspecified;
};

using SampleInputAccessConfig = std::variant<
    SequentialSampleInputConfig,
    RegionSampleInputConfig,
    CoverageSampleInputConfig>;

struct SampleInputConfig {
    std::string name;
    ChannelLayout channel_layout;
    SampleInputAccessConfig access;
};
```

Using distinct Region/Coverage alternative types is preferable to introducing a
nested enum such as `RandomAccessForm`: the variant itself expresses the alternatives
and prevents meaningless Region/Coverage fields from existing.

At the top level, sample and event declarations should likewise be distinct config
alternatives rather than a `kind x access` cross-product when their valid access
configuration differs:

```cpp
using InputConfig = std::variant<SampleInputConfig, EventInputConfig>;
```

The exact event-side access types may remain as appropriate for event semantics; they
do not acquire sample-only reduction/UI fields merely for symmetry.

### Sample outputs

Tick history/latency remains meaningful only for Tick production, but it is not part of
the static `constexpr` output-production config. Tock sample outputs select whether
their random-access result is one Region or a Coverage:

```cpp
struct TickOutputConfig {};

struct RegionTockOutputConfig {};
struct CoverageTockOutputConfig {
    CoverageOrdering ordering = CoverageOrdering::unspecified;
};

using SampleOutputProductionConfig = std::variant<
    TickOutputConfig,
    RegionTockOutputConfig,
    CoverageTockOutputConfig>;

struct SampleOutputConfig {
    std::string name;
    ChannelLayout channel_layout;
    SampleOutputProductionConfig production;
    OutputRetention retention = OutputRetention::ephemeral;
};
```

A configured Tick node contributes its required output `history()` and `latency()` in
`constrain_ports()`. This allows constructor values and graph-resolved port sizes/paces
to determine the temporal window without changing the static callback shape.

`OutputRetention::{ephemeral,persisted}` remains an independent axis. Both Region and
Coverage Tock outputs may be ephemeral or persisted.

The exact final naming of the Region/Coverage alternatives and ordering enum can
change, but the type separation is the intended contract. A Coverage input uses the
ordering property as a consumer requirement; a Coverage producer uses it as a producer
guarantee.

## 8. Sequential combine helpers select the operation statically

Authored node helpers should make `combine_values` a template-selected operation,
while the materialized config contains the raw function pointer expected by generated
runtime/JIT code.

Conceptually:

```cpp
template<auto Combine = std::plus<Sample>{}>
constexpr InputConfig sequential_sample_input(/* ... */)
{
    SampleCombineValuesFn fn = +[](Sample a, Sample b) -> Sample {
        return Combine(a, b);
    };

    // Build SequentialSampleInputConfig with fn.
}
```

If `Combine(a, b)` cannot be used as `Sample(Sample, Sample)`, the helper does not
compile. Node authors therefore get an ergonomic template interface without storing a
runtime-erased callable such as `std::function` in the config.

Node/package code is compiled to LLVM. The resulting `combine_values` target is a
function in that compiled/JIT-linked module, so the executable config can use an
ordinary function pointer after relocation. It is not an authored string identifier
or a raw JSON-level operation enum. Whole-graph LLVM may also be able to
specialize/devirtualize/in-line a statically known combiner such as addition.

Frontend/reflection metadata does not need to serialize or expose the executable
function pointer. It needs the scalar metadata (`default_value`, `min`, `max`, etc.)
that is semantically useful outside the generated DSP module.

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

The exact owner/versioning/publication representation that replaces the current
persisted page store is still an implementation-design problem. This direction only
requires that its node-facing/random-access semantic unit is Region/Coverage rather
than fixed pages.

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

## 11. Migration boundary from the checked-in implementation

The current repository still contains page-backed persisted/random-access sample
storage and APIs designed around disjoint canonical Coverage. Those are implementation
checkpoints, not the target contract described here.

Migration should preserve the broader architectural invariants already established:

- the audio thread does not block on background evaluation;
- random-access materialization/publication occurs off the audio thread;
- `TockState` remains separately owned background state and may allocate dynamically;
- `NodeStorage` remains the packed fixed-layout audio-thread realization;
- output retention remains independent of Tick/Tock production;
- connection validation rejects requirements the producer cannot satisfy directly
  unless an explicit adaptation/recording/materialization policy exists.

The concrete replacement for current page-store ownership/version publication can be
designed separately without reintroducing pages into the authored node contract.
