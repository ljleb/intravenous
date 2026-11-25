# Sampler live output before recorded playback

This superseded text appeared in `714b82c2` under “Sampler recording files”
in `docs-dev/project_playback_and_presentation_grids_direction.md`.

> An unarmed callback leaves the recording untouched and delivers silence to
> live consumers. Waveform observations and Random Access consumers use the
> published recording from that port.

The [current Sampler behavior](../project_playback_and_presentation_grids_direction.md#sampler-recording-files)
lets Sequential consumers hear that same recording when the Tick callback
leaves the port `untouched`.
