# Historical Temporary-Adapter Coordination Policy

_Status: superseded repository-process text preserved from
`docs/implementation_fronts_direction.md` on 2026-10-13. It is retained for rationale
and is not current implementation guidance._

The former fifth coordination principle read:

> **Prefer explicit integration contracts over shared ownership.** A team may add a
> temporary adapter to the current representation rather than taking ownership of a
> neighboring front merely to make progress.

This permitted incomplete neighboring representations to be bridged so another front
could continue. In practice, temporary compatibility code can obscure the real feature
dependency and can survive long enough to be mistaken for intentional architecture.

The replacement policy is in
[`development_workflow.md`](../development_workflow.md) and the current
[`implementation_fronts_direction.md`](../implementation_fronts_direction.md): depend
on a completed explicit contract, or report the representation mismatch as friction
and change the dependency order or design.

