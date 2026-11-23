# Presentation ABI 6 scene input

This records the previous scene layout in
`docs/api/reference/presentation_c_abi.md` on 2026-11-22. ABI 7 added
typed waveform views. See the [current C ABI](../api/reference/presentation_c_abi.md).

ABI 6 used descriptor ABI byte 6 and a 192-byte context. The last field
was the edit count at byte 184. The context provided trigger-column
observations. Published WASM presentations were limited to those
observations. The host allocated a 128 KiB scene input buffer.
