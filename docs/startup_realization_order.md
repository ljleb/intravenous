# Startup And Realization Order

This note records the intended server startup order when project state, module
realization, and live execution are decoupled correctly.

## Core distinction

There are three different states:

- desired project state
- realized runtime state
- active execution

Project load reconstructs desired state. It does not need to synchronously
realize iv-module definitions or begin audio execution.

## Intended order

1. Construct all long-lived app modules.
2. Bind all bridges between app modules.
3. Start only inert local infrastructure that cannot emit unsolicited
   project-domain activity.
4. Start any local services required so replayed project commands can target
   their owning modules.
5. Load `iv_project.jsonl` synchronously through the shared mutation
   surface.
6. Emit `server.ready`.
7. Start the iv-module reload/watch thread.
8. Let the reload thread compile dirty or changed definitions in the
   background.
9. Apply completed reload results only at a safe activation point, ideally
   between task-runner passes, or immediately when execution is idle.
10. Let realized definitions update instances, introspection, derived lanes,
    and execution state through the normal event graph.

## Valid initialized state

After project load but before realization:

- required iv-module definitions may be declared but not yet built
- iv-module instances may exist but remain unrealized
- graph-input and graph-output authored state may already be stored
- timeline connectivity may be authored but still pending lane materialization

This is a valid initialized server state.

## Reload direction

Declaration changes should:

- update known declarations
- mark definitions dirty
- refresh watched dependency state

They should not synchronously compile.

The reload/watch thread should be the single initiator of compilation for:

- startup realization
- runtime instance creation
- later hot reload after source edits

To minimize underruns and stutter, compilation should build as much replacement
execution state as possible before activation, then swap the completed results
into the live runtime only at a safe activation boundary.
