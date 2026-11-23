# Former session snapshot aggregation

This passage records the `session.snapshot` implementation described in
`ebc98753`. See the [current recovery protocol](../project_session_server_direction.md).

> It returns durable project commands and current runtime records through
> `session.snapshot`.
>
> The snapshot contains the commands from `ProjectPersistence::snapshot()`. These
> commands include node instances, node replacements, graph connections, project
> toolchain values, and audio-device selection. The snapshot also contains the
> current package list, realized node instances, audio-device state, autosave
> state, and node-message protocol.
