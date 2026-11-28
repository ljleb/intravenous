# Stage 3 before the automation presentation

Non-normative. Preserved on 2026-11-14 from the current-stage checkpoint in
`docs-dev/vue_client_implementation_stages.md`, the last paragraph of
`vscode/client/bindings/README.md`, and the last paragraph of
`src/intravenous/builtin_packages/automation/README.md`.
The replacement status is in [Vue Client Implementation Stages](../vue_client_implementation_stages.md#automation-presentation-source-checkpoint-2026-11-14).

## Stage checkpoint

Current checkpoint (2026-11-14): generic lane wiring and live manual controls are
implemented and owner-tested. The typed frontend `node.send(event)` path is now
implemented in source and awaits owner verification. The specialized automation
presentation remains next, including authoritative point data after reload/undo
and grouped message gestures. Live output meters and other Tick observation
remain in Stage 4. The earlier checkpoint is preserved in
[historical documentation](stage_3_before_typed_frontend_dispatch_2026-11-14.md).


## Bindings guide

This implements typed event dispatch. The specialized curve presentation, its
authoritative construction-data read model, and grouping of point-drag messages
remain the next Stage 3 feature. This binding does not claim those features.

## Automation package guide

The specialized Vue curve editor and typed frontend bindings are pending.
