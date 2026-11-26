# Tick Output Coverage And Feedback

_Status: owner direction updated 2026-11-25. Native runtime verification is
pending._

The owner restored sparse Tick output coverage on November 17 and separated
it from replay eligibility on November 25. The
[November 17 contract](./historical/tick_coverage_before_output_rules_2026-11-25/tick_input_union_and_feedback_direction.md)
records the previous enum and why sparse coverage was restored.

## Values and coverage are different

A Tick callback can compute a value at an executed position using a Sequential
input's neutral/manual fallback. That does not, by itself, make its output
covered. Downstream consumers use output coverage when deciding whether to
consume that value or apply their own fallback.

For acyclic Tick nodes, each computed Sequential output is full unless the
node publishes a rule for it:

```cpp
void propagate_forward_coverage(iv::PropagateForwardCoverageContext<Filter>& ctx) const
{
    ctx.output<"filtered">().publish(ctx.input<"signal">().coverage().extend_right(2));
}
```

The Tick specialization records the expression during graph configuration.
`|` takes the union, `&` takes the intersection, and `extend_right(count)`
adds positions immediately after each covered span. Those operations use the
shared Sequential value lattice of the inputs and output. A stateful filter
can use the extension to describe the last two outputs that depend on the
signal after it becomes absent. Live evaluation follows the actual input
sequence, including the values selected during pause. Background evaluation
uses position-addressed coverage and can derive the filter's coverage without
computing its values. Skipping Tick calls through a gap requires a separate
state contract. On a graph transition, a stateful Tick node carries its
tail countdown forward when its `State` moves and both graphs evaluate
the same rule.

`TickReplayMode::enabled` grants intrinsic replay eligibility. Replay must be
deterministic, side-effect-free, and pointwise. Replayable Tick nodes have no
`State`, history, latency, Random Access input, or authored reverse coverage
callback. They can declare forward coverage like any computed Tick output.
GraphJit must still establish that it can obtain their input values for the
requested positions. A non-replayable filter can propagate a tail and still
require realtime execution to produce its values.

An open input supplies its neutral value or manual fallback for computation.
The coverage expression sees that position as absent. A connected
full-coverage producer contributes normally. For a
gain with sparse automation and an open scale input, the downstream input
applies its own fallback in the automation gaps.

## Feedback SCCs

Use the semantic dependency graph, including authored `.detach()` edges, to
identify feedback strongly connected components (SCCs). A feedback component
contains multiple mutually dependent nodes or an actual self-loop; singleton
acyclic components are not feedback.

Every Tick member of a feedback SCC has effective full coverage for live
execution. Its authored rules remain part of the configured node. GraphJit
can therefore schedule the feedback loop without solving a recursive
coverage equation or requiring a separate full-coverage producer.

Feedback SCCs are not contextually replayable through ordinary position-addressed
background Tick replay, regardless of members' intrinsic replay declarations.
Check SCC membership explicitly. Published recordings can terminate a dependency
traversal without replaying their original feedback producer. An acyclic
processor may consume those selected recorded contents normally.

A future pure isolated background Sequential renderer is a separate execution
facility: it can copy and initialize an eligible Sequential subgraph for each
semantic Region and execute stateful feedback from that initial state. That
changes neither ordinary pointwise contextual replay eligibility nor the
feed-forward dependency DAG of background work. Overlapping Regions may have
different values at the same position because their instance histories begin
at different Region boundaries. The concrete Tock/input API remains undecided;
see [Project Playback](./project_playback_and_presentation_grids_direction.md#future-isolated-background-sequential-rendering-distinct-from-playback).

## Unchanged output and storage contracts

Tick nodes declare Sequential outputs. Tock nodes declare Region or Coverage
outputs. The Tock specialization of `PropagateForwardCoverageContext` keeps its
existing content requests and Region publication. Both execution domains retain
`OutputRealization::{computed, buffered}`. Buffered Tick output coverage in the
background comes from the committed recording. Extending the expression API to
those outputs requires a recording rule for gaps in new writes over an
existing take.

Replayable nodes' own outputs remain computed. They may consume buffered data
produced by other nodes. Recording storage, logical coverage, published-valid
contents and selected demand windows remain distinct. Missing materialization
of known-covered data remains an underrun, never fallback.
