# Vue Stage 2 Before The Project Graph Canvas

Preserved from `docs/vue_client_implementation_stages.md` on 2026-11-12.
This is an implementation checkpoint, not design authority. Its replacement is
[the current Stage 2 status](../vue_client_implementation_stages.md#stage-2-implementation-status).

## Previous implementation status

### Stage 2 implementation status (2026-11-12, written, not yet built or tested)

- Step 1, project root scope: `graph.getScope` returns `ProjectGraphScope`
  (`project_graph_scope_types.h`), built by
  `ProjectGraph::handle_socket_rpc_get_graph_scope` from the current generation
  and the desired state it reads from `NodeInstances` and `GraphConnections`
  (see [Graph scope read](../event_flows/graph_scope_read.md)). Every root build
  marks its `ProjectGraphPublication` with `graph_changed`, which
  `SocketRpcServer` publishes as `graph.changed`. Module-instance scopes are not
  implemented yet.
- Steps 2-5 are not started.
