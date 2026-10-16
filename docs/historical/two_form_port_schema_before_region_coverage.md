# Two-Form Port Schema Before Region/Coverage

_Historical snapshot recorded 2026-10-15 while completing the typed continuous-
value configuration migration._

Several design documents still described the earlier two-form schema after
`random_access_port_data_direction.md` had made Region and Coverage distinct authored
forms. The superseded statements were:

- input access was presented as only `SequentialInputConfig` or
  `RandomAccessInputConfig`;
- output production was presented as only `TickOutputConfig` or
  `TockOutputConfig`;
- Random Access/Tock ports as a group were described as owning configurable
  coverage ordering; and
- some implementation-status text said history and latency were still stored in
  static port configs.

Those statements appeared in `graph_jit_direction.md`,
`coverage_and_background_evaluation.md`, `sequential_port_storage_planning.md`,
`project_graph_application_architecture.md`, `dsp_execution_storage_glossary.md`,
and `port_realization_migration_status.md`.

The replacement contract is documented normatively in
[Random-Access Port Data, Audio Value Types, And Input Contract Direction](../random_access_port_data_direction.md):

```text
input access      Sequential | Region | Coverage
output production Tick       | Region | Coverage
output retention  Ephemeral  | Persisted
```

Region values are intrinsically finite and ordered. Only Coverage ports participate
in configurable `CoverageOrdering`. Sequential input history and Tick output
history/latency are contributed through per-instance port constraints.

