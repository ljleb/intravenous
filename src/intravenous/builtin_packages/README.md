# Shipped IV Packages

Packages in this directory are source packages shipped with Intravenous. The `builtin`
package is staged into the build tree and discovered through the application's default
package search root. Its siblings are optional packages: they are not staged, built,
or loaded merely by building or starting Intravenous.

For a source-tree run, add an optional package's own directory to
`IV_MODULE_SEARCH_PATH` to select it for discovery. A selected package controls its own
native dependencies through its package-local CMake project.

The default package includes automation points and curves without additional
dependencies. See [the automation guide](builtin/AUTOMATION.md) for the
`iv.builtin.automation.*` definitions and curve-editor construction arguments.

## Beats

`iv.builtin.beats` produces a regular grid at a fixed tempo. Its `beats` output
provides computed Event Coverage. Intravenous generates the marks in each
requested window on the background thread.

The construction arguments are tempo in beats per minute, beats per bar, and
subdivision factors. They default to `120`, `4`, and `[2, 2]`. Each factor
divides the previous interval into equal parts. Use `[2, 2]` for four divisions
per beat, `[3]` for triplets, or `[]` for beats and bars alone. Bar 1 starts at
sample zero. The top-right BPM button in the Beats lane opens these settings.
See the [original origin argument](../../../docs/historical/beats_constructor_origin.md)
for the earlier phase anchor.

Each position has a single `BeatEvent`. Its `depth` is `0` for a bar line,
`1` for another beat, and `2` or higher for successive subdivisions.
A `TriggerEvent` input receives an occurrence at every mark.

The node represents tempo to six decimal places. The subdivision factors can
multiply to at most 1,024. The finest interval must span at least one project
sample. The node rounds a mark to the nearest sample and advances on ties.

Choose **Beats** in the ruler popover to use an editor-managed Beats node as
the project snap grid. To use an authored Beats node or another compatible
source, choose **Custom** and connect its output to the ruler's Beats input.
Seeking and automation point editing snap to the marks visible at the current
zoom level. [Previous Timeline routing](../../../docs/historical/ruler_grid_before_managed_snap_nodes_2026-11-19.md).
