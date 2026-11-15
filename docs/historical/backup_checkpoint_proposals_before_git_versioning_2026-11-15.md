# Backup/Checkpoint Proposals Before Git-Based Project Versioning (2026-11-15)

_Status: superseded design suggestion, retained for provenance; not an active
implementation direction._

The 2026-11-14/15 design discussion clarified that normalized, human-readable
`iv_project.jsonl` is intended to be Git-compatible. A separate restorable
backup/checkpoint subsystem is not a useful planned product feature. Ordinary
session undo/redo remains distinct. The previous speculative backup text is
preserved verbatim below. Replacements:
- [Project Undo/Redo Direction](../project_undo_redo_and_backups_direction.md)
- [Project File Serialization Direction](../project_file_serialization_direction.md)

## From `docs/project_undo_redo_and_backups_direction.md`

Backup/checkpoint support is a promising **future use of the same project-state
capture and serialization facilities**. A backup is a restorable snapshot at a
point in time; an inverse-only undo entry is not by itself a complete backup.
A future history store could mix full snapshots/checkpoints with reversible
change records, or serialize a chosen undo point together with the base state
needed to reconstruct it. This is a candidate design, not a requirement to
persist session undo history now or to change the canonical project file format.
Backups must preserve the full durable project model and stable identities and
must not confuse runtime/executor state with reconstructable project intent.

## From `docs/project_file_serialization_direction.md`

### Undo records and backup checkpoints are separate from the project file

Undo/redo is planned as server-owned history of **reversible batches of accepted
durable project changes**, constructed from authoritative before/after records.
It may reuse this command vocabulary without serializing each user action into
`iv_project.jsonl`. Saving or replaying a normalized snapshot does not append or
replay undo steps. A future backup/checkpoint system may reuse the same complete
project snapshot and serialization facilities, possibly alongside an action
journal; an inverse-only undo record is not a complete restorable backup.
