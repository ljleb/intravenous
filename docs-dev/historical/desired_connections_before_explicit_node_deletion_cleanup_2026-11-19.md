# Desired connections before explicit node deletion cleanup

Preserved on 2026-11-19 from `docs-dev/project_undo_redo_and_backups_direction.md`.
The owner requested removing a deleted node's endpoints in the same
undoable edit. See the [replacement contract](../project_undo_redo_and_backups_direction.md#explicit-instance-deletion).

## History example

- Deleting several nodes is one action. Its reverse restores the affected complete
  `ProjectNodeInstanceDeclaration`, `ProjectNodeReplacement`,
  `ProjectConnection`, and `ProjectManualControl` records **that actually changed**,
  preserving stable identities. Do not infer deletion of connections merely from
  losing an endpoint: `GraphConnections` intentionally retains dangling desired
  matchers where applicable.

## Edit batches

Deleting an instance removes its owned replacement records and manual values.
Desired connections retain their matchers, including dangling ones, unless
the same action explicitly changes them. Undo restores the exact records that
changed rather than deleting or recreating inferred connections.
