# Package dependencies before implicit builtin import

Non-normative. Preserved from `docs-dev/package_pipeline_architecture.md` on
2026-11-16 before the owner clarified that `builtin` is automatically
imported and does not require an authored dependency declaration. Replaced by
[static package selection](../package_pipeline_architecture.md#logical-package-namespaces-and-static-selection).

`PackageWatcher` resolves desired instance/replacement IDs against this manifest
catalog and computes the declared package-dependency closure without executing
package code. It builds the dirty required closure first and publishes its
accepted definitions in one transaction. Explicit dependencies must name exact
package namespaces; a missing dependency is a package failure, never a request
to compile the entire catalog. Authored registered-definition lookups into
another package require that declared dependency. Source/header/library build
dependencies remain the compiler/loader's existing separate watch inputs.
