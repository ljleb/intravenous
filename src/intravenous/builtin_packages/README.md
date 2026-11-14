# Shipped IV Packages

Packages in this directory are source packages shipped with Intravenous. The `builtin`
package is staged into the build tree and discovered through the application's default
package search root. Its siblings are optional packages: they are not staged, built,
or loaded merely by building or starting Intravenous.

For a source-tree run, add an optional package's own directory to
`IV_MODULE_SEARCH_PATH` to select it for discovery. A selected package controls its own
native dependencies through its package-local CMake project.
