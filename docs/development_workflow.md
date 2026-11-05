# Design, Implementation, And Documentation Workflow

_Status: normative repository process for collaboration, implementation sequencing,
friction reporting, and preservation of design intent._

This repository is co-authored by a human designer/programmer and coding agents. The
process must preserve the feedback that was previously available when one person held
the whole implementation in working memory. In particular, delegating implementation
must not allow implementation convenience to gradually redefine the intended
application.

The central rule is:

> The intended destination, the observed implementation, and implementation friction
> are different kinds of information. Record all three, but never silently substitute
> one for another.

## 1. Sources of truth have different roles

### 1.1 Normative design

Normative design documents describe the application that is intended to exist. They
govern semantic behavior, ownership, public interfaces, concurrency boundaries, and
other architectural invariants even when the checked-in implementation is incomplete
or contradicts them.

An implementation that cannot yet satisfy a normative statement is evidence about the
implementation or a friction signal. It is not, by itself, evidence that the normative
statement should be changed.

### 1.2 Current implementation

Source code and tests describe what currently exists. They are evidence for an
implementation-status report, not retroactive evidence of original intent.

Tests demonstrate behavior covered by those tests. Existing behavior is not
automatically a compatibility requirement when it is scaffolding, an incomplete
migration, or contrary to the normative design.

### 1.3 Implementation coordination and status

Roadmaps, implementation-front documents, audits, and status notes describe the path
between current source and normative design. They may record incomplete work,
dependency order, known costs, and implementation discoveries. They must not acquire
normative semantic authority merely because they describe source in greater detail.

### 1.4 Historical material

`docs/historical/` preserves superseded designs, removed explanations, completed
migration records, and earlier versions of semantic text. Historical material retains
rationale and provenance but is non-normative.

## 2. Collaborating actors

### 2.1 Human co-author

The human co-author supplies product intent, architectural judgment, and subjective
friction judgment. Because implementation may be delegated, the human may no longer
see every local constraint that would have been discovered while personally writing
the code. Coding agents must therefore surface those constraints rather than resolving
them invisibly.

### 2.2 Coding agent

A coding agent owns implementation work it accepts, including understanding the
relevant normative documents, current source, dependency prerequisites, and affected
downstream work. The agent is expected to use that context in service of the complete
feature, not to optimize for the smallest compiling diff.

The agent must distinguish:

- a requirement stated by normative design;
- an implementation fact observed in source;
- an assumption made to proceed;
- a newly discovered friction signal;
- a proposed design change; and
- a temporary defect inside an unfinished feature.

A coding agent has several recurring failure modes that this process must actively
counter:

- **local-completion bias:** finishing the currently visible function despite an
  architectural contradiction outside it;
- **compilation bias:** treating compiler success as the objective and introducing an
  interface or adapter that has no place in the intended design;
- **incremental-diff bias:** preferring a sequence of locally simple changes that
  collectively establish the wrong abstraction;
- **premature-decomposition bias:** creating clean classes around behavior that has not
  yet been understood end to end;
- **documentation-rationalization bias:** changing stated intent to agree with source;
  and
- **context-loss risk:** forgetting that surviving code was temporary after a context
  compaction or work handoff.

The safeguards below are intended to correct those failure modes. They are not a
requirement for the agent to imitate the human co-author's private thought process.

### 2.3 Compiler and tests

The compiler and tests are verification tools used at useful boundaries. They do not
dictate implementation granularity. Keeping every intermediate commit compiling is not
a project goal.

Compilation is also a useful discovery tool. An agent may compile an intentionally
unfinished feature to enumerate affected call sites, expose invalid assumptions, or
validate a completed lower-layer milestone. It must not add compatibility code merely
to make that diagnostic build pass, and compiler success does not turn an incomplete
milestone into a complete feature.

### 2.4 Information and handoff model

The collaborators have different direct information and different blind spots:

| Actor | Information directly available | Information that must not be inferred |
| --- | --- | --- |
| Human co-author | Product intent, architectural priorities, earlier design rationale, subjective friction judgment | Every local constraint discovered in delegated source work |
| Coding agent | Current source, current documentation, repository history made available to it, compiler/test diagnostics, and the dependency/call graph it inspects | Unwritten intent, or permission to turn an implementation fact into a design decision |
| Compiler and tests | Mechanical type, linkage, runtime, and asserted-behavior results | Architectural desirability, completeness of untested semantics, or product intent |
| Normative documents | Durable accepted destination and terminology | Whether source has implemented the destination |
| Status/friction records | Durable observations about the path from source to destination | Authority to weaken or replace the destination |

The normal collaboration loop is therefore:

```text
human intent + normative documents
              |
              v
agent compares destination with current source
              |
              +---- no unexpected friction ----> implement complete feature
              |
              `---- unexpected friction --------> report reach and alternatives
                                                        |
                                                        v
                                              human/agent design decision
                                                        |
                                                        v
                                         update destination and dependency graph
                                                        |
                                                        v
                                              resume complete feature
```

The friction report is the substitute for implementation feedback the human would
have received while personally writing the affected code. Omitting it deprives the
human co-author of information needed to make the architectural decision.

## 3. Feature implementation method

### 3.1 Select one complete, dependency-ready feature

Before editing source:

1. identify the complete semantic feature being implemented;
2. identify the normative documents that define it;
3. compare that destination with the current source;
4. identify prerequisite features and confirm that they are complete enough to depend
   on; and
5. update the remaining-work dependency graph if the comparison reveals a different
   order.

Do not depend on a knowingly provisional lower layer as though its contract were final.
Either complete the prerequisite first or expand the current feature boundary to
include the prerequisite and every affected layer. Some neighboring layers can only be
designed correctly together; a nominal layer boundary must not force them into
separate features.

Do not alternate between unrelated incomplete features merely to produce small passes.

### 3.2 Implement the whole feature

Implementation should advance directly toward the final design:

- remove or reshape obstructing APIs and representations when the feature requires it;
- allow the repository to have compiler errors while the coherent change is underway;
- do not introduce compatibility wrappers, duplicate APIs, synchronous substitutes,
  fake implementations, or other scaffolding solely to preserve intermediate builds;
- do not split work into tiny diffs whose only completion criterion is compilation; and
- continue through all affected layers required for the feature's semantic contract.

A useful pass is a complete feature or a genuinely independent lower-layer primitive,
not an arbitrary number of files or tests.

Internal implementation milestones may exist inside that pass. They are useful for
reasoning, compiler diagnostics, and handoff, but they must be described as incomplete
parts of the same feature rather than independent delivered features.

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

### 3.4 Write tests after the implementation shape is known

Do not force a speculative test API before the implementation has established its real
interfaces and invariants. After the feature is implemented:

1. test its externally meaningful semantics;
2. test important ownership, failure, concurrency, and transition boundaries;
3. add regression tests for friction or defects discovered during implementation; and
4. refactor the implementation with those tests as verification.

Focused experiments used to understand an existing behavior are allowed, but they are
not a requirement to design the final implementation test suite in advance.

Investigative tests and temporary reproductions are specifically allowed before or
during implementation when they answer a factual question about current behavior,
ordering, ownership, failure, concurrency, or data loss. Their purpose is observation,
not invention of the future public interface. Retain them as regression tests only when
they express a meaningful final invariant; otherwise remove or archive the experiment
when the question is resolved.

### 3.5 Preserve unfinished-feature context

An intentionally broken working tree is safe only while its feature boundary and
unfinished state remain recoverable. Before a context compaction, handoff, or other
interruption that may lose working memory, record in the relevant active status note or
handoff:

- the one feature currently being implemented;
- the normative destination and affected layers;
- which internal milestones are complete and incomplete;
- every unresolved assumption or friction signal;
- expected compiler errors and the incomplete layer that owns them; and
- the next bottom-up connection required to continue.

Do not preserve this context by adding compatibility scaffolding to the source. Remove
the handoff record or move it to `docs/historical/` when the feature is complete, while
retaining any friction or design decision that remains useful.

## 4. Friction is architectural input

An implementation friction signal is a surprising constraint or requirement that was
not represented in the plan and that would avoidably increase one or more of:

- time complexity, especially work on a realtime or publication boundary;
- memory complexity or retained lifetime;
- dependency complexity, including strong class/function coupling;
- duplicated ownership or synchronization;
- semantic special cases in otherwise homogeneous interfaces;
- migration, compatibility, or state-transition complexity; or
- the amount of future code that must understand a temporary representation.

Difficulty alone is not friction. A necessary consequence of an explicitly chosen
semantic requirement may simply be the cost of the feature. Friction is evidence that
the selected decomposition or an earlier assumption may be wrong.

### 4.1 Required response to friction

When meaningful friction appears, do not immediately work around it. Pause the feature
and report:

1. **Planned assumption:** the design or dependency assumption under which work began.
2. **Observed constraint:** the concrete source/runtime fact that conflicts with it.
3. **Complexity introduced:** the time, memory, dependency, or semantic cost of
   continuing unchanged.
4. **Reach:** every affected interface, owner, execution stage, document, and remaining
   feature known at that point.
5. **Alternatives:** including the broader redesign that removes the friction, not only
   the smallest local workaround.
6. **Recommendation:** which alternative best preserves the complete intended system
   and why.
7. **Dependency-graph change:** how the remaining implementation order changes if the
   recommendation is accepted.

Do not encode a far-reaching design choice merely because one option is easiest to
make compile. Resume implementation after the design decision is understood.

### 4.2 Friction records

Record unresolved friction in the relevant implementation-status or roadmap document,
clearly labeled as an observation rather than a requirement. When resolved, retain the
observation and add the decision and consequence. If the whole record no longer helps
active implementation, move it to `docs/historical/`; do not erase it.

### 4.3 Friction imposed by this workflow

This workflow is an instrument for preserving design quality and collaboration signal;
it is not beyond revision. Either the human co-author or a coding agent may discover
that a process rule itself creates surprising delay, information loss, repeated work,
dependency complexity, or pressure toward worse code.

The collaborator who notices that friction must bring it to the other before silently
working around the rule. Together, classify the friction as one of:

- **accidental process friction:** the rule obstructs the intended work without
  producing information or protection worth its cost; or
- **useful process friction:** the rule deliberately slows or interrupts work in order
  to expose an architectural decision, preserve intent, prevent scaffolding, recover
  context, or protect another identified invariant.

For accidental process friction, change the workflow. Preserve superseded semantic
process text according to the documentation mutation protocol and state what failure
the replacement avoids.

For useful process friction, record why the friction is valuable, what signal or
invariant it protects, and the conditions under which that judgment should be revisited.
Do not merely tolerate unexplained inconvenience and call it discipline.

A workflow experiment may be adopted provisionally when neither classification is yet
clear. State the expected benefit, observable cost, and review point. Evidence from the
experiment may then justify retaining, revising, or removing the rule without being
misrepresented as a product-architecture decision.

## 5. Documentation mutation protocol

Documentation changes must add information. They must not make history less
recoverable or turn an implementation compromise into apparent original intent.

### 5.1 Classify information before writing

Every material documentation change must be classified as one of:

- normative design intent;
- implementation status;
- implementation friction;
- proposed design change;
- accepted design change; or
- historical context.

Do not mix these categories in one unlabelled statement. In particular, phrases such
as “the design does X” are inappropriate when only the current source does X.

### 5.2 Updating normative design

Normative design changes require an explicit design decision. Successful implementation
alone is not such a decision.

When a decision adds detail without contradicting prior intent, add the clarification
and identify it as a newly settled decision where ambiguity would otherwise remain.

When a decision removes, contradicts, or supersedes semantic text:

1. preserve the prior text or prior complete document under `docs/historical/`;
2. include provenance: the source document, preservation date or revision, why it was
   superseded, and the current replacement document;
3. update the current normative document with the accepted replacement;
4. include a brief supersession link when the removed alternative contains rationale
   that remains useful; and
5. update dependent roadmaps and implementation fronts without presenting the new
   decision as though it had always been the design.

### 5.3 Updating implementation status

Implementation-status text may be freely extended to describe landed or missing code,
but it must remain subordinate to normative design. Status updates should say:

- which behavior is implemented;
- which behavior is partial or absent;
- where source currently diverges from the intended destination;
- which friction signals are unresolved; and
- which dependency-ready feature should be implemented next.

Do not weaken a normative contract to make a status report look complete.

### 5.4 Deleting documentation

Do not discard semantic information. Before deleting a document or a material section,
move or copy it into `docs/historical/` and mark its status and replacement. Purely
mechanical corrections that preserve meaning do not require a historical snapshot.

When unsure whether an edit is semantic, preserve the old text.

### 5.5 Retrospective corrections

If current normative documents already contain implementation-derived compromises,
do not guess at the original intent and silently rewrite them. Identify the suspected
drift, compare available history and design discussion, and ask the human co-author to
resolve the intended contract. Preserve both the previous wording and the accepted
correction.

## 6. Reporting work

Status reports should be hierarchical and should distinguish:

1. complete semantic features;
2. partial features and their exact missing behavior;
3. correctness blockers;
4. performance or maintainability work that does not change semantics;
5. observed friction and proposed design changes; and
6. the dependency-ordered next complete features.

Every bullet should name what is being allocated, reclaimed, migrated, published,
recorded, or otherwise changed. Avoid context-free descriptions such as “allocation
and reclamation remain separate.”

When handing off unfinished work, state explicitly whether the repository is expected
to compile and which incomplete feature owns any expected errors.

## 7. Completion criteria

A feature is complete only when:

- its complete semantic path is implemented across all required layers;
- known dependency surfaces no longer rely on scaffolding for that feature;
- discovered friction has been resolved or explicitly accepted as part of the design;
- the bottom-up implementation has been refactored into understandable stable
  responsibilities;
- feature tests cover the meaningful semantics and boundary failures; and
- implementation-status documents describe the result without changing normative
  intent.

Passing compilation and tests is evidence used by this definition, not a replacement
for it.
