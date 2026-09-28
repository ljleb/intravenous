# Random-Access Port Data And Sample Input Contract Direction

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

This direction has two goals:

1. make random-access sample data directly usable by algorithms that require
   contiguous buffers, without a page iterator or per-sample accessor in the authored
   node API; and
2. make the authored port configuration types express only valid contracts, while
   preserving the useful separation between input access, output production,
   retention, channel layout, and data kind.

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

## 3. Sequential sample inputs define reduction semantics

A Sequential sample input is the place where multiple sample contributions are
collapsed into one ordered sample stream. Its config therefore owns both:

- the UI/default scalar metadata for the input; and
- the algebra used to combine fan-in and overlapping Coverage contributions.

The planned config shape is approximately:

```cpp
using SampleCombineValuesFn = Sample (*)(Sample, Sample);

struct SequentialSampleInputConfig {
    std::size_t history = 0;

    Sample default_value = 0.0;
    Sample min = -std::numeric_limits<Sample::storage>::infinity();
    Sample max = std::numeric_limits<Sample::storage>::infinity();

    Sample neutral_value = 0.0;
    SampleCombineValuesFn combine_values;
};
```

The field grouping is intentional:

```text
history

default_value
min
max

neutral_value
combine_values
```

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

## 6. Sample port config types should make invalid states unrepresentable

The current `InputConfig` representation combines a sample/event `kind` variant with
an independent `InputAccessConfig`. That cross-product becomes undesirable once only
Sequential sample inputs own `default_value`, `min`, `max`, `neutral_value`, and
`combine_values`.

The planned sample-input representation is instead approximately:

```cpp
struct SequentialSampleInputConfig {
    std::size_t history = 0;

    Sample default_value = 0.0;
    Sample min = -std::numeric_limits<Sample::storage>::infinity();
    Sample max = std::numeric_limits<Sample::storage>::infinity();

    Sample neutral_value = 0.0;
    SampleCombineValuesFn combine_values;
};

struct RegionSampleInputConfig {};
struct CoverageSampleInputConfig {};

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

Tick history/latency remains meaningful only for Tick production. Tock sample outputs
select whether their random-access result is one Region or a Coverage:

```cpp
struct TickOutputConfig {
    std::size_t history = 0;
    std::size_t latency = 0;
};

struct RegionTockOutputConfig {};
struct CoverageTockOutputConfig {};

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

`OutputRetention::{ephemeral,persisted}` remains an independent axis. Both Region and
Coverage Tock outputs may be ephemeral or persisted.

The exact final naming of the empty Region/Coverage alternative structs can change,
but the type separation is the intended contract.

## 7. Sequential combine helpers select the operation statically

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

## 8. Persisted and ephemeral random-access sample data stop using pages

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

## 9. Region count is evaluation data, not realtime layout data

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

## 10. Migration boundary from the checked-in implementation

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
