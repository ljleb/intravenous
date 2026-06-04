# Task Runner Execution Direction

This note captures the current intended direction for runtime execution task
ordering.

It should be preferred over older execution wording in
[execution_model_direction.md](./execution_model_direction.md) and
[timeline_lanes_design.md](./timeline_lanes_design.md) where the documents
disagree. Those documents still matter for runtime ownership, lane semantics,
and broader timeline direction. This document is specifically about the task
runner, task graph updates, and execution planning.

## Scope

The task runner is mainly for `Timeline` and `IvModuleInstances`.

It is not intended to become a generic one-shot job system for every async
operation in the repo. Existing module-owned threads such as JSON-RPC handling
or iv-module reload watching can continue to do their own event work directly.

The task runner exists to maintain and execute a persistent DAG of runtime tasks
that need explicit ordering.

## Core execution model

- Tasks run once per execution pass.
- The active task graph is acyclic.
- The runner repeatedly executes the entire active graph in a tight loop.
- Nodes themselves always generate sound when executed.
- Play/pause state does not live inside task nodes.
- `Timeline` owns play/pause separately by controlling the current index being
  sampled.

## Task graph updates

Task registration is persistent across passes.

The public API should stay small. The main operation is an update call that
applies a delta to the declared graph.

The update shape should support:

- task creation
- task deletion
- task updates, including dependency replacement and callback replacement

A useful shape is:

```cpp
struct TaskCallback {
    void (*invoke)(void *) = nullptr;
    void *context = nullptr;
};

struct TaskRecord {
    std::string id;
    std::vector<std::string> depends_on;
    TaskCallback callback;
};

struct TaskUpdateRecord {
    std::string id;
    std::optional<std::vector<std::string>> depends_on;
    std::optional<TaskCallback> callback;
};

struct TaskGraphUpdate {
    std::vector<TaskRecord> to_create;
    std::vector<std::string> to_delete;
    std::vector<TaskUpdateRecord> to_update;
};
```

`depends_on` is the only declared relationship form. Reverse "users" are
derived internally by the runner as needed.

Any module may update dependencies on tasks it did not create. The important
thing is consistent declaration of dependencies, not ownership of both tasks by
the same module.

Within a single `TaskGraphUpdate`, a task id may appear exactly once across all
create, delete, and update entries. Duplicate mentions are a hard error.

Creating an already-existing task is also a hard error. This is intentional:
trying to recreate an existing task indicates that some part of the code has
lost track of the currently declared graph, and allowing that would invite
subtle bugs.

## Update semantics

The active graph should never be mutated in place.

Instead:

- updates produce a successor graph version
- the current execution pass continues using its captured active version
- at the end of the pass, the runner atomically swaps the active graph pointer
- the next pass begins from the new active version

If a task is currently running while a deletion is requested, that task is
allowed to finish in the current pass. It must not be scheduled again once the
next graph version becomes active.

Unrelated tasks should continue executing while updates are being prepared.

This is meant to allow small and moderate graph updates to happen seamlessly
without globally blocking execution.

Updates should be prepared under a lock.

If a newer update arrives while a successor graph is already pending, the newer
update should be applied to the latest pending graph version rather than to the
currently active version. The runner should always swap to the latest ready
graph version at the end of a pass.

This is effectively a latest-ready-version model, similar in spirit to triple
buffering.

## Validation rules

Unresolved dependencies are a hard error.

The published graph version must:

- contain all referenced dependencies
- remain acyclic

Broken dependency declarations should fail fast during graph publication rather
than creating dormant tasks with unclear semantics.

Deleting a task should automatically remove dependency references to it in the
successor graph.

Updating a missing task is also a hard error.

Deleting a missing task should also be treated as a hard error for the initial
implementation. The goal is to catch graph-ownership mistakes early.

## Declared graph versus execution plan

The runner should keep two internal representations:

- the declared task graph
- a compiled execution plan derived from that graph

The declared graph is the semantic source of truth. Updates apply there.

The compiled execution plan is an optimized runtime representation built from
the declared graph before execution.

## Initial merge strategy

For the initial implementation, execution-plan merging should stay conservative
and obvious.

The runner should merge only strictly linear paths where:

- task `B` depends on task `A`
- `A` has exactly one user, `B`
- `B` has exactly one dependency, `A`

This keeps the first implementation simple and avoids introducing more complex
ordering requirements inside merged runs.

Anything more complicated should remain as separate tasks until there is a
strong reason to optimize further.

## Callback strategy

The task callback mechanism should avoid `std::function`.

Fine-grained declared tasks are useful for expressing dependencies precisely,
but `std::function` adds unnecessary dynamic-dispatch and possible allocation
overhead for this use case.

The preferred initial direction is a cheap callback representation such as:

- function pointer
- opaque context pointer

This keeps callback dispatch small, avoids heap-heavy type erasure, and gives
the compiled execution plan a better chance of preserving cache locality.

Exactly one module should own the callback and callback context for a given task
id. That same module is responsible for ensuring that the callback context
remains valid for as long as any active or pending graph version may still
reference it.

## Cross-module dependencies

Cross-module task ordering is expected and allowed.

For example, `GraphInputLanes` may need to declare dependencies between its own
lane-related tasks and other timeline or iv-module-instance tasks even when it
did not create all of those tasks itself.

This is important for implicit ordering relationships that are not visible from
ordinary lane connectivity alone.

One future example is DSP graph output lanes:

- a DSP graph may eventually expose timeline-facing input lanes
- it may also expose timeline-facing output lanes
- the input and output lane tasks may require explicit ordering even when the
  timeline graph does not show a direct edge between them

The task runner should not care which module introduced the dependency. It only
needs the declared DAG to be correct.

## Non-goals

The task runner does not need to provide:

- a generic one-shot task abstraction
- resource-locking logic
- ownership of mutexes or atomics for shared state

If shared state needs synchronization, that synchronization should live with the
owning resource or module.

The task runner is responsible for task ordering and execution planning, not
for replacing ordinary local thread-safety mechanisms.

## Worker-pool scheduling

The runner should use a worker pool and execute ready tasks as fast as it can.

Dependency ordering is the rule:

- a task may run only after all of its dependencies have completed for the
  current pass

When the runner has a choice among multiple ready tasks, it should use a simple
priority heuristic:

- prefer tasks with more users first
- apply a stable secondary tie-breaker after that

User count is only a ready-queue heuristic. It must not affect dependency
semantics or correctness.

## Destruction

Destruction should drive shutdown.

The runner should not expose a separate invalid "shut down but still alive"
state. Destroying the object is equivalent to deleting all tasks and waiting
for the current DAG execution pass to complete.

The runner should not tear down the currently executing pass mid-flight.
