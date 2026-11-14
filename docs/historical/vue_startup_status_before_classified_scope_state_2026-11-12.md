# Startup Status Before Classified Scope State

Preserved from `docs/vue_client_implementation_stages.md` on 2026-11-12.
The owner accepted a classified per-node loading/failure state. The replacement
is recorded in [the active implementation status](../vue_client_implementation_stages.md#classified-scope-state).

+#### Startup failure indication: investigation, not fixed

Startup replay can fail configuration before package definitions are available.
`NodeInstances` records the unavailable definition as an ordinary diagnostic, and
`graph.getScope.errorMessage` exposes it. The current lane consequently turns red
even when the pending package build subsequently succeeds.

The backend already exposes package progress through `server.status`
(`rebuildStarted`, `rebuildFinished`, `rebuildFailed`) and `ivPackages.list`
(`buildState`: `queued`, `building`, `built`, `failed`). The extension consumes
these for build logs and the Modules panel. The lane receives the scope snapshot,
which has no classified per-node loading/error state; it does not reconcile
those package states with the diagnostic.

A new route is not required merely to obtain package progress. A correct node
indicator still needs a distinction between a definition awaiting a build and
a confirmed failed desired node, including failed edits while a previous
generation remains active. Prefer a classified per-node state in the shared
read model, derived from existing package and configuration state, over client
timers or matching diagnostic strings. This pass records the issue; it does not
claim that the startup red flash is fixed.

The previous timeline status also said:

> The ruler starts with a provisional 0–10 second viewport.

> No new zoom, clip-editing, node-reordering, or connection gestures were assigned.

The current viewport uses a time-per-pixel scale and the owner's explicit
navigation bindings. Clip editing, node reordering, and connection gestures
remain separate work.
