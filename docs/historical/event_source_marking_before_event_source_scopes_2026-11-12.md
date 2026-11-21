# Event Source Marking Before Event Source Scopes (2026-11-12)

_Status: historical, non-normative. Verbatim sections preserved on 2026-11-12
from `docs/event_propagation_tree_constraint.md` at revision `a3bba875`. They
were superseded the same day by the repository owner's decision that an event
source is entered explicitly with a scope, `IV_EVENT_SOURCE(SourceModule, name)
{ ... }`: linker events may be raised only inside such a scope, everything
raised inside one scope is one propagation tree, and the source module itself
counts as visited. Module transactions (`EventModuleVisit`) were added at the
same time so that one legitimate multistep computation, such as a node-message
interaction, does not have to enter a module once per step. The replacement
text is in [Event Propagation Tree Constraint](../event_propagation_tree_constraint.md)._

## Why these sections were superseded

`IV_INVOKE_LINKER_EVENT_SOURCE` marked one raise as a root, so one source could
start as many trees as it raised events, and an unmarked top-level raise ran
unchecked. Only `PackageWatcherService` marked its raises; client messages,
project replay, autosave and control-compile completions all ran without a
propagation context. The source module was never recorded as visited, so a
propagation that returned to its own source, such as
`SocketRpcServer -> ProjectGraph -> NodeInstances -> SocketRpcServer`, could not
be detected.

## Superseded section from "Enforcement"

### Runtime propagation context

Debug builds thread a **propagation context** through event invocations. The
context is created when a root event is raised outside any active propagation,
and inherited by every event raised as a consequence:

```cpp
struct PropagationContext {
    EventId root_event;
    std::unordered_set<ModuleId> visited_modules;
    // causal trace entries
};
```

Each subscriber dispatch marks its module visited; raising a further event
inside a handler reuses the same context rather than creating a new one. The
invariant enforced dynamically:

> **Within one causal propagation, a module may be reached at most once.**

Tracking the *set of modules visited during the whole propagation* — not the
currently active call stack — is what makes sibling convergence visible.
Synchronous sequential dispatch (`root -> B -> D -> back -> C -> D`) would
leave `D` off the stack by the time `C` raises into it; the visited set still
contains it, so the violation is caught.

The context travels via the invocation mechanism: `IV_INVOKE_LINKER_EVENT` and
its variants initialize or inherit the context around subscriber dispatch, and
`IV_INVOKE_LINKER_EVENT_SOURCE` marks a member as a propagation root. A
thread-local current context is sufficient for synchronous propagation and
adds no cost in release builds, where all of this compiles out.

Because the check observes the branch that actually executed, conditions are
handled naturally: `if (foo) raise(A); else raise(B);` never produces a false
collision.

## Superseded sections from "Runtime mechanism design" to the end

## Runtime mechanism design

The runtime check is implemented implicitly and thread-locally: event
signatures do not change, and modules never pass provenance objects manually.
Provenance is an execution concern of the event mechanism, not part of module
APIs.

### Public API change

Exactly one primitive is added; the existing invocation macro keeps its name
and meaning:

```text
IV_INVOKE_LINKER_EVENT_SOURCE(event_name, ...)
    - must be called with no linker event being dispatched on this thread
    - starts a new propagation context
    - raises the event
    - destroys the context when the synchronous propagation returns

IV_INVOKE_LINKER_EVENT(event_name, ...)
    - raises an ordinary event
    - if a propagation context exists, inherits it
    - otherwise behaves as it does today
```

The subscriber function type is unchanged: subscribers remain plain
`void(Args...)`. In particular the event context is not injected as a first
parameter; that would infect every module API with an execution concern.

`IV_DECLARE_LINKER_EVENT`, `IV_DEFINE_LINKER_EVENT`, and the linker-section
representation are unchanged. The section still holds subscriber function
pointers; the templated bridge function already knows the concrete subscriber
type at exactly the point where module identity is needed, so there is no
reason to widen the section's ABI with metadata.

### Two thread-local concepts

```cpp
thread_local event_stack;            // every event invocation, tracked or not
thread_local propagation_context*;   // only underneath a marked source
```

The event stack exists for every invocation — including currently untracked
ones — because nesting is the fact that decides whether a raise can be a root:

> Calling `IV_INVOKE_LINKER_EVENT_SOURCE` while any event invocation is active
> on this thread is an error — even if the enclosing event is itself
> untracked.

An untracked enclosing event is still a synchronously preceding application
cause, so a source nested under it is not a source. The check is therefore
against the event stack, not merely against the propagation pointer:

```text
IV_INVOKE_LINKER_EVENT(A)
    -> subscriber
       -> IV_INVOKE_LINKER_EVENT_SOURCE(B)   // error: nested under A
```

### Invocation mechanics

The macro bodies move into internal helpers so RAII can maintain state and
exceptions cannot corrupt it:

```text
IV_INVOKE_LINKER_EVENT:   push event frame -> dispatch subscribers -> pop
IV_INVOKE_LINKER_EVENT_SOURCE:
    assert event stack empty and no active propagation
    create propagation context
    push event frame -> dispatch subscribers -> pop
    destroy context
```

Subscriber wrappers record module entry. The bridge subscriber template
(`iv_bridge_subscriber<Bridge, Member>::invoke`) knows the concrete subscriber
class, which is the module identity for the visited-set check; the dispatch
becomes:

```text
check/record module visit -> push module frame -> member body -> pop
```

### Module identity

Module identity is type identity: the subscriber's participant class. This is
consistent with the bridge model, where a participant is a concrete type and
bridges connect types, not instances. If two semantically distinct module
instances of the same C++ type ever need distinguishing, identity would switch
to something instance-based — but type identity matches the architecture
today.

### Provenance traces

Detection needs only `visited_modules.contains(module)`, but the diagnostic
needs the path. The live path is tracked as alternating frames:

```text
[event A] [module B] [event C] [module D] ...
```

On a violation, the first visit's retained path snapshot plus the current path
yield a full two-path report:

```
event propagation re-entered module ProjectGraph

source:
    project mutation source

first entry:
    source event
      -> ProjectGraph::handle_first_change
      -> derived event A
      -> ConsumerA::handle_change

second entry:
    source event
      -> ConsumerB::handle_change
      -> derived event B
      -> ProjectGraph::handle_second_change
```

The runtime checker therefore doubles as an architecture debugger: every
failure reports the exact subscriber -> raise chains that collided.

### Migration of unmarked roots

Initially:

```text
IV_INVOKE_LINKER_EVENT_SOURCE          checked propagation
IV_INVOKE_LINKER_EVENT under a source  checked propagation
IV_INVOKE_LINKER_EVENT at top level    today's behavior + event stack only
```

Once every control source is annotated, the natural tightening is: a
top-level `IV_INVOKE_LINKER_EVENT` (event stack empty, no propagation) becomes
a debug error demanding the source variant. This prevents new control sources
from silently bypassing the invariant, and makes the semantic distinction
load-bearing:

```cpp
IV_INVOKE_LINKER_EVENT_SOURCE(E, ...)  // external/new cause
IV_INVOKE_LINKER_EVENT(E, ...)         // consequence of the current cause
```

Neither macro is interchangeable inside a subscriber.

### Asynchronous boundaries

The nested-source rule is unconditional in debug builds: a source nested under
any active event invocation fails, because "source" means *no preceding
application-level event cause on this thread*, and a handler on the stack
demonstrably contradicts that.

The sanctioned escape is a deliberately defined cause-breaking boundary. Work
scheduled for later (task runner, deferred queue) executes when the event
stack is empty, so a source invocation there is genuinely a new cause and the
model accepts it naturally. If queued work must instead retain the original
cause, that requires an explicit movable propagation token; that mechanism
should not be built until a concrete requirement exists.

## Implementation notes

- `linker_event.h`: thread-local event stack, propagation context, RAII scopes
  around both invocation macros, `IV_INVOKE_LINKER_EVENT_SOURCE` primitive;
- `bridge.h`: module-entry guard in `iv_bridge_subscriber::invoke`, which
  already holds the participant type for the visited-set check;
- no changes to `IV_DECLARE_LINKER_EVENT` / `IV_DEFINE_LINKER_EVENT`, the
  subscriber function type, or the linker-section ABI;
- all instrumentation compiles out in release builds;
- debug failures produce the two-path causal trace with source location;
- a post-build static filter, if pursued later, can read constexpr metadata
  emitted into linker-set sections by the bridge and raise macros, merged
  across translation units at link time; it is an optimization, not a
  prerequisite;
- any static-check CMake integration should live in a self-contained directory
  imported by the parent `CMakeLists.txt`.
