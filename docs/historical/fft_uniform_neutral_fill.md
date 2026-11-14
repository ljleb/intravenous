# Superseded Uniform FFT Neutral Fill

_Status: non-normative historical design text._

_Provenance: preserved on 2026-10-15 from
[`random_access_port_data_direction.md`](../random_access_port_data_direction.md)
when the dynamically sized `FFTBlock` neutral representation was changed from one
repeated bin value to a resolved-block initializer operation. The replacement is the
current “Sequential reduction is configurable per input” section of that document._

The earlier direction said:

> For a dynamically sized logical value such as `FFTBlock`, the static declaration
> cannot contain a scalar-style default or a complete resolved-size neutral block. Its
> extension instead represents the reduction contract with a neutral bin/fill operation
> and a block-combine operation over resolved storage. The authored `neutral_value` and
> `combine_values` field names retain their semantic meaning while the extension
> supplies their valid types and lowering behavior.

The first implementation interpreted “neutral bin/fill operation” as one authored
`FFTBin` repeated across every bin of the resolved block. That representation was
superseded because `combine_values` already operates on an entire logical FFT block and
may define a reduction whose identity is size-dependent or non-uniform across bins.

