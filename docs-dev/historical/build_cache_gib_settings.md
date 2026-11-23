# Build cache settings before byte values

The initial cache policy used `thresholdGiB`, `targetGiB`, and
`minimumFreeGiB` in `build-cache.json`. These fields accepted whole GiB and
defaulted to 40, 30, and 15 respectively. The policy also used `maxIdleDays`
with a default of 30 days. See the [current byte-based configuration](../development_run.md#configure-the-build-cache).

## Initial suffix mapping for byte settings

On November 20, 2026, the first byte-based commands in `docs-dev/development_run.md`
used lowercase suffixes for decimal sizes and uppercase suffixes for binary
sizes. The initial table used `-h` for decimal and `-H` for binary. Both flags
also applied to the tables printed after `set` and `reset`.

The next CLI revision used `-h` for binary and `-H` for decimal and made the
flags inspection-only. It also used lowercase single letters for binary sizes
and uppercase single letters for decimal sizes.

The [current CLI](../development_run.md#configure-the-build-cache) keeps the
display flags and uses GNU Coreutils suffix semantics. A bare size letter
means binary in either case. An explicit suffix such as `GB` means decimal.
