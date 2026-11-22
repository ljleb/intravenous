# Refactoring sequence before the build handoff review

On 2026-11-21, the owner agreed that each feature needs a refactoring
assessment before build handoff. The assessment can happen at any point
during the work. This archive preserves the former sequence from
`docs/development_workflow.md` and `AGENTS.md`. The
[current workflow](../development_workflow.md#33-assess-refactoring-before-build-handoff)
sets a deadline for the assessment and gives the implementation room to
reveal where a refactor is useful.

### 3.3 Prefer bottom-up discovery, then refactor top-down

Before editing, inspect enough of the existing call graph, ownership graph, and
normative data flow to identify the real feature boundary. Structural inspection is a
strength of a coding agent and should reduce accidental omissions; it must not become
premature top-down class design.

When the final internal decomposition is not yet known, implement the behavior in the
most direct local form. It is acceptable for the initial working implementation to use
one large function or tightly connected local code while relationships are being
discovered.

One large function is permitted, not required. The rule is to avoid freezing uncertain
responsibilities behind abstractions. If existing stable ownership boundaries already
match the complete feature, using them directly is preferable to manufacturing a
monolith solely to mimic another programmer's working style.

Once the complete behavior works:

1. identify the actual stable responsibilities and data flow;
2. extract functions, classes, and ownership boundaries around those facts;
3. remove temporary duplication created inside the feature implementation;
4. verify that the refactoring did not introduce additional dynamic work or coupling;
   and
5. keep the base interface homogeneous where semantics are homogeneous while using
   static information to erase costs for configurations that do not use a feature.

Premature top-down decomposition is not a substitute for discovering how the whole
feature needs to connect.

### Former testing and completion rules

The final step in section 3.4 was:

> 4. refactor the implementation with those tests as verification.

Section 7 included this completion criterion:

> - the bottom-up implementation has been refactored into understandable stable
>   responsibilities;

The last collaboration rule in `AGENTS.md` was:

> - write implementation tests after the feature's actual shape is known, then refactor
>   the working bottom-up implementation without changing its semantics.
