# Native node API and LLVM package ABI

This reference covers authored C++ nodes and the package boundary used by
the server. Include [`intravenous/dsl.h`](../../../src/intravenous/dsl.h)
for authoring. The [native node tutorial](../tutorials/llvm_node.md)
contains a complete minimal package.

## Package and public definitions

`iv_package.json` uses schema 2. A native package declares a `namespace`
and a relative `entry` inside the package. `dependencies` lists imported
namespaces. `builtin` is available implicitly. Public definition IDs belong
to the package namespace. Display names and tags are editor metadata.

| Registration | Authored declaration |
| --- | --- |
| `IV_NODE(id, Type, displayName, ...tags)` | Leaf class with static port declarations and one primary callback family |
| `IV_MODULE(id, function, displayName, ...tags)` | Graph-building function that takes a `GraphBuilder&` and any registered construction arguments |
| `IV_MODULE_CLASS(id, Type, displayName, ...tags)` | Class with a public `void build_graph(GraphBuilder&) const` and registered constructor |

Source introspection generates constructor adapters and reads the
declarations. Instantiation by public ID resolves the provider and its
argument signature. Use `GraphBuilder::node<Type>(...)` inside a package
for a private leaf type. Use `GraphBuilder::node<"definition.id">(...)`
for a registered definition. Use `graph.outputs(...)` to expose outputs
from a module. See [registration checks](../../../src/intravenous/module/package_definitions.h)
and [embedding and selectors](../../graph_builder_embedding_and_matchers.md).

## Ports and callbacks

Declare static `inputs()` and `outputs()` when a leaf has those ports.
Use `iv::input<Value>` and `iv::output<Value>` for continuous data.
Use `iv::event_input` and `iv::event_output` with an `EventTypeId`
for event data. A port has a type and access or production contract. A
continuous port also has channel layout and resolved timing.

| Domain | Callback and output contract |
| --- | --- |
| Tick sample | `tick(TickSampleContext<Node> const&)` processes one sample or event position and writes Sequential outputs. |
| Tick block | `tick_block(TickBlockContext<Node> const&)` processes a scheduled slice. Use the context's block size or the accessed view. |
| Tock | `tock_coverage(TockCoverageContext<Node>&)` writes Region or Coverage results. Visit the output's prepared runs. |
| Batch callbacks | A node can declare an applicable batch form in place of its scalar or block callback. The exact signatures and validation are in `node/traits.h` and the batch callback guide. |

Use `ctx.input<"name">()` and `ctx.output<"name">()` to bind a port by
its authored name. The accessors expose operations allowed by the port
type and callback. A Sequential stream does not have the same read
operations as a Region or Coverage input. Tick Region and Coverage inputs
require complete-buffer access under the current registration rules.

An authored leaf port has an identifier and can have a short display name
and a long display name. Callbacks and connections use the identifier.
Use lowercase ASCII letters, digits, and single underscores. Start with a
letter and end with a letter or digit.

```cpp
iv::input<iv::Sample>("phase_offset");
iv::input<iv::Sample>("phase_offset", "Phase");
iv::input<iv::Sample>("phase_offset", "Phase", "Phase offset");
```

Give a short name before adding a long one. The editor prefers the long name
when it fits, uses the short name in tighter spaces, and shows the identifier
when the author supplies neither. Display names allow UTF-8 text and
punctuation. Use one line, with single spaces and no space at either end.
The editor reserves ` | `, ` · `, and ` → ` with a space on each side.
Non-breaking spaces count as spaces here. **A|B**, **N·m**, and **Hz→MIDI**
are valid labels.

Module input identifiers come from the receiving variable. Use
`auto const dry = g.input();`, `auto const dry = g.input("Dry");`, or
`auto const dry = g.input("Dry", "Dry signal");`.
Set defaults and bounds in the options argument after the display names.
Display names can still be omitted when you supply options.

```cpp
auto const wet = g.input("Wet", "Wet signal", {.default_value = 0, .min = 0, .max = 1});
auto const azimuth = g.input("Azimuth", {
    .min = -180, .max = 180, .geometry = iv::KnobGeometry::circle,
});
```

Module outputs use the ID on the binding. Add labels with
`g.outputs("wet"_P.with_display_name("Wet", "Wet signal") = wet_signal);`.
Generated positional leaf ports can use `iv::PortNames::generated()`.
A single unnamed module export via `g.outputs(expression)` uses the
identifier `value` and display name **Value**.
[Earlier required-label API](../../historical/required_port_display_name_2026-11-26.md)

Tock outputs require forward coverage propagation. Use
`propagate_forward_coverage` to publish Regions and mark changes. The
callback can use `CoverageTask` and await input coverage or data. Use
`propagate_reverse_coverage` for Random Access input demand. Read and write
the positions supplied to the callback. A physical view can contain only
the requested subset of a larger logical Region.
[Tick context](../../../src/intravenous/node/tick.h) ·
[Tock context](../../../src/intravenous/node/coverage_port_context.h) ·
[Callback traits](../../../src/intravenous/node/traits.h) ·
[Batch callbacks](../../batched_node_callbacks_direction.md)

## Node state and configuration

Constructor arguments configure an instance. Use `constrain_ports`
to relate port properties when the graph resolves them. `State` is
execution state owned by the runtime. Native state creation and transfer
follow the lifecycle helpers. A move-constructible Tick State can use
automatic movement. An explicit lifecycle callback can supply the
transfer when the node requires custom behavior.

Tick replay is declared through `TickReplayMode`. The registration and
whole-graph checks govern when a Tick node can supply replayed windows.
The intrinsically replayable case requires computed Sequential outputs
and forbids State, Random Access inputs, and authored coverage callbacks.
[Constraints](../../../src/intravenous/node/port_constraints.h) ·
[Lifecycle](../../../src/intravenous/node/lifecycle.h) ·
[Registration validation](../../../src/intravenous/module/package_definitions.h)

## Native artifact boundary

The package finalizer emits `iv_package_abi_version()` with
`IV_PACKAGE_ABI_VERSION` from
[`module/abi.h`](../../../src/intravenous/module/abi.h). The loader rejects
a mismatch. This package format uses the server's target, compiler and SDK.
The build identity covers the target and toolchain, source and entry,
header and precompiled-header inputs, and package build tools.

Package definition records expose configuration and signature callbacks.
Compiler records expose runtime callbacks and state metadata. Code and
retained data belong to a loaded package revision. Graphs keep the
revision alive while they use its definitions. For eligible execution
regions, GraphJit can build a specialized native function from the package
bitcode, including the node's Tick callback. These records form a
compiler and runtime interface. Node authors normally use the DSL and its
registration macros to produce them.
[Loader](../../../src/intravenous/module/loader.cpp) ·
[Package finalizer](../../../package_tools/iv_package_finalize.cpp) ·
[GraphJit specialization](../../../src/intravenous/graph_jit/region_specialization.h)
