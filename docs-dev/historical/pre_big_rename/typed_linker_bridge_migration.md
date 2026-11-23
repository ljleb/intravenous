# Typed linker bridge migration

## Destination

Linker events are process-global transport. A bridge is an exclusive,
process-global connection between exactly two modules. It has no application
behavior of its own.

A completed bridge contains only its participant declaration and direct event
to member-function subscriptions:

```cpp
// example_a_example_b_bridge.h
#include <intravenous/bridge.h>

IV_DECLARE_BRIDGE(example_a_example_b_bridge, ExampleA, ExampleB);

// example_a_example_b_bridge.cpp
IV_DEFINE_BRIDGE(example_a_example_b_bridge);
IV_SUBSCRIBE_LINKER_EVENT(
    example_a_example_b_bridge,
    iv_runtime_example_a_changed_event,
    &ExampleB::handle_example_a_changed);
```

Binding returns a move-only RAII scope. At most one scope may bind a named
bridge at a time. Attempting to bind it again is a runtime error; nesting and
replacement are not supported. Destroying the active scope disconnects the
bridge.

The subscriber thunk dispatches directly to the named member on the matching
bridge participant. It does not adapt arguments, construct responses, invoke
further events, or apply policy.

Modules own connectables. An event and the member it connects to have exactly
matching signatures. A module may expose a member shaped for a connection
without depending on the reason another module makes that connection.

## What does not belong in a bridge

Bridge files must not contain:

- forwarding handler functions or lambdas;
- endpoint globals or manual bind/unbind APIs;
- argument conversion, validation, response-builder completion, or error
  translation;
- follow-on event invocation, notifications, or state-change publication;
- initialization or synchronization work performed at bind time.

When existing bridge logic is non-trivial, decide case by case which module
should own it. Usually it belongs with the module that raises the event, so
that module emits a connection-ready contract. Do not blindly fold optional
downstream concerns into a producer; that would turn the app into connection
spaghetti.

Bridges with more than two participants must be split into two-party links.
An event subscription with no pair of concrete module endpoints is not a
bridge: delete it, fold it into its owning module, or replace the event with a
direct call. Do not invent a nominal participant just to fit the bridge API.

## Migration sequence

1. Finish and test the typed bridge primitive: exclusive binding, move-only
   scopes, direct member dispatch, and safe disconnection.
2. Convert direct-forwarding bridges first. Each conversion removes its old
   globals and bind/unbind functions, then replaces their callers with a
   lifetime scope.
3. Align signatures for bridges that only complete builders or make small
   representation conversions. Move that work into a participating module.
4. Move larger timeline, project, and RPC policies into their appropriate
   modules, preserving behavior with focused tests before making the bridge
   declarative.
5. Convert singleton-event bridge subscriptions using the corresponding typed
   facility, or first turn those singleton events into linker-set events.
6. Audit that every bridge is declarative. The only multicast subscription
   interface is the typed `IV_SUBSCRIBE_LINKER_EVENT` bridge macro.
7. Move modules and declarative bridges out of the flat `runtime/` directory
   in a separate, move-only change. Co-locate module implementation and event
   declarations; group bridges in a dedicated bridge area.

## Completion checks

- No bridge has custom endpoint globals, bind/unbind functions, handlers,
  lambdas, or event invocation.
- Every bridge is a two-party typed connection with an exclusive RAII scope.
- Every event-to-member connection has an exact signature match.
- The app and tests retain binding scopes rather than manually unbinding.
- No legacy bridge subscription macro remains.
