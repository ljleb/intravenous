# Earlier move-only NodeRef contract

_Historical, non-normative. Preserved on 2026-11-23 from the
[Node-Reference DSL Contract](../node_ref_dsl_contract.md#contexts-and-identities).
Commit `e4554e75` enabled copy construction. The current contract describes
copies and records named occurrences for output forwarding._

> A **node reference** is a move-only builder-time value. It refers to exactly
> one node bundle in one `GraphBuilder` and creates or inspects graph wiring.

> `NodeRef` is the move-only common base and contains an erased bundle handle.

> Typed refs derive from `NodeRef`, but an erased handle for an lvalue is made by
> `node_ref()`, not by copying or slicing a node ref.
