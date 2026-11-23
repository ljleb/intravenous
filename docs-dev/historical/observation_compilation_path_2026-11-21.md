# Former observation compilation rule

This passage came from [Vue output observation direction](../vue_output_observation_direction.md).
It was preserved on 2026-11-21 after the co-author approved direct probe
attachment and code reuse in the active execution generation.

> An interest change that alters executable requirements goes through the same
> coordinated `ProjectGraph`/`GraphJit`/`GraphExecutor` pipeline. Aggregate-interest
> equality and coalescing avoid redundant builds. If requirements change during
> an already running compilation, another compile may still be necessary; the
> single-compile guarantee applies to a structural edit with unchanged accepted
> requirements, not to arbitrary concurrent changes.

## First implementation's Tock State restriction

This passage came from the first
[observation cutover status note](../observation_cutover_continuity_status.md#implementation-status).
It was preserved on 2026-11-21 when the implementation was corrected to
allow private playback previews with a fresh Tock cache.

> Storage cutovers that require a background playback preview have a
> remaining constraint. The preview runs before the predecessor releases
> its mutable Tock State. If it would reset that state, the executor rejects
> the cutover and keeps running the predecessor. A future change could let
> the preview read a snapshot or share ownership of the required Tock State.
