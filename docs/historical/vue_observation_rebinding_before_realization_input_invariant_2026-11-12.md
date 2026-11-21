# Vue Observation Rebinding Before The Realization-Input Invariant (2026-11-12)

_Status: historical, non-normative. Verbatim passages preserved on 2026-11-12 from
the documentation at revision `85790159` ("clarifications"). They were superseded
the same day by the accepted decision that the accepted executable live-probe
requirements are inputs to every graph realization. The replacement contract is
in [Vue Output Observation And Query Direction](../vue_output_observation_direction.md)
(§5 "Accepted observation requirements are realization inputs" and §6), with the
application-module flows in
[Project Graph Application Architecture](../project_graph_application_architecture.md)
and the compiler consequence in [Graph JIT Direction](../graph_jit_direction.md)._

## Why these passages were superseded

The passages below said that `Observations` rebinds a compatible semantic source
*after* a generation cutover. They did not say whether a structural graph edit
compiles with the current observation requirements. Read literally, a structural
edit could compile without probes, and `Observations` would then request a second
compilation to restore them after cutover. That doubles the edit's compilation
latency and leaves live views blank between the two activations.

The accepted replacement makes the executable live-probe requirements a
retained `ProjectGraph` compilation input. Every structural build re-resolves
compatible probes in the same compilation and generation. After staging, `Observations` prepares generation-safe result routing;
rebinding alone never triggers another compilation.

## Superseded passage from `docs/vue_output_observation_direction.md`, §5

````text
- subscription reconciliation, lease/expiry, disconnection cleanup, and
  generation rebinding/invalidation;
````

## Superseded passage from `docs/vue_output_observation_direction.md`, §6

````text
The external boundary reuses existing semantic `NodeAddress` plus resolved
output/channel identity, with implementation-local capture identities and
versions kept internal. Replies/results carry sufficient generation and
`(semantic_version, data_version)` provenance (where applicable), actual
coverage and request identity to reject stale deliveries. Rebind a compatible
semantic source after a generation cutover; otherwise invalidate it, and
never retain pointers into reclaimed generations.
````

## Superseded table row from `docs/project_graph_application_architecture.md`

````text
| `ProjectGraph` | orchestrate one complete root-graph construction transaction without duplicating instance/connection intent |
````
