# Front 3 page walk before the physical-run index

_Historical status: superseded on 2026-11-01 by the implemented
`RecordingPageRunNode` index documented in
[the active Front 3 status](../implementation_fronts_direction.md#front-3-regioncoverage-random-access-representation)._

The first sparse recording implementation kept only the path-copied per-page
directory. Virtual-view assembly walked every page in each published canonical
Region and discovered consecutive physical runs while it walked. The plan classified
reducing that mapping work as a Front 16 optimization.

That classification violated the runtime performance invariant. Publishing a small
append to one long connected recording Region would still search every retained page.
The active implementation therefore keeps a second path-copied index of consecutive
physical runs as required publication metadata. Backing-page reclamation and measured
compaction remain Front 16 optimizations.
