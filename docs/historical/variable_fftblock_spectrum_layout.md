# Superseded Variable `FFTBlock` Spectrum Layout

_Status: historical, non-normative._

_Provenance: this text was the `FFTBlock` size/layout direction in
`docs/random_access_port_data_direction.md` before the preliminary canonical real-FFT
layout was selected on 2026-10-14._

_Reason for supersession: allowing both a full N-bin complex spectrum and an N/2+1-bin
real-transform spectrum under the same `FFTBlock` type left the meaning of `size()` and
the library-adapter boundary ambiguous. The current direction fixes one semantic layout:
the complete nonredundant half-spectrum of a real-valued input, with explicit DC and
Nyquist complex bins. Backend-native packed/full layouts remain adapter details._

## Preserved direction

`FFTBlock::size()` counted complex bins rather than underlying real scalars, a transform
library's work-buffer elements, or a configured time-domain transform length. A
transform node owned the constraint between its configured time-domain length and its
output bin count. A node could expose all `N` complex bins or only the `N / 2 + 1`
nonnegative-frequency bins of a real transform; either result was an ordinary
contiguous array of explicit `FFTBin` values.

The current replacement is documented in
[Random-Access Port Data Direction](../random_access_port_data_direction.md#fftblock-has-a-canonical-library-neutral-bin-representation).
