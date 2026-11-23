# Historical logical package namespace proposal

Non-normative. Preserved from `docs-dev/package_pipeline_architecture.md` on
2026-11-16, before the owner selected longest-prefix ownership and required-
closure-first compilation followed by a separate catalog pass. Replaced by
[static package selection](../package_pipeline_architecture.md#logical-package-namespaces-and-static-selection).

### Proposed logical package namespaces (2026-11-15; not implemented)

Future packages may have stable logical identifiers usable as prefixes for
public `IV_NODE`/`IV_MODULE` definitions. These are distinct from the already
implemented discovery provenance (`built_in`, `common`, `project_local`) and
build artifact/cache identity. This proposal should extend GraphBuilder's
existing `builtin` lookup and planned namespace imports, not silently
replace them. Neither an `iv_package.json` field spelling nor collision/
import precedence was finalized in the discussion. See
[Node Definitions And Instances](./node_definitions_and_instances_direction.md#future-package-identity-publication-and-type-construction-2026-11-15-clarification).


## Related earlier wording from `node_definitions_and_instances_direction.md`

The existing exact-ID lookup and implicit `builtin.<id>` fallback above are
**implemented behavior**. The planned import mechanism should **extend** that
behavior, treating `builtin` as the default import instead of replacing it
with an unrelated special resolver. A future stable logical package identifier
may supply the namespace prefix for canonical public definition IDs, and the
owning package's namespace may be imported automatically in its builders.
Import collisions and precedence remain subject to the previously recorded
namespace-import design; no manifest syntax or exact resolution rule is
selected here. Package identity for namespacing must not be confused with
filesystem provenance or build-artifact identity.
