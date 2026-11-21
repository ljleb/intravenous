# Scope Interface Availability Before Failure-Only Construction

_Status: historical investigation and alternatives, preserved on 2026-11-13._

Source: `docs/vue_client_implementation_stages.md`. The owner selected
failure-only construction to avoid rollback work on successful builds.
The replacement status is in [Vue Client Implementation Stages](../vue_client_implementation_stages.md#interface-availability-after-a-failed-build).

#### Known scope-port defect

`ProjectGraph::handle_socket_rpc_get_graph_scope` fills ports only from
`current_generation_->placements`. If one invalid instance prevents the first
successful whole-project generation, the scope has no ports even for a valid
definition such as `iv.project.saw`. The editor must distinguish availability
of a configured node interface from whole-project execution success. Do not
pretend that changing the timeline presentation fixes this backend defect, or
relax whole-project execution atomicity merely to expose UI metadata.

##### Configuration constraint found on 2026-11-13

The owner approved fixing interface availability before connection editing,
then completing instance/configuration editing and the first undo/redo slice.
Investigation found a constraint before implementing the first change.

`NodeInstances::configure_and_embed()` first validates every request. Any
missing definition or invalid argument returns diagnostics before constructing
any instance. Construction then uses one shared `GraphBuilder` session.
A constructor failure invalidates that session and clears all placements.
`ProjectGraph::rebuild_locked()` discards the candidate graph on either failure.
`NodeInstancesProjectGraphRead` currently returns instance declarations, not
independently configured interfaces.

For example, one unavailable module definition prevents a valid `saw` instance
from being constructed. Its ports therefore cannot be recovered merely by
changing `ProjectGraph::handle_socket_rpc_get_graph_scope()`. Static definition
metadata is also insufficient for module nodes whose configuration constructs
their public interface. Using ports from the previous executable generation
can describe a different configuration from the desired instance.

The ordinary configuration path deliberately shares a builder session because
construction arguments can reference other project nodes through `NodeRef`.
Configuring every instance separately and importing its result would change
that contract. Clearing the builder failure and continuing would require
defined rollback for every builder record, not just a diagnostic reset.

Two alternatives need a design decision:

- Prepare interfaces separately after a failed whole-project configuration.
  Reuse valid configured results where possible. Additional construction uses
  separate sessions for independent groups of constructor references. A group
  with an invalid dependency has no current interface. This leaves successful
  execution builds unchanged, but adds work and constructor invocations during
  failure handling. Metadata must be associated with the exact desired
  configuration and definition snapshot, and must not be published from a
  provisional node-message preview.
- Add transaction rollback within `GraphBuilder`, then continue independent
  construction after a failure. This changes builder failure ownership and
  reaches configuration objects, node bundles, virtual-node records, package
  references, and message-handler lifetimes. Its reach needs investigation
  before implementation.

The recommendation is the failure-only interface preparation. It preserves
execution atomicity and the current successful-build path. This is a proposed
design change, not an implemented fallback or an accepted requirement.
Connection editing remains next after this decision and its implementation.

