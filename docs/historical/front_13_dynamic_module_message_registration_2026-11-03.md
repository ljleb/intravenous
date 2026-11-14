# Front 13 dynamic module-message registration before class modules

This file preserves the module-message design that was present in
`docs/node_interaction_and_presentation_communication_direction.md` before the
2026-11-03 class-module decision. It is historical. The replacement design is the
"Module-node handlers" section in that document.

The earlier design registered a callback while a function module constructed its
graph:

```cpp
auto filter = g.node<"filter">(...);
auto envelope = g.node<"envelope">(...);

g.on_message(
    [filter, envelope](ModuleMessageContext& ctx,
                       SomeEvent const& event) {
        ctx.send(filter, ...);
        ctx.send(envelope, ...);
    });
```

The callback captured `NodeRef` values and construction values. This made the set of
module handlers depend on execution of the module-construction function. Package
metadata could not describe that set before graph construction. A captured `NodeRef`
also retained a builder-local representation that was not suitable for later
message dispatch.

The replacement uses `IV_MODULE_CLASS`, a constructor for construction arguments,
`build_graph(GraphBuilder&) const`, compiler-discovered `on_message` overloads, and
`ModuleMessageContext<Module>::ref<"local-name">()`. Function modules remain the
short form for modules that do not receive semantic node messages.
