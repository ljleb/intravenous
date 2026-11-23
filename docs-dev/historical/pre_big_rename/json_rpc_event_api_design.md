# JSON-RPC Event API Design Notes

This note captures the intended direction for the JSON-RPC server API surface.
It is a design guardrail, not a full refactor plan.

## Goal

The JSON-RPC endpoint should be a transport adapter.

It should own protocol mechanics:

- Request ids.
- JSON-RPC envelopes.
- Method dispatch.
- Error response formatting.
- Response and notification writes.
- Client connection lifecycle.

It should not own domain control flow for graph, timeline, lane, or runtime
features. Domain modules should register C++ API callbacks that the endpoint
invokes when matching client requests arrive.

## Static Wiring

Static control-flow points are used as main-time wiring tools. They have no
unregistration path and are intended for process-lifetime API composition.

Each public API member shaped as an event should support multiple callbacks.
Routes that truly need single-result semantics should still use a multi-callback
event, but enforce the response contract through a builder or single-result
collector.

## Request Shapes

Commands that only need acknowledgement should use fanout void events:

```cpp
Event<void(Request const&)>
```

If no callback throws, the JSON-RPC response is:

```json
{ "ok": true }
```

If a callback throws, the endpoint returns a JSON-RPC error so the client can
surface the failure.

Requests that return data should generally use a shared builder:

```cpp
Event<void(Request const&, ResponseBuilder&)>
```

The endpoint creates the builder, invokes all callbacks, then returns:

```cpp
builder.build()
```

The builder owns the route-specific merge and validation rules. Depending on the
route, it may append contributions, deduplicate by id, enforce a single base
result, allow an empty result, or throw when the response is invalid.

## Builders

Builders keep response semantics close to the route without forcing each
callback to allocate or return a full response object.

Examples:

```cpp
struct QueryBySpansResponseBuilder {
    void add_node(LogicalNodeInfo node);
    ProjectQueryResult build();
};

struct LogicalNodeResponseBuilder {
    void set_base(LogicalNodeInfo node);
    void add_annotation(/* ... */);
    LogicalNodeInfo build();
};

struct LaneViewResponseBuilder {
    void add_lane(LaneInfo lane);
    void add_connection(LaneConnectionInfo connection);
    LaneViewResult build();
};
```

Single-result routes can use a builder or collector that errors on duplicate
results:

```cpp
Event<void(GetLogicalNodeRequest const&, SingleResult<LogicalNodeInfo>&)>
```

This keeps inversion intact while still making duplicate ownership visible.

## Notifications

Outgoing client notifications are ordinary methods on the JSON-RPC adapter:

```cpp
void send_server_status(ServerStatusNotification const&);
void send_timeline_lane_view_updated(LaneViewResult const&);
```

Domain modules should not know socket details. Main-time wiring connects domain
notification events to these adapter methods.

Notification deferral should remain intentional: updates produced while handling
a request should be sent after the request response when that ordering matters.

## Route Ownership

Route registration and route execution should stay separate.

Static callbacks register route definitions once. Per-request and per-client
state should be passed through request objects, builders, or small request
contexts rather than captured in static registration callbacks.

Duplicate JSON-RPC method registration should fail loudly. Multiple callbacks
are allowed inside a route event; duplicate route names are not.

## Initial Direction

The first useful slice is:

1. Introduce a tiny route registry owned by the JSON-RPC adapter.
2. Register route definitions through a static control-flow point.
3. Move one aggregate query route to `Event<void(Request const&, Builder&)>`.
4. Move one command route to `Event<void(Request const&)>`.
5. Preserve current JSON-RPC response/error behavior.
6. Preserve deferred notification ordering.
