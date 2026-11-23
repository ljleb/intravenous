# Front 3 Event Coverage Before Anonymous Region Lists

_Historical status: superseded on 2026-10-26 by the event Region/Coverage
contract in [Random-Access Port Data Direction](../random_access_port_data_direction.md).
This file preserves the previous design and source-status wording._

The previous Front 3 design generalized logical Region lists only for continuous
ports. An authored continuous forward-propagation callback published one
`GlobalIndexRange` for a Region output or a `span<GlobalIndexRange const>` for a
Coverage output. An authored event callback instead published one normalized
`IndexCoverage` for both kinds of random-access output.

The previous typed callback names exposed that difference directly:

- continuous Region output: `publish_region()`;
- continuous Coverage output: `publish_regions()`; and
- event Region or Coverage output: `publish_coverage()`.

The previous rationale said that an event output had no data-bearing Region
decomposition. `PersistedEventChunk` records could have ranges, but those ranges were
treated as physical retained-storage chunks. Propagation, fan-in, transaction event
storage, persisted publication, and Tick materialization were allowed to normalize
overlaps and discard exact duplicate ranges.

The superseded persisted-data direction also described one logical random-access
event read as a segmented traversal over stored chunks. It promised timestamp order
and clipping to the requested positional range, but it did not expose stored chunk
membership to authored node code. Its equal-time order was defined by range-relative
time, semantic source/connection index, and producer-local order. The replacement
keeps that deterministic ordering for flat Sequential playback while adding the
logical Region list required by Region and Coverage inputs.

That design is superseded. It cannot represent two independently processed event
groups whose ranges overlap or are identical. The accepted replacement gives event
Region and Coverage ports the same logical range-list shape as continuous ports.
Each event Region has independent event storage. `IndexCoverage` remains only the
derived positional union used for demand, invalidation, and scheduling.

The first replacement uses anonymous Regions. List position is observable during one
publication and its corresponding callbacks, but it is not a stable cross-evaluation
identity. Optional explicit Region identifiers, dynamic identifier creation, UI
discovery, and selection by identifier remain future work.
