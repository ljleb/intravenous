# Vue Stage 2 Before The Primary Timeline

Implementation history preserved from `docs/vue_client_implementation_stages.md`
on 2026-11-12. The owner confirmed one child per horizontal row under a shared
horizontal time axis. This replaces the canvas as the initial primary view.
The canvas remains a candidate for a future alternative graph presentation.
The current status is in [Vue Client Implementation Stages](../vue_client_implementation_stages.md#stage-2-implementation-status).

+### Stage 2 implementation status

Updated 2026-11-12. The project-root canvas and its tests are written;
compilation, tests, and the UI checks below remain for the repository owner.
The previous checkpoint is preserved in
[Vue Stage 2 Before The Project Graph Canvas](./historical/vue_stage_2_before_project_graph_canvas_2026-11-12.md).

Owner follow-up: all tests pass and the inspected UI behavior works. Permanent
interaction instructions and the numeric spatial zoom display were removed.
The primary UI must use the timeline-like presentation clarified in
[Node Presentation And Manual Controls Direction](./node_presentation_and_manual_controls_direction.md#the-graph-editor-is-opened-on-one-node).
Do not extend the free-position canvas as the primary editor. Establish the
timeline's concrete interactions with the owner before further UI implementation.

The owner subsequently confirmed the existing canvas as a candidate for a future
alternative presentation of graph/subgraph nodes. The immediate work remains
the primary timeline presentation. Before implementing its layout, clarify how
the subgraph's children occupy the timeline: the current active documents define
the four display modes and parent-allocated regions, but do not specify the
child-row arrangement or time-axis mapping. A concrete question about one row
per child under a shared time axis is pending; do not treat that proposed
arrangement as accepted design.

- Step 1, project root scope: `graph.getScope` returns `ProjectGraphScope`
  (`project_graph_scope_types.h`), built by
  `ProjectGraph::handle_socket_rpc_get_graph_scope` from the current generation
  and the desired state it reads from `NodeInstances` and `GraphConnections`
  (see [Graph scope read](./event_flows/graph_scope_read.md)). Every root build
  marks its `ProjectGraphPublication` with `graph_changed`, which
  `SocketRpcServer` publishes as `graph.changed`. Module-instance scopes are not
  implemented yet.
- Step 2, project root: `Intravenous: Open Graph Editor` opens a Vue editor tab.
  `GraphEditorProvider` reads the scope through the existing `WorkspaceSession`,
  refreshes on `graph.changed`, and reloads after a server restart. Reads are
  serialized; a change during a read discards that response and requests a new
  snapshot. Each restored editor tab uses the same window-owned session.
- The root uses the generic presentation's graph-scope view. Child nodes use
  the frontend presentation registry, with stable addresses and continuous value
  metadata from the scope API. Pan, cursor-centered zoom, draggable node titles,
  and a fit command operate on frontend geometry. New nodes receive positions
  in their returned order, without moving already placed nodes.
- Positions and viewport are saved to `.intravenous/graph-layout.json`, separate
  from `iv_project.jsonl`. Saves are serialized and replace the layout file
  atomically. Positions survive temporary removal and restoration of a node.
  An unreadable layout is reported and is not overwritten automatically.
- Missing or interior endpoints stay visible in the connection list. Lines are
  drawn only for endpoints that resolve to ports in the root scope. A matcher
  with no port name or index selects all ports, as `GraphConnections` does.
  Unapplied connections and unrealized instances remain visible.
- Step 2 remains incomplete for opening module-instance scopes. Their backend
  read model still needs implementation; this pass does not expose an open
  button that cannot work. The source-inspection side panel remains available.
- Steps 3-5 are not started: graph mutation gestures, retirement of
  `ModulesViewProvider`, and the first undo/redo slice.

#### Verify the project-root canvas

After rebuilding the server and the extension:

1. Open `projects/simple_sine` and run `Intravenous: Open Graph Editor` from
   the command palette. Check that the project instances and their ports appear.
2. Drag a node title, pan the background, and zoom with the wheel. Check that
   zoom stays centered on the cursor and that connection lines follow the nodes.
3. Reload the VS Code window. Check that the editor tab, positions, and viewport
   return. Open a second graph tab and check that both see package refreshes.
4. Use the existing Modules panel to create or delete an instance. Check that
   the canvas refreshes without moving other nodes. A retained connection with
   a missing endpoint must remain in the connection list.
5. Check a project with event connections and a connection matcher that selects
   multiple ports. Event port indices must not be treated as continuous port
   indices. Selectors into module interiors must not attach to guessed root ports.

