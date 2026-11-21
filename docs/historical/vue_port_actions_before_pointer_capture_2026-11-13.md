# Vue Port Actions Before Pointer Capture

_Status: historical implementation checkpoint, preserved on 2026-11-13._

Source: `docs/vue_client_implementation_stages.md`. The owner requested
extension commands instead of toolbar/lane action buttons, colored port
circles, and hover expansion from a grouped slot. The owner also reported
that connection creation did not appear to work. The previous implementation
used native HTML drag-and-drop and displayed only a temporary wire.

The replacement status is in
[Vue Client Implementation Stages](../vue_client_implementation_stages.md#connection-editing-implementation-checkpoint).

#### Connection-editing implementation checkpoint

The next Stage 2 feature is in progress. The default lane now has individual
port handles for small port counts and a temporary panel for larger counts.
Dragging expands the destination direction and shows a temporary wire.
The extension resolves endpoints against a fresh authoritative scope and uses
`graphConnections.upsert` / `graphConnections.delete` through `WorkspaceRpc`.
The panel lists existing connections for deletion. Type conversion remains a
backend decision. Pure client regressions cover endpoint identity, direction,
channel/event types, and stale or read-only scopes.

Instance/configuration editing is also written. The root toolbar opens the
definition catalog, creates packages, and saves the project. A lane's edit
button opens its name and construction-argument form, deletion, and source
navigation. The host rereads the scope before mutations. Configuration updates
compare the form's starting values against the current accepted record before
submitting through the revision-checked RPC path.

`ModulesViewProvider` and its old editor command/serializer are removed. Package
creation, instance creation, naming/deletion, and source navigation now use the
Vue editor. The source-selection side panel is retained and still filters its
node projection. `WorkspaceSession` retains the project catalog for selection
and notification processing, without maintaining a second module-panel state.

Compilation, tests, and UI verification remain with the owner. These changes
form the validation boundary before the first server-owned undo/redo slice.
No undo history or batched project-mutation API is implemented yet. Stage 3
has not started.

