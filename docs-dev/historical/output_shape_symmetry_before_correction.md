# Output Shape Symmetry Before Correction

_Status: historical, non-normative._

Preserved from the staged output-capability documentation labelled 2026-11-16,
before applying the owner's explicit correction: Tick produces only Sequential
outputs; Tock produces only Region or Coverage outputs. Both share computed and
buffered realization. This file preserves the erroneous broader combinations,
not accepted current intent. The replacements are in
[Output Capabilities, Buffer Access, And Background Demand](../output_capabilities_and_background_demand.md),
[Coverage And Background Evaluation](../coverage_and_background_evaluation.md), and
[the glossary](../dsp_execution_storage_glossary.md).

## Source: `docs-dev/output_capabilities_and_background_demand.md`

**Tick and Tock name callback/execution domains, not output shapes.** A Tick
primitive executes through `tick()`/`tick_block()` on the realtime graph; a
Tock primitive executes through `tock_coverage()` in the background. Each can
author Sequential, Region, or Coverage outputs, with either realization mode,
subject to the access and scheduling requirements below. One concrete primitive
still has a single primary callback domain; a module can compose both.

Output shape, realization, and callback domain must not silently be inferred
from each other. They are independent descriptions, but they are not three
promises of independent memory allocation.

## Source: `docs-dev/output_capabilities_and_background_demand.md`

| Tick | Region or Coverage, computed or buffered | Declared output shape governs access; live writes never grant implicit historical Random Access |
| Tock | Sequential/computed | Demand-generated bounded sequential playback data |


## Source: `docs-dev/output_capabilities_and_background_demand.md`

Tick Region/Coverage output declarations do not authorize coverage algebra or
background requests on the audio thread. Any logical Region/Coverage metadata
needed for planning is established by the background/executor boundary; realtime
callbacks only write their scheduled, preallocated output data. A declaration
cannot expose arbitrary historical random access to values that were never
recorded or reproducibly replayed.

A Tock Sequential output (including a sparse automation projection) is prepared
in bounded future windows before the realtime consumer executes.

## Source: `docs-dev/coverage_and_background_evaluation.md`

> `OutputRealization::computed` or `OutputRealization::buffered`. Both Tick and
> Tock primitives can declare each output shape. Callback domain is independent
> from output shape and realization; a concrete primitive still uses one
> primary callback domain.

## Source: `docs-dev/coverage_and_background_evaluation.md`

Both callback domains may declare all three output shapes. A concrete
primitive still has exactly one primary Tick or Tock callback domain.
Input access, output shape and output realization are distinct static
properties, and sample/event data properties remain separate.

## Source: `docs-dev/coverage_and_background_evaluation.md`

for all six callback/realization examples and

## Source: `docs-dev/coverage_and_background_evaluation.md`

Tock-produced Sequential data feed realtime through prepared bounded
windows.

## Source: `docs-dev/dsp_execution_storage_glossary.md`

A primitive's Tick or Tock callback domain is a separate execution fact. Neither
callback kind restricts the three output shapes. Neither realization mode
specifies whether or how much the background executor caches.

## Source: `docs-dev/dsp_execution_storage_glossary.md`

stream of ordered values. A Tick/Sequential producer runs on the audio thread;
a Tock/Sequential producer prepares requested portions in the background.
A live Tick/Sequential stream produces values at every executed position;

## Source: `docs-dev/dsp_execution_storage_glossary.md`, Tick and Tock callbacks

position-addressed evaluation. Both callback domains can declare Sequential,
Region or Coverage outputs. One concrete primitive has one primary callback
domain; module nodes compose domains.

## Source: `docs-dev/automation_and_background_migration_plan.md`

The superseding migration list originally requested:

> Sequential/Region/Coverage output shapes for both Tick and Tock primitives,

This was corrected with the same disjoint output-shape rule as the normative
output-capability document. The initial Tick Random Access implementation also
requires complete-buffer input access, as explicitly chosen by the owner.

## Remaining contradictory passages corrected during source completion

The following passages were retained from the working documentation before
applying the owner's disjoint output-shape decision consistently. Most came
from `coverage_and_background_evaluation.md`; the final source-status paragraph
came from `output_capabilities_and_background_demand.md`. Their replacements
are in those documents and the migration plan linked above.

```text
contracts, mirrored Tick/Tock output
> shapes,
```

```text
`RegionInputConfig`, `CoverageInputConfig`, and mirrored
`SequentialOutputConfig`, `RegionOutputConfig`, `CoverageOutputConfig` for
both Tick and Tock primitives. The fourth property is
```

```text
retention flag). Data/value types and port pacing remain independent. The
source still uses `TickOutputConfig` and `OutputRetention`, pending migration.
```

```text
The *intended* authored port schema is mirrored across Tick and Tock:
```

```text
`TickOutputConfig`, `OutputRetention::ephemeral` and
`OutputRetention::persisted` are checked-in implementation identifiers only.
The old retention contract and its complete-publication obligations are
```

```text
The checked-in `TickReplayMode::{disabled,input_union,full}` declaration,
its coverage propagation rules, and restriction to ephemeral Tick outputs are
**legacy source behavior** pending migration; neither `input_union` nor
`full` defines the new output realization mode.
```

```text
static constexpr concrete schemas; mirrored Sequential/Region/Coverage output
shapes for Tick and Tock; computed/buffered output realization; a separate
```

```text
and invalid replayability trait declarations. Output shape is not constrained
to one family by callback domain; a primitive still selects exactly one
primary Tick or Tock callback family. The planned batch
```

```text
`tock_coverage()`/`tock_coverage_batch()`; both callback families may declare
Sequential, Region or Coverage outputs; skip plus each authored
```

```text
1. Input access is Sequential/Region/Coverage; output production is
   Sequential/Region/Coverage for **both** Tick and Tock primitives; output
   realization is `computed`/`buffered`. Callback domain is separate.
```

```text
7. Tick and Tock output declaration families are mirrored. Tick realtime
   Sequential production is continuous and its `.detach()` feedback history
```

```text
The checked-in implementation still has `TickOutputConfig`,
`OutputRetention::{ephemeral,persisted}`, `TickReplayMode` coupling replay to
coverage, strict persisted retention, whole-Region selection/materialization
and complete-coverage Tock publication assumptions. Those are source-state facts
and **must not** be mistaken for the destination above.
```
