# Session snapshots with command pages

Superseded implementation record. Preserved on November 23, 2026 from
`docs-dev/project_session_server_direction.md` after live tests found that one
durable command or the runtime instance records could exceed a reply's limit.
See [the current session server](../project_session_server_direction.md).
See also [the earlier aggregate response](session_snapshot_aggregation_2026-11-23.md).

`session.snapshot` pages the commands from `ProjectPersistence::snapshot()`.
The commands include node instances, node replacements, graph connections,
project toolchain values, and audio-device selection. Set `commandOffset` to
the previous page's `nextCommandOffset` until that value is null. Each page
allows up to 512 KiB of encoded commands, including escaped presentation state.
The server returns an RPC error if a single encoded command exceeds that limit.

`WorkspaceRpc.snapshot()` assembles the command pages and reads runtime records
through their normal RPCs. These records include realized node instances,
packages, audio-device state, and the node-message protocol. The client requires
the same `sessionRevision` on each reply before it returns the snapshot.
