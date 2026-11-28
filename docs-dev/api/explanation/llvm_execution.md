# Native packages and execution

An Intravenous package can contain C++ that defines nodes. The package
compiler uses Clang and source introspection to register public definitions
and convert construction arguments. The package finalizer produces the
LLVM artifact and the native callback code used by the server. The metadata
lets the editor create nodes by stable definition ID.

## Construction and runtime

A leaf declares ports and authors execution callbacks. A module describes a
graph of those leaves or other modules in a function that gets a
`GraphBuilder`. The builder executes during configuration, when the system
can resolve node types, ports, and connections. The leaf callback processes
the scheduled data later. A leaf used only inside a package can be
constructed by type. Registration with `IV_NODE` adds a public definition.

A Tick callback writes Sequential outputs. Its planner schedules it in a
stream, possibly in small slices for a feedback loop. For eligible regions,
GraphJit specializes across node boundaries and can replace known inputs
with LLVM constants. That matters for arrays of small arithmetic nodes.
See [region specialization](../../../src/intravenous/graph_jit/region_specialization.h).

A Tock callback serves demand over a Region or Coverage. Forward
propagation publishes coverage and changes. Reverse propagation describes
the input data needed to satisfy downstream demand. Execution materializes
the prepared ranges or sparse runs. Keeping these stages distinct lets an
observer sample a small part of a very wide window.

## Runtime state

Tick State can carry signal history. Its lifecycle matters during graph
replacement. A Tock implementation can use State as working storage while
keeping results reproducible from its configuration and requested inputs.
A node instance and a presentation session have separate lifetimes. The
server can run a node regardless of which lane is open.

## Why the native artifact has a different ABI

A native LLVM package runs inside the server with its compiler and SDK.
Its artifact supplies records that represent native C++ types and callback
addresses. The loader checks the package ABI and includes the SDK in the
build signature. The [LLVM package ABI](../../../src/intravenous/module/abi.h)
and [finalizer](../../../package_tools/iv_package_finalize.cpp) are the
source of truth.

A presentation goes to a client, which can run on another machine. Its
freestanding WASM artifact uses a fixed wasm32 layout and a versioned C
ABI. The C++ SDK is a source-level wrapper over that ABI. The native
compilation pipeline preserves the information GraphJit needs to optimize
an audio region, while the presentation boundary gives the client the
freedom to schedule and isolate its own workers.
