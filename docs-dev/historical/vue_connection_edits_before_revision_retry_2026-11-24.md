# Connection edits before revision retry

Preserved on 2026-11-24 from the
[connection-editing implementation checkpoint](../vue_client_implementation_stages.md#connection-editing-implementation-checkpoint).
The replacement uses the revision returned with each scope and rereads after a
revision rejection.

> `WorkspaceSession.editGraphConnectionEnd` reads the current scope before
> submission and captures the session revision before that read. A notification
> between the read and submission cannot make an old edit pass the revision
> check.
