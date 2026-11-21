# Earlier access to packed Tock runs

_Historical proposal, 2026-11-19. The owner asked to shorten access
chains in authored node callbacks. See the
[revised Tock API](../sparse_tock_demand_proposal.md#authored-api)._

From `docs/sparse_tock_demand_proposal.md`, Authored API.

3. **Evaluation.** The logical Region's `prepared_runs()` exposes the
   selected entries as dense storage. Both run types have `size()` and
   `index(slot)`, which returns the Region-relative index. A continuous
   run adds `position(slot)` and typed `value(slot, channel)` access.
   An indexed event run provides a read-only span of `TimedEvent`s or
   a writable `write(slot, TimedEvent)` on an output. A constant-payload
   Region also exposes `write_time(slot, GlobalIndex)`, using its declared
   payload. A positional event request still calls the Region writer in
   timestamp order because its occurrence count can be discovered during
   production.

```cpp
// A continuous output evaluates only the selected positions.
for (auto region : ctx.output<"value">().regions()) {
    for (auto run : region.prepared_runs()) {
        for (std::size_t slot = 0; slot < run.size(); ++slot) {
            run.value(slot, 0) = value_at(run.position(slot), ctx.sample_rate);
        }
    }
}

// An indexed event output evaluates only the selected occurrences.
for (auto region : ctx.output<"events">().regions()) {
    for (auto run : region.prepared_runs()) {
        for (std::size_t slot = 0; slot < run.size(); ++slot) {
            run.write(slot, event_at_index(run.index(slot), ctx.sample_rate));
        }
    }
}
```

## Direct access on the context

The next draft put the run methods on the Tock context. The owner kept
`ctx.input<>()` and `ctx.output<>()` as the entry points for port access.

3. **Evaluation.** Use `ctx.input_runs<"in">()` to read prepared runs and
   `ctx.output_runs<"out">()` to fill them. Both use the named port's
   type. The iterator visits Regions in their existing order and visits
   runs in index order within each Region. Each run carries its
   `RegionRef` through `region()`, so overlapping Regions keep distinct
   identities. Both kinds of run have `size()` and `index(slot)` for the
   Region-relative index. A continuous run adds `position(slot)` and typed
   `value(slot, channel)`. An indexed event run provides a read-only span
   of `TimedEvent`s or a writable `write(slot, TimedEvent)`. A constant-payload
   Region also supports `write_time(slot, GlobalIndex)`. A positional event
   request uses the Region writer in timestamp order. Its occurrence count
   can be discovered during production.

```cpp
// A continuous output evaluates each requested sample position.
for (auto run : ctx.output_runs<"value">()) {
    for (std::size_t slot = 0; slot < run.size(); ++slot) {
        run.value(slot, 0) = value_at(run.position(slot), ctx.sample_rate);
    }
}

// An indexed event output evaluates each requested occurrence.
for (auto run : ctx.output_runs<"events">()) {
    for (std::size_t slot = 0; slot < run.size(); ++slot) {
        run.write(slot, event_at_index(run.index(slot), ctx.sample_rate));
    }
}
```

## Earlier contiguous output fallback

The shared demand proposal originally allowed the planner to prepare a
contiguous window for a sparse output request. The owner requires all
continuous Tock kernels to honor the prepared runs. See the
[migration requirement](../sparse_tock_demand_proposal.md#tock-callback-migration).

Migrate continuous Tock kernels to `prepared_runs()` on their input
and output ports. Use `required_selections()` in reverse propagation as
part of the same change. Keep whole-Region and explicitly contiguous
accessors for consumers whose algorithms require them. An algorithm that
needs contiguous input can
request it through reverse propagation. If it also needs contiguous output
evaluation, the planner can prepare a valid positional window and gather
the selected frames from it. Apply the observation work budget to the
whole window in that case.

## Earlier positional demand interface

The owner approved a compact continuous positional demand on 2026-11-19.
The following wording from `docs/sparse_tock_demand_proposal.md` predates
that decision. See the [accepted demand API](../sparse_tock_demand_proposal.md#authored-api).

> Keep `IndexCoverage` as the set of positions for existence, change, and
> positional demand. A positional event demand selects all occurrences at the
> requested positions. A continuous positional demand selects the frames in the
> specified positions for each affected Region. Region-relative selections
> retain both the individual Region and the exact entries requested within it.
> Keep stride runs compact throughout planning and storage.

> Both output facades expose `ctx.output<"out">().required_selections()`
> alongside positional `required()`. Reverse propagation must account for both.

> `IndexCoverage` continues to describe existence and changes. Put sparse
> sample selections in `RegionSelection`. The planner can intersect a run
> with a positional range using arithmetic on its endpoints. This avoids
> expanding a regular run into a set of one-frame windows and then packing it
> again. Dense positional demands produce step-one runs and keep their
> existing contiguous storage fast path.
