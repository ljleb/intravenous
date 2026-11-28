# Presentation ABI 10 before retained timeline curves

Historical baseline preserved on 2026-11-23 from [Presentation C ABI](../api/reference/presentation_c_abi.md) at revision `4f98cbb1`. ABI 11 assigns the former padding in the gesture and context records to a captured viewport and retained curve data. The current reference replaces these ABI 10 requirements.

This reference describes the core wasm32 ABI version 10.

| Field | Contract |
| --- | --- |
| Descriptor prefix | Three bytes in order: descriptor version `1`, ABI version `10`, flags |

_Static_assert(IV_PRESENTATION_ABI_VERSION == 10, "ABI version");
