# Historical Sample-Only Port Configuration Sketch

_Status: non-normative historical material. Preserved 2026-10-14 from sections 7
and 8 of `docs/random_access_port_data_direction.md`. It was superseded by the
accepted typed continuous-port declaration API in
[Random-Access Port Data, Audio Value Types, And Input Contract Direction](../random_access_port_data_direction.md#7-authored-continuous-port-configs-are-typed-and-toolable)._

This text is retained because it records the intermediate Sample-only variant design
and the earlier template-selected combiner sketch. It is not a compatibility
requirement.

## Sample port config types should make invalid states unrepresentable

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

Tick history/latency remains meaningful only for Tick production, but it is not part of
the static `constexpr` output-production config. Tock sample outputs select whether
their random-access result is one Region or a Coverage:

```cpp
struct TickOutputConfig {};

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

A configured Tick node contributes its required output `history()` and `latency()` in
`constrain_ports()`. This allows constructor values and graph-resolved port sizes/paces
to determine the temporal window without changing the static callback shape.

`OutputRetention::{ephemeral,persisted}` remains an independent axis. Both Region and
Coverage Tock outputs may be ephemeral or persisted.

The exact final naming of the Region/Coverage alternatives and ordering enum can
change, but the type separation is the intended contract. `CoverageOrdering` belongs
to the configured realization/constraint layer rather than these static config
structures: a Coverage input may constrain the realized ordering it requires, a
Coverage producer may constrain the ordering it guarantees, and an order-preserving
node may relate the two in `constrain_ports()`.

## Sequential combine helpers select the operation statically

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
